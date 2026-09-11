// SPDX-License-Identifier: GPL-2.0-or-later
#include "Horizon/JitTest.h"
#include <cstring>
#include <exception>
#include <thread>
#include "Common/Arm64Emitter.h"
#include "Horizon/Log.h"

namespace Horizon
{
namespace
{
u32 TestCallback(u32 value) { return value * 3; }
}
bool RunJitEmitterSelfTest()
{
  using namespace Arm64Gen;
  using enum ARM64Reg;
  try
  {
    // Declare child first so the parent invalidates it before child destruction.
    ARM64CodeBlock child, parent, other;
    parent.AllocCodeSpace(8192);
    parent.AddChildCodeSpace(&child, 4096);
    other.AllocCodeSpace(4096);
    const auto* child_entry = child.GetCodePtr();
    child.ADD(W0, W0, 7); child.RET(); child.FlushIcache();
    const auto* other_entry = other.GetCodePtr();
    other.MOVI2R(W0, 91); other.RET(); other.FlushIcache();

    const auto* first = parent.GetCodePtr();
    const auto zero = parent.CBZ(W0);
    parent.ABI_PushRegisters(BitSet32{30});
    parent.BL(child_entry);
    parent.QuickCallFunction(X16, TestCallback);
    parent.ABI_PopRegisters(BitSet32{30});
    parent.RET();
    parent.SetJumpTarget(zero);
    parent.MOVI2R(W0, 42); parent.RET();

    auto* table = parent.AlignCode16();
    parent.ReserveCodeSpace(sizeof(void*));
    std::memcpy(Common::GetWritableJitAddress(table, sizeof(void*)), &child_entry, sizeof(void*));
    const auto* table_entry = parent.GetCodePtr();
    parent.MOVP2R(X1, table);
    parent.LDR(IndexType::Unsigned, X1, X1, 0);
    parent.BR(X1);

    const auto* separate = parent.GetCodePtr();
    parent.ABI_PushRegisters(BitSet32{30});
    parent.QuickCallFunction(X16, other_entry);
    parent.ABI_PopRegisters(BitSet32{30}); parent.RET();
    parent.FlushIcache();
    using Function = u32 (*)(u32);
    const auto check = [&](const char* stage, const u8* entry, u32 input, u32 expected) {
      const u32 actual = reinterpret_cast<Function>(const_cast<u8*>(entry))(input);
      Log("Phocoena JIT self-test", "%s input=%u actual=%u expected=%u", stage, input, actual, expected);
      return actual == expected;
    };
    bool ok = check("conditional fixup", first, 0, 42) &&
              check("child BL + C++ ABI", first, 5, 36) &&
              check("RX literal pointer table", table_entry, 5, 12) &&
              check("separate allocation call", separate, 0, 91);
    auto* patch_address = const_cast<u8*>(child_entry);
    ARM64XEmitter patch(patch_address, patch_address + sizeof(u32));
    patch.ADD(W0, W0, 8); patch.FlushIcache();
    ok &= check("patched instruction + cache", first, 5, 39);
    ok &= check("pointer table after patch", table_entry, 5, 13);
    // Ownership is handed over by thread creation/join. Never patch a function
    // while another thread executes it. Scheduling on distinct physical cores
    // is not guaranteed by this test.
    bool worker_ok = false;
    std::thread worker([&] {
      try
      {
        worker_ok = check("worker executes published code", first, 5, 39);
        ARM64XEmitter worker_patch(patch_address, patch_address + sizeof(u32));
        worker_patch.ADD(W0, W0, 9); worker_patch.FlushIcache();
        worker_ok &= check("worker patches + executes", table_entry, 5, 14);
      }
      catch (const std::exception& error)
      {
        Error("Phocoena JIT self-test", "worker: %s", error.what());
        worker_ok = false;
      }
    });
    worker.join();
    ok &= worker_ok;
    ok &= check("joined worker publishes patch", first, 5, 42);
    other.ClearCodeSpace();
    u32 poison = 0;
    std::memcpy(&poison, other_entry, sizeof(poison));
    ok &= poison == 0xd4200000;
    Log("Phocoena JIT self-test", "%s; emitted ARM64 functions only, not full PPC validation", ok ? "PASS" : "FAIL");
    return ok;
  }
  catch (const std::exception& error)
  {
    Error("Phocoena JIT self-test", "%s", error.what());
    return false;
  }
}
}

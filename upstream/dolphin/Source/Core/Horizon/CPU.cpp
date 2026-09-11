// SPDX-License-Identifier: GPL-2.0-or-later
#include "Horizon/CPU.h"

#include <new>
#include <exception>

#include "Core/PowerPC/Interpreter/Interpreter.h"
#include "Core/PowerPC/JitCommon/JitBase.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "Horizon/Log.h"
#include "Horizon/JitTest.h"
#include "Horizon/Fastmem.h"

namespace Horizon
{
CPUCoreBase* InitializeCPUCore(Core::System& system, PowerPC::CPUCore requested)
{
  if (requested == PowerPC::CPUCore::JITARM64)
  {
    Log("CPU init", "BEGIN experimental upstream AArch64 JIT; 256 MiB CodeMemory; "
        "canonical RX addresses; horizon_ppc_fastmem=%s",
        IsPpcFastmemRequested() ? "requested" : "off");
    if (RunJitEmitterSelfTest())
    {
      try
      {
        if (auto* core = system.GetJitInterface().InitJitCore(PowerPC::CPUCore::JITARM64))
        {
          Log("CPU init", "active=AArch64 JIT; native JIT=on; fastmem=%s arena=%s; READY",
              IsPpcFastmemArenaActive() ? "on" : "checked fallback",
              IsPpcFastmemArenaActive() ? "mapped" : "unavailable");
          return core;
        }
      }
      catch (const std::exception& error)
      {
        Error("CPU init", "native JIT initialization failed: %s", error.what());
        // Release child code spaces before member destructors, even if Init
        // failed partway through allocating caches or generating helpers.
        if (auto* core = system.GetJitInterface().GetCore()) core->Shutdown();
      }
      system.GetJitInterface().SetJit(nullptr);
    }
    Error("CPU init", "native JIT unavailable or self-test failed; trying Cached Interpreter");
    requested = PowerPC::CPUCore::CachedInterpreter;
  }
  if (requested == PowerPC::CPUCore::CachedInterpreter)
  {
    Log("CPU init", "BEGIN upstream Cached Interpreter; 128 MiB data cache; "
        "native JIT=off fastmem=off large entry map=off");
    try
    {
      // Despite its JitInterface plumbing, this backend caches C++ callbacks and
      // operands in ordinary RW data pages; it does not emit native instructions.
      if (auto* core = system.GetJitInterface().InitJitCore(PowerPC::CPUCore::CachedInterpreter))
      {
        Log("CPU init", "active=Cached Interpreter; native JIT=off; READY");
        return core;
      }
      Error("CPU init", "Cached Interpreter unavailable; falling back to Interpreter");
    }
    catch (const std::bad_alloc&)
    {
      Error("CPU init", "Cached Interpreter allocation failed; falling back to Interpreter");
    }
    system.GetJitInterface().SetJit(nullptr);
  }
  else if (requested != PowerPC::CPUCore::Interpreter)
  {
    Error("CPU init", "refused unsupported native CPU backend %d; falling back to Interpreter",
          static_cast<int>(requested));
  }
  Log("CPU init", "active=Interpreter; native JIT=off; READY");
  return &system.GetInterpreter();
}
}

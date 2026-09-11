// SPDX-License-Identifier: GPL-2.0-or-later
// Exercise the actual upstream cached emitter + CodeBlock<..., false> without
// linking the full PPC core. The callback state below is test data, not a PPC CPU.
#include "Core/PowerPC/CachedInterpreter/CachedInterpreterEmitter.h"
#include "Common/MsgHandler.h"

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>
#include <stdexcept>

namespace PowerPC { struct PowerPCState { u32 value = 0; }; }
namespace
{
bool fail_allocation = false;
unsigned data_allocations = 0, frees = 0, executable_requests = 0;
struct Operands { u32 value; u32 padding = 0; };
s32 Add(PowerPC::PowerPCState& state, const Operands& operands)
{
  state.value += operands.value;
  return sizeof(void*) + sizeof(operands);
}
s32 Stop(PowerPC::PowerPCState&, const void*) { return 0; }
}
namespace Common
{
void* AllocateMemoryPages(size_t size)
{
  if (fail_allocation) return nullptr;
  ++data_allocations;
  void* memory = std::aligned_alloc(4096, size);
  if (memory) std::memset(memory, 0, size);
  return memory;
}
void* AllocateExecutableMemory(size_t)
{
  ++executable_requests;
  throw std::runtime_error("Test attempted executable allocation");
}
bool FreeMemoryPages(void* memory, size_t)
{
  ++frees;
  std::free(memory);
  return true;
}
bool MsgAlertFmtImpl(bool, MsgType, Log::LogType, const char*, int,
                     fmt::string_view, const fmt::format_args&)
{
  throw std::runtime_error("Unexpected upstream assertion in cached emitter test");
}
}

int main()
{
  {
    CachedInterpreterCodeBlock cache;
    cache.AllocCodeSpace(4096);
    const u8* begin = cache.GetCodePtr();
    assert(begin && data_allocations == 1 && executable_requests == 0);
    cache.Write(Add, Operands{42});
    cache.Write(Add, Operands{7});
    cache.Write(Stop);
    assert(!cache.HasWriteFailed());
    // Dispatch pointers to already-compiled C++ functions stored in data pages.
    // We never cast the data page itself to an executable function.
    using Callback = s32 (*)(PowerPC::PowerPCState&, const void*);
    PowerPC::PowerPCState state;
    const u8* cursor = begin;
    unsigned calls = 0;
    while (true)
    {
      Callback callback;
      std::memcpy(&callback, cursor, sizeof(callback));
      ++calls;
      const s32 distance = callback(state, cursor + sizeof(callback));
      if (!distance) break;
      cursor += distance;
    }
    assert(state.value == 49 && calls == 3);
    cache.ClearCodeSpace();
    assert(cache.GetCodePtr() == begin);
    cache.SetCodePtr(cache.GetWritableCodePtr(), cache.GetWritableCodePtr() + sizeof(Callback));
    cache.Write(Add, Operands{99});
    assert(cache.HasWriteFailed());
  }
  assert(frees == 1 && executable_requests == 0);
  fail_allocation = true;
  bool caught = false;
  try
  {
    CachedInterpreterCodeBlock cache;
    cache.AllocCodeSpace(4096);
  }
  catch (const std::bad_alloc&) { caught = true; }
  assert(caught && frees == 1 && executable_requests == 0);
  std::cout << "PASS: actual cached emitter, data allocation, callback dispatch, bounds and OOM; no native code generated (not a PPC execution test)\n";
}

// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
using Result = std::uint32_t;
enum JitType { JitType_SetProcessMemoryPermission, JitType_CodeMemory };
struct Jit
{
  JitType type;
  std::size_t size;
  void* rw_addr;
  void* rx_addr;
  int handle;
};
inline bool R_FAILED(Result result) { return result != 0; }
inline bool R_SUCCEEDED(Result result) { return result == 0; }
Result jitCreate(Jit*, std::size_t);
Result jitClose(Jit*);
void armDCacheFlush(void*, std::size_t);
void armICacheInvalidate(void*, std::size_t);

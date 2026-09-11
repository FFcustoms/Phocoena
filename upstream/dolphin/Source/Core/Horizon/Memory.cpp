// SPDX-License-Identifier: GPL-2.0-or-later
#include "Common/MemoryUtil.h"
#include <cstdlib>
#include <cstring>
#include <new>
#include <malloc.h>
#include <switch.h>
#include "Horizon/Log.h"
#include "Horizon/JitMemory.h"

namespace Common
{
void* AllocateExecutableMemory(size_t size)
{
  return Horizon::AllocateJitMemory(size);
}
// CodeMemory permanently separates RW and RX. Emission stores are translated,
// and the emitter flushes each generated or patched range before execution.
void JITPageWriteEnableExecuteDisable() {}
void JITPageWriteDisableExecuteEnable() {}
void* AllocateAlignedMemory(size_t size, size_t alignment)
{
  if (alignment < sizeof(void*) || (alignment & (alignment - 1)) != 0)
    return nullptr;
  void* ptr = memalign(alignment, size);
  if (!ptr)
    Horizon::Error("Memory", "aligned allocation failed size=%zu alignment=%zu", size, alignment);
  return ptr;
}
void* AllocateMemoryPages(size_t size)
{
  if (size > SIZE_MAX - 4095)
    return nullptr;
  auto* ptr = AllocateAlignedMemory((size + 4095) & ~size_t(4095), 4096);
  if (ptr)
    std::memset(ptr, 0, size);
  return ptr;
}
bool FreeMemoryPages(void* ptr, size_t size)
{
  const auto result = Horizon::FreeJitMemory(ptr, size);
  if (result != Horizon::JitFreeResult::NotJit)
    return result == Horizon::JitFreeResult::Released;
  std::free(ptr);
  return true;
}
void FreeAlignedMemory(void* ptr) { std::free(ptr); }
static bool Protect(void* ptr, size_t size, Permission permission)
{
  const Result rc = svcSetMemoryPermission(ptr, (size + 4095) & ~size_t(4095), permission);
  if (R_FAILED(rc))
    Horizon::Error("Memory", "page permission=%u size=%zu failed: 0x%08x", permission, size, rc);
  return R_SUCCEEDED(rc);
}
bool ReadProtectMemory(void* ptr, size_t size) { return Protect(ptr, size, Perm_None); }
bool WriteProtectMemory(void* ptr, size_t size, bool executable)
{
  return !executable && Protect(ptr, size, Perm_R);
}
bool UnWriteProtectMemory(void* ptr, size_t size, bool executable)
{
  return !executable && Protect(ptr, size, Perm_Rw);
}
size_t MemPhysical()
{
  u64 total = 0;
  if (R_FAILED(svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0)))
    return 0;
  return total;
}
}

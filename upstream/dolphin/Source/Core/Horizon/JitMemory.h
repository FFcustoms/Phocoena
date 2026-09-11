// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>

namespace Horizon
{
// Horizon limits the number of CodeMemory kernel objects much more aggressively
// than ordinary virtual address space. Keep large JIT caches dedicated, but
// suballocate small emitters from a few shared CodeMemory arenas.
inline constexpr std::size_t MAX_JIT_REGIONS = 1024;
inline constexpr std::size_t MAX_JIT_ALLOCATIONS = 2048;
inline constexpr std::size_t SMALL_JIT_POOL_SIZE = 4 * 1024 * 1024;
inline constexpr std::size_t MAX_SMALL_JIT_POOLS = 2;
inline constexpr std::size_t SMALL_JIT_MAX_ALLOCATION = 64 * 1024;

struct JitMemoryStats
{
  std::uint64_t allocations = 0;
  std::uint64_t releases = 0;
  std::uint64_t failures = 0;
  std::uint64_t pooled_allocations = 0;
  std::size_t active = 0;
  std::size_t peak_active = 0;
  std::size_t kernel_regions_active = 0;
  std::size_t kernel_regions_peak = 0;
  std::size_t pooled_pages_active = 0;
  std::size_t pooled_pages_peak = 0;
};

// All public code pointers are RX addresses. Only the emitter's actual stores
// translate to RW. This preserves branch distances, literals and entry pointers.
void* AllocateJitMemory(std::size_t size);
enum class JitFreeResult { NotJit, Released, Failed };
JitFreeResult FreeJitMemory(void* rx, std::size_t size);
JitMemoryStats ReadJitMemoryStats();
void SetSmallJitPoolingEnabled(bool enabled);
bool IsSmallJitPoolingEnabled();
}

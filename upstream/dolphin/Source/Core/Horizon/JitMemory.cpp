// SPDX-License-Identifier: GPL-2.0-or-later
#include "Horizon/JitMemory.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <mutex>
#include <new>
#include <stdexcept>
#include <vector>
#include <switch.h>

#include "Common/MemoryUtil.h"
#include "Horizon/Log.h"

namespace
{
constexpr std::size_t PAGE_SIZE = 4096;

struct Allocation
{
  std::size_t offset = 0;
  std::size_t size = 0;
  bool used = false;
};

struct Region
{
  Jit jit{};
  bool used = false;
  bool pooled = false;
  bool quarantined = false;
  std::size_t active_allocations = 0;
  // Zero means free; otherwise this is allocation-index + 1. This makes every
  // emitter write/patch an O(number of kernel regions) lookup instead of an
  // O(number of vertex formats) allocation-list scan.
  std::vector<std::uint16_t> pages;
  std::vector<Allocation> allocations;
};
std::vector<Region> regions;
std::mutex region_mutex;
Horizon::JitMemoryStats stats;
bool small_pooling_enabled = true;

bool ShouldLogSmallAllocation(std::uint64_t allocation)
{
  return allocation <= 4 || (allocation & (allocation - 1)) == 0;
}

bool Contains(const void* base, std::size_t capacity, const void* pointer, std::size_t size)
{
  const auto b = reinterpret_cast<std::uintptr_t>(base);
  const auto p = reinterpret_cast<std::uintptr_t>(pointer);
  return p >= b && p - b <= capacity && size <= capacity - (p - b);
}

std::size_t AlignSize(std::size_t size)
{
  if (size == 0 || size > std::numeric_limits<std::size_t>::max() - (PAGE_SIZE - 1))
    throw std::bad_alloc();
  return (size + (PAGE_SIZE - 1)) & ~(PAGE_SIZE - 1);
}

std::size_t CountPoolRegions()
{
  return static_cast<std::size_t>(std::count_if(regions.begin(), regions.end(),
                                                [](const Region& region) {
                                                  return region.used && region.pooled &&
                                                         !region.quarantined;
                                                }));
}

bool CloseRegion(Region& region);

Region& OpenRegion(std::size_t size, bool pooled)
{
  auto it = std::find_if(regions.begin(), regions.end(),
                         [](const Region& region) { return !region.used; });
  const bool appended = it == regions.end();
  if (appended)
  {
    if (regions.size() >= Horizon::MAX_JIT_REGIONS)
      throw std::bad_alloc();
    regions.emplace_back();
    it = std::prev(regions.end());
  }

  Region& region = *it;
  const Result result = jitCreate(&region.jit, size);
  if (R_FAILED(result))
  {
    if (appended)
      regions.pop_back();
    Horizon::Error("JIT memory", "jitCreate size=%zu failed: 0x%08x", size, result);
    throw std::bad_alloc();
  }
  region.used = true;
  region.pooled = pooled;
  ++stats.kernel_regions_active;
  stats.kernel_regions_peak = std::max(stats.kernel_regions_peak, stats.kernel_regions_active);
  if (region.jit.type != JitType_CodeMemory || !region.jit.rw_addr ||
      !region.jit.rx_addr || region.jit.rw_addr == region.jit.rx_addr)
  {
    Horizon::Error("JIT memory", "requires simultaneous distinct CodeMemory aliases; libnx type=%d",
                   region.jit.type);
    const Result closed = jitClose(&region.jit);
    if (R_FAILED(closed))
    {
      Horizon::Error("JIT memory", "rejected allocation cleanup failed: 0x%08x", closed);
      region.quarantined = true;
      region.pooled = false;
    }
    else
    {
      --stats.kernel_regions_active;
      region = {};
      if (appended)
        regions.pop_back();
    }
    throw std::bad_alloc();
  }

  if (pooled)
  {
    try
    {
      region.pages.assign(region.jit.size / PAGE_SIZE, 0);
    }
    catch (...)
    {
      const bool closed = CloseRegion(region);
      if (appended && closed)
        regions.pop_back();
      throw;
    }
  }
  Horizon::Log("JIT memory", "%s CodeMemory region size=%zu kernel_active=%zu kernel_peak=%zu RW=%p RX=%p",
               pooled ? "opened pooled" : "allocated dedicated", region.jit.size,
               stats.kernel_regions_active, stats.kernel_regions_peak, region.jit.rw_addr,
               region.jit.rx_addr);
  return region;
}

bool CloseRegion(Region& region)
{
  const Result result = jitClose(&region.jit);
  if (R_FAILED(result))
  {
    Horizon::Error("JIT memory", "jitClose RX=%p failed: 0x%08x; retaining allocation",
                   region.jit.rx_addr, result);
    return false;
  }
  --stats.kernel_regions_active;
  region = {};
  return true;
}

std::size_t FindFreePages(const Region& region, std::size_t pages_needed)
{
  std::size_t run = 0;
  for (std::size_t page = 0; page < region.pages.size(); ++page)
  {
    run = region.pages[page] ? 0 : run + 1;
    if (run == pages_needed)
      return page + 1 - run;
  }
  return region.pages.size();
}

void RecordAllocation(bool pooled, std::size_t pages)
{
  ++stats.allocations;
  ++stats.active;
  stats.peak_active = std::max(stats.peak_active, stats.active);
  if (pooled)
  {
    ++stats.pooled_allocations;
    stats.pooled_pages_active += pages;
    stats.pooled_pages_peak = std::max(stats.pooled_pages_peak, stats.pooled_pages_active);
  }
}

Allocation& AddPoolAllocation(Region& region, std::size_t offset, std::size_t size)
{
  const std::size_t first_page = offset / PAGE_SIZE;
  const std::size_t page_count = size / PAGE_SIZE;
  auto it = std::find_if(region.allocations.begin(), region.allocations.end(),
                         [](const Allocation& allocation) { return !allocation.used; });
  if (it == region.allocations.end())
  {
    region.allocations.emplace_back();
    it = std::prev(region.allocations.end());
  }
  const auto index = static_cast<std::size_t>(std::distance(region.allocations.begin(), it));
  if (index >= std::numeric_limits<std::uint16_t>::max())
    throw std::bad_alloc();
  *it = {offset, size, true};
  std::fill_n(region.pages.begin() + first_page, page_count,
              static_cast<std::uint16_t>(index + 1));
  ++region.active_allocations;
  RecordAllocation(true, page_count);
  return *it;
}

Allocation* FindPoolAllocation(Region& region, const void* pointer, std::size_t size)
{
  if (size == 0 || !Contains(region.jit.rx_addr, region.jit.size, pointer, size))
    return nullptr;
  const auto offset = reinterpret_cast<std::uintptr_t>(pointer) -
                      reinterpret_cast<std::uintptr_t>(region.jit.rx_addr);
  const std::size_t first_page = offset / PAGE_SIZE;
  const std::size_t last_page = (offset + size - 1) / PAGE_SIZE;
  const std::uint16_t owner = region.pages[first_page];
  if (owner == 0 || owner > region.allocations.size())
    return nullptr;
  for (std::size_t page = first_page + 1; page <= last_page; ++page)
    if (region.pages[page] != owner)
      return nullptr;
  Allocation& allocation = region.allocations[owner - 1];
  const auto* allocation_rx = static_cast<const std::uint8_t*>(region.jit.rx_addr) +
                              allocation.offset;
  return allocation.used && Contains(allocation_rx, allocation.size, pointer, size) ?
             &allocation :
             nullptr;
}

Region& Find(void* rx, std::size_t size)
{
  for (auto& region : regions)
  {
    if (!region.used || region.quarantined ||
        !Contains(region.jit.rx_addr, region.jit.size, rx, size))
      continue;
    if (!region.pooled)
      return region;
    if (FindPoolAllocation(region, rx, size))
      return region;
  }
  Horizon::Error("JIT memory", "unregistered/out-of-range RX address=%p size=%zu", rx, size);
  throw std::runtime_error("JIT RX range validation failed");
}

void* Writable(Region& region, void* rx)
{
  const auto offset = reinterpret_cast<std::uintptr_t>(rx) -
                      reinterpret_cast<std::uintptr_t>(region.jit.rx_addr);
  return static_cast<unsigned char*>(region.jit.rw_addr) + offset;
}
}

namespace Horizon
{
void* AllocateJitMemory(std::size_t size)
{
  std::size_t aligned_size;
  try
  {
    aligned_size = AlignSize(size);
  }
  catch (const std::bad_alloc&)
  {
    std::lock_guard lock(region_mutex);
    ++stats.failures;
    throw std::bad_alloc();
  }

  std::lock_guard lock(region_mutex);
  if (stats.active >= MAX_JIT_ALLOCATIONS)
  {
    ++stats.failures;
    Error("JIT memory", "logical allocation limit reached active=%zu limit=%zu", stats.active,
          MAX_JIT_ALLOCATIONS);
    throw std::bad_alloc();
  }

  try
  {
    if (small_pooling_enabled && aligned_size <= SMALL_JIT_MAX_ALLOCATION)
    {
      Region* selected = nullptr;
      std::size_t first_page = 0;
      const std::size_t pages_needed = aligned_size / PAGE_SIZE;
      for (auto& region : regions)
      {
        if (!region.used || !region.pooled || region.quarantined)
          continue;
        first_page = FindFreePages(region, pages_needed);
        if (first_page != region.pages.size())
        {
          selected = &region;
          break;
        }
      }
      if (!selected)
      {
        if (CountPoolRegions() >= MAX_SMALL_JIT_POOLS)
          throw std::bad_alloc();
        selected = &OpenRegion(SMALL_JIT_POOL_SIZE, true);
        first_page = 0;
      }
      const std::size_t offset = first_page * PAGE_SIZE;
      AddPoolAllocation(*selected, offset, aligned_size);
      if (ShouldLogSmallAllocation(stats.pooled_allocations))
      {
        Log("JIT memory", "suballocated size=%zu offset=%zu active=%zu peak=%zu "
            "pool_pages=%zu/%zu kernel_regions=%zu",
            aligned_size, offset, stats.active, stats.peak_active, stats.pooled_pages_active,
            stats.pooled_pages_peak, stats.kernel_regions_active);
      }
      return static_cast<std::uint8_t*>(selected->jit.rx_addr) + offset;
    }

    Region& region = OpenRegion(aligned_size, false);
    RecordAllocation(false, 0);
    return region.jit.rx_addr;
  }
  catch (const std::bad_alloc&)
  {
    ++stats.failures;
    Error("JIT memory", "allocation size=%zu failed active=%zu kernel_regions=%zu pools=%zu",
          aligned_size, stats.active, stats.kernel_regions_active, CountPoolRegions());
    throw;
  }
}

JitFreeResult FreeJitMemory(void* rx, std::size_t size)
{
  std::lock_guard lock(region_mutex);
  for (auto& region : regions)
  {
    if (!region.used || region.quarantined) continue;
    if (!Contains(region.jit.rx_addr, region.jit.size, rx, 1) &&
        !Contains(region.jit.rw_addr, region.jit.size, rx, 1)) continue;
    std::size_t aligned_size;
    try
    {
      aligned_size = AlignSize(size);
    }
    catch (const std::bad_alloc&)
    {
      Error("JIT memory", "refused invalid free address=%p size=%zu", rx, size);
      return JitFreeResult::Failed;
    }

    if (region.pooled)
    {
      Allocation* allocation = FindPoolAllocation(region, rx, 1);
      if (allocation)
      {
        auto* allocation_rx = static_cast<std::uint8_t*>(region.jit.rx_addr) + allocation->offset;
        if (rx != allocation_rx || aligned_size != allocation->size)
        {
          Error("JIT memory", "refused invalid pooled free address=%p size=%zu", rx, size);
          return JitFreeResult::Failed;
        }
        if (region.active_allocations == 1 && !CloseRegion(region))
          return JitFreeResult::Failed;
        if (region.used)
        {
          const std::size_t first_page = allocation->offset / PAGE_SIZE;
          const std::size_t page_count = allocation->size / PAGE_SIZE;
          std::fill_n(region.pages.begin() + first_page, page_count, 0);
          *allocation = {};
          --region.active_allocations;
        }
        stats.pooled_pages_active -= aligned_size / PAGE_SIZE;
        --stats.active;
        ++stats.releases;
        if (stats.active == 0)
          Log("JIT memory", "released final pooled RX=%p size=%zu kernel_regions=%zu", rx,
              aligned_size, stats.kernel_regions_active);
        return JitFreeResult::Released;
      }
      Error("JIT memory", "refused invalid pooled free address=%p size=%zu", rx, size);
      return JitFreeResult::Failed;
    }

    if (rx != region.jit.rx_addr || aligned_size != region.jit.size)
    {
      Error("JIT memory", "refused invalid free address=%p size=%zu", rx, size);
      return JitFreeResult::Failed;
    }
    const std::size_t released_size = region.jit.size;
    if (!CloseRegion(region))
      return JitFreeResult::Failed;
    --stats.active;
    ++stats.releases;
    Log("JIT memory", "released dedicated RX=%p size=%zu active=%zu kernel_regions=%zu", rx,
        released_size, stats.active, stats.kernel_regions_active);
    return JitFreeResult::Released;
  }
  return JitFreeResult::NotJit;
}

JitMemoryStats ReadJitMemoryStats()
{
  std::lock_guard lock(region_mutex);
  return stats;
}

void SetSmallJitPoolingEnabled(bool enabled)
{
  std::lock_guard lock(region_mutex);
  if (stats.active != 0 && small_pooling_enabled != enabled)
  {
    Error("JIT memory", "refused pooling policy change with %zu live allocations", stats.active);
    return;
  }
  small_pooling_enabled = enabled;
}

bool IsSmallJitPoolingEnabled()
{
  std::lock_guard lock(region_mutex);
  return small_pooling_enabled;
}
}

namespace Common
{
void* GetWritableJitAddress(void* rx, std::size_t size)
{
  std::lock_guard lock(region_mutex);
  return Writable(Find(rx, size), rx);
}

void FlushJitCode(void* rx, std::size_t size)
{
  if (size == 0) return;
  std::lock_guard lock(region_mutex);
  auto& region = Find(rx, size);
  // Same ordered cache operations as libnx jitTransitionToExecutable, bounded
  // to the emitted/patched range instead of flushing the entire 256 MiB cache.
  // CodeMemory keeps both aliases mapped; no live dispatcher is unmapped.
  armDCacheFlush(Writable(region, rx), size);
  armICacheInvalidate(rx, size);
}
}

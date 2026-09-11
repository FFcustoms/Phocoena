// SPDX-License-Identifier: GPL-2.0-or-later
#include "Horizon/Fastmem.h"

#include <atomic>

namespace Horizon
{
namespace
{
std::atomic<bool> s_requested{false};
std::atomic<bool> s_arena_supported{false};
std::atomic<bool> s_read_only_supported{false};
std::atomic<bool> s_arena_active{false};

struct AtomicStats
{
  std::atomic<std::uint64_t> compiled_direct_sites{0};
  std::atomic<std::uint64_t> compiled_fallback_sites{0};
  std::atomic<std::uint64_t> compiled_translated_sites{0};
  std::atomic<std::uint64_t> compiled_mmio_sites{0};
  std::atomic<std::uint64_t> runtime_slow_accesses{0};
  std::atomic<std::uint64_t> runtime_mmio_accesses{0};
  std::atomic<std::uint64_t> faults_received{0};
  std::atomic<std::uint64_t> faults_handled{0};
  std::atomic<std::uint64_t> faults_unhandled{0};
  std::atomic<std::uint64_t> mappings_created{0};
  std::atomic<std::uint64_t> mappings_released{0};
  std::atomic<std::uint64_t> mapping_failures{0};
};
AtomicStats s_stats;

std::uint64_t Read(const std::atomic<std::uint64_t>& value)
{
  return value.load(std::memory_order_relaxed);
}
}  // namespace

void SetPpcFastmemRequested(bool requested)
{
  s_requested.store(requested, std::memory_order_release);
  if (!requested)
    s_arena_active.store(false, std::memory_order_release);
}

bool IsPpcFastmemRequested()
{
  return s_requested.load(std::memory_order_acquire);
}

void SetPpcFastmemCapabilities(bool arena, bool read_only_mappings)
{
  s_arena_supported.store(arena, std::memory_order_release);
  s_read_only_supported.store(arena && read_only_mappings, std::memory_order_release);
}

bool IsPpcFastmemArenaSupported()
{
  return IsPpcFastmemRequested() && s_arena_supported.load(std::memory_order_acquire);
}

bool ArePpcFastmemReadOnlyMappingsSupported()
{
  return IsPpcFastmemArenaSupported() &&
         s_read_only_supported.load(std::memory_order_acquire);
}

void SetPpcFastmemArenaActive(bool active)
{
  s_arena_active.store(active && IsPpcFastmemArenaSupported(), std::memory_order_release);
}

bool IsPpcFastmemArenaActive()
{
  return s_arena_active.load(std::memory_order_acquire);
}

bool IsPpcFastmemExceptionRecoveryEnabled()
{
  return IsPpcFastmemRequested() && IsPpcFastmemArenaActive();
}

void ResetPpcFastmemStats()
{
  s_stats.compiled_direct_sites.store(0, std::memory_order_relaxed);
  s_stats.compiled_fallback_sites.store(0, std::memory_order_relaxed);
  s_stats.compiled_translated_sites.store(0, std::memory_order_relaxed);
  s_stats.compiled_mmio_sites.store(0, std::memory_order_relaxed);
  s_stats.runtime_slow_accesses.store(0, std::memory_order_relaxed);
  s_stats.runtime_mmio_accesses.store(0, std::memory_order_relaxed);
  s_stats.faults_received.store(0, std::memory_order_relaxed);
  s_stats.faults_handled.store(0, std::memory_order_relaxed);
  s_stats.faults_unhandled.store(0, std::memory_order_relaxed);
  s_stats.mappings_created.store(0, std::memory_order_relaxed);
  s_stats.mappings_released.store(0, std::memory_order_relaxed);
  s_stats.mapping_failures.store(0, std::memory_order_relaxed);
}

PpcFastmemStats ReadPpcFastmemStats()
{
  return {Read(s_stats.compiled_direct_sites), Read(s_stats.compiled_fallback_sites),
          Read(s_stats.compiled_translated_sites), Read(s_stats.compiled_mmio_sites),
          Read(s_stats.runtime_slow_accesses), Read(s_stats.runtime_mmio_accesses),
          Read(s_stats.faults_received), Read(s_stats.faults_handled),
          Read(s_stats.faults_unhandled), Read(s_stats.mappings_created),
          Read(s_stats.mappings_released), Read(s_stats.mapping_failures)};
}

void RecordPpcMemorySite(PpcMemorySite site)
{
  switch (site)
  {
  case PpcMemorySite::Direct:
    s_stats.compiled_direct_sites.fetch_add(1, std::memory_order_relaxed);
    break;
  case PpcMemorySite::DirectWithFallback:
    s_stats.compiled_fallback_sites.fetch_add(1, std::memory_order_relaxed);
    break;
  case PpcMemorySite::Translated:
    s_stats.compiled_translated_sites.fetch_add(1, std::memory_order_relaxed);
    break;
  case PpcMemorySite::Mmio:
    s_stats.compiled_mmio_sites.fetch_add(1, std::memory_order_relaxed);
    break;
  }
}

void RecordPpcSlowAccess(bool mmio)
{
  if (!IsPpcFastmemRequested())
    return;
  s_stats.runtime_slow_accesses.fetch_add(1, std::memory_order_relaxed);
  if (mmio)
    s_stats.runtime_mmio_accesses.fetch_add(1, std::memory_order_relaxed);
}

void RecordPpcFastmemFault(bool handled)
{
  s_stats.faults_received.fetch_add(1, std::memory_order_relaxed);
  (handled ? s_stats.faults_handled : s_stats.faults_unhandled)
      .fetch_add(1, std::memory_order_relaxed);
}

void RecordPpcFastmemMapping(bool created, bool success)
{
  if (!success)
    s_stats.mapping_failures.fetch_add(1, std::memory_order_relaxed);
  else if (created)
    s_stats.mappings_created.fetch_add(1, std::memory_order_relaxed);
  else
    s_stats.mappings_released.fetch_add(1, std::memory_order_relaxed);
}
}  // namespace Horizon

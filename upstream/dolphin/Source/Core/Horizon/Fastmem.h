// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

namespace Horizon
{
enum class PpcMemorySite : std::uint8_t
{
  Direct,
  DirectWithFallback,
  Translated,
  Mmio,
};

struct PpcFastmemStats
{
  std::uint64_t compiled_direct_sites = 0;
  std::uint64_t compiled_fallback_sites = 0;
  std::uint64_t compiled_translated_sites = 0;
  std::uint64_t compiled_mmio_sites = 0;
  std::uint64_t runtime_slow_accesses = 0;
  std::uint64_t runtime_mmio_accesses = 0;
  std::uint64_t faults_received = 0;
  std::uint64_t faults_handled = 0;
  std::uint64_t faults_unhandled = 0;
  std::uint64_t mappings_created = 0;
  std::uint64_t mappings_released = 0;
  std::uint64_t mapping_failures = 0;
};

// The frontend sets the request before MemoryManager initialization. Capability
// probes and arena initialization can still force the JIT back to the previous
// checked page-table path without exposing an unsafe host pointer.
void SetPpcFastmemRequested(bool requested);
bool IsPpcFastmemRequested();
void SetPpcFastmemCapabilities(bool arena, bool read_only_mappings);
bool IsPpcFastmemArenaSupported();
bool ArePpcFastmemReadOnlyMappingsSupported();
void SetPpcFastmemArenaActive(bool active);
bool IsPpcFastmemArenaActive();
bool IsPpcFastmemExceptionRecoveryEnabled();

void ResetPpcFastmemStats();
PpcFastmemStats ReadPpcFastmemStats();
void RecordPpcMemorySite(PpcMemorySite site);
void RecordPpcSlowAccess(bool mmio);
void RecordPpcFastmemFault(bool handled);
void RecordPpcFastmemMapping(bool created, bool success);
}  // namespace Horizon

#include <cassert>
#include <iostream>

#include "Horizon/Fastmem.h"

int main()
{
  using namespace Horizon;
  SetPpcFastmemRequested(false);
  SetPpcFastmemCapabilities(true, true);
  SetPpcFastmemArenaActive(true);
  assert(!IsPpcFastmemArenaActive());
  assert(!IsPpcFastmemExceptionRecoveryEnabled());

  SetPpcFastmemRequested(true);
  assert(IsPpcFastmemArenaSupported());
  assert(ArePpcFastmemReadOnlyMappingsSupported());
  SetPpcFastmemArenaActive(true);
  assert(IsPpcFastmemExceptionRecoveryEnabled());

  ResetPpcFastmemStats();
  RecordPpcMemorySite(PpcMemorySite::Direct);
  RecordPpcMemorySite(PpcMemorySite::DirectWithFallback);
  RecordPpcMemorySite(PpcMemorySite::Translated);
  RecordPpcMemorySite(PpcMemorySite::Mmio);
  RecordPpcSlowAccess(false);
  RecordPpcSlowAccess(true);
  RecordPpcFastmemFault(true);
  RecordPpcFastmemFault(false);
  RecordPpcFastmemMapping(true, true);
  RecordPpcFastmemMapping(false, true);
  RecordPpcFastmemMapping(true, false);
  const auto stats = ReadPpcFastmemStats();
  assert(stats.compiled_direct_sites == 1 && stats.compiled_fallback_sites == 1 &&
         stats.compiled_translated_sites == 1 && stats.compiled_mmio_sites == 1);
  assert(stats.runtime_slow_accesses == 2 && stats.runtime_mmio_accesses == 1);
  assert(stats.faults_received == 2 && stats.faults_handled == 1 &&
         stats.faults_unhandled == 1);
  assert(stats.mappings_created == 1 && stats.mappings_released == 1 &&
         stats.mapping_failures == 1);

  SetPpcFastmemRequested(false);
  RecordPpcSlowAccess(false);
  assert(ReadPpcFastmemStats().runtime_slow_accesses == 2);
  std::cout << "PASS: Horizon fastmem capability gates and aggregate counters\n";
}

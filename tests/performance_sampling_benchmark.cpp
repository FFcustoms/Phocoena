// SPDX-License-Identifier: GPL-2.0-or-later
#include "Horizon/Performance.h"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>

int main(int argc, char** argv)
{
  using namespace Horizon;
  if (argc != 2)
    return 2;

  const std::uint32_t requested = static_cast<std::uint32_t>(std::stoul(argv[1]));
  if (requested != PERF_SAMPLE_SCALE_LEGACY && requested != PERF_SAMPLE_SCALE_DEFAULT)
    return 2;

  constexpr std::uint64_t iterations = 20000000;
  SetPerfSampleScale(requested);
  const auto before = GetPerfCounters().Read();
  const auto started = std::chrono::steady_clock::now();
  std::thread worker([] {
    for (std::uint64_t i = 0; i < iterations; ++i)
    {
      const SampledPerfSample<PerfMetric::VulkanDrawSampled> sample;
    }
  });
  worker.join();
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now() - started);
  const auto delta = PerformanceDelta(before, GetPerfCounters().Read())
                         [static_cast<std::size_t>(PerfMetric::VulkanDrawSampled)];
  std::cout << "scale=" << requested << " iterations=" << iterations
            << " sampled=" << delta.calls << " elapsed_ns=" << elapsed.count() << '\n';
  return 0;
}

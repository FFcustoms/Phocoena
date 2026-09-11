// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>
#include "Horizon/Performance.h"

namespace Horizon
{
// Only callbacks on the CPU thread read PPC/CoreTiming state. The host reads a
// copy, never pauses the CPU and never reads live registers across threads.
// No SD logging happens in these callbacks; the host reports every five seconds.
class BootProgress
{
public:
  static constexpr std::size_t CADENCE_CAPACITY = 512;

  struct Snapshot
  {
    std::uint64_t vi_fields = 0, xfb_copies = 0, presents_queued = 0, presents_completed = 0;
    std::uint64_t unique_presents_queued = 0, duplicate_presents_queued = 0;
    std::uint64_t unique_presents_completed = 0, duplicate_presents_completed = 0;
    std::uint64_t present_interval_sequence = 0, frame_sequence_gaps = 0;
    std::array<std::uint32_t, CADENCE_CAPACITY> present_interval_us{};
    std::uint64_t ticks = 0;
    std::uint32_t pc = 0, r3 = 0, ticks_per_second = 0, xfb_width = 0, xfb_height = 0;
    std::chrono::steady_clock::time_point sampled_at{};
    ThreadUsage cpu_usage, gpu_usage;
  };

  void OnVI(std::uint32_t pc, std::uint32_t r3, std::uint64_t ticks,
            std::uint32_t ticks_per_second, ThreadUsage usage = {})
  {
    std::lock_guard lock(m_mutex);
    ++m_snapshot.vi_fields;
    m_snapshot.pc = pc;
    m_snapshot.r3 = r3;
    m_snapshot.ticks = ticks;
    m_snapshot.ticks_per_second = ticks_per_second;
    m_snapshot.sampled_at = std::chrono::steady_clock::now();
    m_snapshot.cpu_usage = usage;
  }

  void OnXFB()
  {
    std::lock_guard lock(m_mutex);
    ++m_snapshot.xfb_copies;
  }

  void OnBeforePresent(std::uint32_t width, std::uint32_t height,
                       std::uint64_t /*frame_count*/ = 0, bool duplicate = false)
  {
    std::lock_guard lock(m_mutex);
    ++m_snapshot.presents_queued;
    ++(duplicate ? m_snapshot.duplicate_presents_queued : m_snapshot.unique_presents_queued);
    m_snapshot.xfb_width = width;
    m_snapshot.xfb_height = height;
  }

  void OnAfterPresent(std::uint64_t frame_count, bool duplicate, ThreadUsage usage = {},
                      std::chrono::steady_clock::time_point presented_at =
                          std::chrono::steady_clock::now())
  {
    std::lock_guard lock(m_mutex);
    ++m_snapshot.presents_completed;
    if (duplicate)
    {
      ++m_snapshot.duplicate_presents_completed;
    }
    else
    {
      ++m_snapshot.unique_presents_completed;
      if (m_have_completed_frame && frame_count > m_last_completed_frame + 1)
        m_snapshot.frame_sequence_gaps += frame_count - m_last_completed_frame - 1;
      m_last_completed_frame = frame_count;
      m_have_completed_frame = true;
      if (m_last_unique_present != std::chrono::steady_clock::time_point{})
      {
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                                 presented_at - m_last_unique_present)
                                 .count();
        if (elapsed >= 0)
        {
          const auto bounded = static_cast<std::uint32_t>(
              std::min<std::uint64_t>(static_cast<std::uint64_t>(elapsed),
                                      std::numeric_limits<std::uint32_t>::max()));
          m_snapshot.present_interval_us[m_snapshot.present_interval_sequence %
                                         CADENCE_CAPACITY] = bounded;
          ++m_snapshot.present_interval_sequence;
        }
      }
      m_last_unique_present = presented_at;
    }
    m_snapshot.gpu_usage = usage;
  }

  void OnAfterPresent(ThreadUsage usage = {}) { OnAfterPresent(0, false, usage); }

  Snapshot Read() const
  {
    std::lock_guard lock(m_mutex);
    return m_snapshot;
  }

private:
  mutable std::mutex m_mutex;
  Snapshot m_snapshot;
  std::chrono::steady_clock::time_point m_last_unique_present{};
  std::uint64_t m_last_completed_frame = 0;
  bool m_have_completed_frame = false;
};

struct FrameCadenceSummary
{
  std::uint64_t samples = 0, lost_samples = 0;
  double mean_ms = 0.0, p95_ms = 0.0, p99_ms = 0.0, one_percent_low_fps = 0.0;
};

// Summarize only intervals recorded after previous_sequence. The callback uses
// a fixed ring and no allocation; sorting happens on the five-second reporter.
inline FrameCadenceSummary SummarizeFrameCadence(const BootProgress::Snapshot& snapshot,
                                                 std::uint64_t previous_sequence)
{
  FrameCadenceSummary result;
  if (snapshot.present_interval_sequence <= previous_sequence)
    return result;
  const std::uint64_t requested = snapshot.present_interval_sequence - previous_sequence;
  result.samples = std::min<std::uint64_t>(requested, BootProgress::CADENCE_CAPACITY);
  result.lost_samples = requested - result.samples;
  std::array<std::uint32_t, BootProgress::CADENCE_CAPACITY> values{};
  const std::uint64_t first = snapshot.present_interval_sequence - result.samples;
  std::uint64_t sum = 0;
  for (std::uint64_t i = 0; i < result.samples; ++i)
  {
    values[i] = snapshot.present_interval_us[(first + i) % BootProgress::CADENCE_CAPACITY];
    sum += values[i];
  }
  std::sort(values.begin(), values.begin() + result.samples);
  const auto percentile = [&](std::uint64_t percent) {
    const std::uint64_t rank = (result.samples * percent + 99) / 100;
    return static_cast<double>(values[std::max<std::uint64_t>(rank, 1) - 1]) / 1000.0;
  };
  result.mean_ms = static_cast<double>(sum) / result.samples / 1000.0;
  result.p95_ms = percentile(95);
  result.p99_ms = percentile(99);
  const std::uint64_t low_count = std::max<std::uint64_t>((result.samples + 99) / 100, 1);
  std::uint64_t low_sum = 0;
  for (std::uint64_t i = result.samples - low_count; i < result.samples; ++i)
    low_sum += values[i];
  const double low_mean_us = static_cast<double>(low_sum) / low_count;
  result.one_percent_low_fps = low_mean_us > 0.0 ? 1000000.0 / low_mean_us : 0.0;
  return result;
}
}

// SPDX-License-Identifier: GPL-2.0-or-later
#include "Horizon/RuntimeDiagnostics.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>

#include <switch.h>

#include "Core/System.h"
#include "Horizon/Log.h"
#include "Horizon/Performance.h"
#include "VideoCommon/CommandProcessor.h"
#include "VideoCommon/Fifo.h"

namespace Horizon
{
namespace
{
constexpr u64 SAMPLE_INTERVAL_MS = 100;
constexpr std::size_t SAMPLE_COUNT = 256;  // ~25.6 seconds at 10 Hz.
constexpr const char* FAILURE_TAIL = "sdmc:/switch/Phocoena/logs/failure-tail.log";

struct RuntimeSample
{
  u64 ms = 0;
  u32 base = 0;
  u32 end = 0;
  u32 read = 0;
  u32 safe_read = 0;
  u32 write = 0;
  u32 distance = 0;
  u32 breakpoint = 0;
  u32 gp_read = 0;
  u32 gp_link = 0;
  u32 bp_enable = 0;
  u32 bp_hit = 0;
  u32 lo_watermark = 0;
  u32 hi_watermark = 0;
  u32 batch_blocks = 1;
  u64 decoder_calls = 0;
  u64 gather_blocks = 0;
};

std::array<RuntimeSample, SAMPLE_COUNT> samples{};
std::mutex samples_mutex;
std::size_t next_sample = 0;
std::size_t valid_samples = 0;
std::atomic<u64> last_sample_ms{0};
std::atomic<bool> gpu_wait_guard_tripped{false};
std::atomic<u64> gpu_wait_timeout_count{0};
std::atomic<u64> pi_fifo_reset_count{0};
std::atomic<u64> pi_reset_code_count{0};
std::atomic<u32> last_pi_reset_code{0};

u64 NowMs()
{
  return armTicksToNs(armGetSystemTick()) / 1000000;
}

RuntimeSample ReadSample(Core::System& system, u64 ms)
{
  auto& fifo = system.GetCommandProcessor().GetFifo();
  const auto batch = ReadFifoBatchStats();
  return {
      .ms = ms,
      .base = fifo.CPBase.load(std::memory_order_relaxed),
      .end = fifo.CPEnd.load(std::memory_order_relaxed),
      .read = fifo.CPReadPointer.load(std::memory_order_relaxed),
      .safe_read = fifo.SafeCPReadPointer.load(std::memory_order_relaxed),
      .write = fifo.CPWritePointer.load(std::memory_order_relaxed),
      .distance = fifo.CPReadWriteDistance.load(std::memory_order_relaxed),
      .breakpoint = fifo.CPBreakpoint.load(std::memory_order_relaxed),
      .gp_read = fifo.bFF_GPReadEnable.load(std::memory_order_relaxed),
      .gp_link = fifo.bFF_GPLinkEnable.load(std::memory_order_relaxed),
      .bp_enable = fifo.bFF_BPEnable.load(std::memory_order_relaxed),
      .bp_hit = fifo.bFF_Breakpoint.load(std::memory_order_relaxed),
      .lo_watermark = fifo.bFF_LoWatermark.load(std::memory_order_relaxed),
      .hi_watermark = fifo.bFF_HiWatermark.load(std::memory_order_relaxed),
      .batch_blocks = GetFifoBatchBlocks(),
      .decoder_calls = batch.decoder_calls,
      .gather_blocks = batch.gather_blocks,
  };
}

void PushSample(const RuntimeSample& sample)
{
  std::lock_guard lock(samples_mutex);
  samples[next_sample] = sample;
  next_sample = (next_sample + 1) % SAMPLE_COUNT;
  valid_samples = std::min(valid_samples + 1, SAMPLE_COUNT);
}

void CaptureNow(Core::System& system)
{
  const u64 ms = NowMs();
  PushSample(ReadSample(system, ms));
  last_sample_ms.store(ms, std::memory_order_relaxed);
}

void DumpTail(Core::System& system, std::string_view reason)
{
  CaptureNow(system);

  std::array<RuntimeSample, SAMPLE_COUNT> snapshot{};
  std::size_t count = 0;
  {
    std::lock_guard lock(samples_mutex);
    count = valid_samples;
    const std::size_t first = (next_sample + SAMPLE_COUNT - valid_samples) % SAMPLE_COUNT;
    for (std::size_t i = 0; i < count; ++i)
      snapshot[i] = samples[(first + i) % SAMPLE_COUNT];
  }

  FILE* file = std::fopen(FAILURE_TAIL, "a");
  if (!file)
  {
    Error("Runtime diagnostics", "failed opening %s", FAILURE_TAIL);
    return;
  }

  std::fprintf(file, "# event_ms=%llu reason=%.*s samples=%zu\n",
               static_cast<unsigned long long>(NowMs()), static_cast<int>(reason.size()),
               reason.data(), count);
  std::fputs("# ms,base,end,read,safe_read,write,distance,breakpoint,gp_read,gp_link,bp_enable,bp_hit,lo_watermark,hi_watermark,batch,decoder_calls,gather_blocks\n",
             file);
  for (std::size_t i = 0; i < count; ++i)
  {
    const auto& s = snapshot[i];
    std::fprintf(file,
                 "%llu,%08x,%08x,%08x,%08x,%08x,%u,%08x,%u,%u,%u,%u,%u,%u,%u,%llu,%llu\n",
                 static_cast<unsigned long long>(s.ms), s.base, s.end, s.read, s.safe_read,
                 s.write, s.distance, s.breakpoint, s.gp_read, s.gp_link, s.bp_enable, s.bp_hit,
                 s.lo_watermark, s.hi_watermark, s.batch_blocks,
                 static_cast<unsigned long long>(s.decoder_calls),
                 static_cast<unsigned long long>(s.gather_blocks));
  }
  std::fputc('\n', file);
  std::fflush(file);
  std::fclose(file);
}

void LogCurrentFifo(Core::System& system, const char* event, u32 value)
{
  const auto sample = ReadSample(system, NowMs());
  Log("Runtime diagnostics",
      "%s value=0x%08x CP base=%08x end=%08x read=%08x safe=%08x write=%08x "
      "distance=%u bp=%08x gp_read=%u gp_link=%u bp_enable=%u bp_hit=%u lo=%u hi=%u "
      "batch=%u decoder_calls=%llu gather_blocks=%llu",
      event, value, sample.base, sample.end, sample.read, sample.safe_read, sample.write,
      sample.distance, sample.breakpoint, sample.gp_read, sample.gp_link, sample.bp_enable,
      sample.bp_hit, sample.lo_watermark, sample.hi_watermark, sample.batch_blocks,
      static_cast<unsigned long long>(sample.decoder_calls),
      static_cast<unsigned long long>(sample.gather_blocks));
}
}  // namespace

void ResetRuntimeDiagnostics()
{
  {
    std::lock_guard lock(samples_mutex);
    samples = {};
    next_sample = 0;
    valid_samples = 0;
  }
  last_sample_ms.store(0, std::memory_order_relaxed);
  gpu_wait_guard_tripped.store(false, std::memory_order_relaxed);
  gpu_wait_timeout_count.store(0, std::memory_order_relaxed);
  pi_fifo_reset_count.store(0, std::memory_order_relaxed);
  pi_reset_code_count.store(0, std::memory_order_relaxed);
  last_pi_reset_code.store(0, std::memory_order_relaxed);

  if (FILE* file = std::fopen(FAILURE_TAIL, "w"))
  {
    std::fputs("# Phocoena runtime failure tail. Written only on critical guest GPU/reset events.\n",
               file);
    std::fclose(file);
  }
}

RuntimeFailureStats ReadRuntimeFailureStats()
{
  return {
      .gpu_wait_timeouts = gpu_wait_timeout_count.load(std::memory_order_relaxed),
      .pi_fifo_resets = pi_fifo_reset_count.load(std::memory_order_relaxed),
      .pi_reset_codes = pi_reset_code_count.load(std::memory_order_relaxed),
      .last_pi_reset_code = last_pi_reset_code.load(std::memory_order_relaxed),
  };
}

void CaptureRuntimeSample(Core::System& system)
{
  const u64 now = NowMs();
  u64 previous = last_sample_ms.load(std::memory_order_relaxed);
  if (previous != 0 && now - previous < SAMPLE_INTERVAL_MS)
    return;
  if (!last_sample_ms.compare_exchange_strong(previous, now, std::memory_order_relaxed))
    return;
  PushSample(ReadSample(system, now));
}

void ReportGuestGpuWaitTimeout(Core::System& system, std::string_view message)
{
  gpu_wait_timeout_count.fetch_add(1, std::memory_order_relaxed);
  const u32 previous_batch = GetFifoBatchBlocks();
  const bool first = !gpu_wait_guard_tripped.exchange(true, std::memory_order_relaxed);
  if (first)
    SetFifoBatchBlocks(1);

  // A GP watchdog means the guest expected forward progress and did not see it.
  // Wake the graphics loop, but never synchronously wait from the CPU thread.
  system.GetFifo().RunGpu();
  LogCurrentFifo(system, first ? "GP WAIT timeout; compatibility guard batch->1 + GPU wake" :
                                 "GP WAIT timeout; compatibility guard already active + GPU wake",
                 previous_batch);
  DumpTail(system, message);
  FlushLog();
}

void ReportPiFifoReset(Core::System& system, u32 value)
{
  // GXAbortFrame can use PI_FIFO_RESET during normal operation. Preserve it as
  // a critical event only when a GP watchdog already tripped in this session.
  if ((value & 1) == 0 || !gpu_wait_guard_tripped.load(std::memory_order_relaxed))
    return;
  pi_fifo_reset_count.fetch_add(1, std::memory_order_relaxed);
  LogCurrentFifo(system, "guest PI_FIFO_RESET after GP watchdog", value);
  DumpTail(system, "guest PI_FIFO_RESET after GP watchdog");
  FlushLog();
}

void ReportPiResetCode(Core::System& system, u32 value)
{
  if (value == 0)
    return;
  pi_reset_code_count.fetch_add(1, std::memory_order_relaxed);
  last_pi_reset_code.store(value, std::memory_order_relaxed);
  LogCurrentFifo(system, "guest PI_RESET_CODE", value);
  DumpTail(system, "guest PI_RESET_CODE");
  FlushLog();
}
}  // namespace Horizon

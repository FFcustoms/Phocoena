// SPDX-License-Identifier: GPL-2.0-or-later
#include "Horizon/Performance.h"
#include <atomic>
#include "VideoCommon/FifoBatch.h"
#ifdef __SWITCH__
#include <switch.h>
#elif defined(__linux__)
#include <time.h>
#include <thread>
#endif

namespace Horizon
{
namespace
{
std::atomic<std::uint64_t> vertex_calls{0};
std::atomic<std::uint64_t> vertices_decoded{0};
std::atomic<std::uint64_t> vertex_loader_fallbacks{0};
std::atomic<std::uint64_t> fifo_decoder_calls{0};
std::atomic<std::uint64_t> fifo_gather_blocks{0};
std::atomic<std::uint64_t> fifo_max_blocks{0};
std::atomic<std::uint64_t> vulkan_enqueued{0};
std::atomic<std::uint64_t> vulkan_completed{0};
std::atomic<std::uint64_t> vulkan_acquire_overlaps{0};
std::atomic<std::uint64_t> vulkan_acquire_drains{0};
std::atomic<std::uint64_t> vulkan_acquire_guards{0};
std::atomic<std::uint64_t> vulkan_pending{0};
std::atomic<std::uint64_t> vulkan_peak_pending{0};
std::atomic<std::uint64_t> vulkan_push_descriptor_writes{0};
std::atomic<std::uint64_t> vulkan_legacy_sampler_allocations{0};
std::atomic<std::uint64_t> vulkan_legacy_sampler_updates{0};
std::atomic<bool> vulkan_present_overlap{false};
std::atomic<bool> vulkan_present_completion_guard{true};
std::atomic<bool> vulkan_triple_frame_buffering{false};
std::atomic<bool> vulkan_push_descriptors_requested{true};
std::atomic<std::uint32_t> fifo_batch_blocks{Fifo::HORIZON_FIFO_DEFAULT_BATCH_BLOCKS};
std::atomic<std::uint32_t> perf_sample_scale{PERF_SAMPLE_SCALE_DEFAULT};
}

void SetPerfSampleScale(std::uint32_t scale)
{
  perf_sample_scale.store(NormalizePerfSampleScale(scale), std::memory_order_relaxed);
}

std::uint32_t GetPerfSampleScale()
{
  return perf_sample_scale.load(std::memory_order_relaxed);
}

void RecordVertexWork(std::uint64_t vertices)
{
  vertex_calls.fetch_add(1, std::memory_order_relaxed);
  vertices_decoded.fetch_add(vertices, std::memory_order_relaxed);
}

void RecordVertexLoaderFallback()
{
  vertex_loader_fallbacks.fetch_add(1, std::memory_order_relaxed);
}

VertexWorkSnapshot ReadVertexWork()
{
  return {vertex_calls.load(std::memory_order_relaxed),
          vertices_decoded.load(std::memory_order_relaxed),
          vertex_loader_fallbacks.load(std::memory_order_relaxed)};
}

void ResetFifoBatchStats()
{
  fifo_decoder_calls.store(0, std::memory_order_relaxed);
  fifo_gather_blocks.store(0, std::memory_order_relaxed);
  fifo_max_blocks.store(0, std::memory_order_relaxed);
}

void RecordFifoBatch(std::uint64_t blocks)
{
  fifo_decoder_calls.fetch_add(1, std::memory_order_relaxed);
  fifo_gather_blocks.fetch_add(blocks, std::memory_order_relaxed);
  std::uint64_t maximum = fifo_max_blocks.load(std::memory_order_relaxed);
  while (maximum < blocks && !fifo_max_blocks.compare_exchange_weak(
                                 maximum, blocks, std::memory_order_relaxed))
  {
  }
}

FifoBatchSnapshot ReadFifoBatchStats()
{
  return {fifo_decoder_calls.load(std::memory_order_relaxed),
          fifo_gather_blocks.load(std::memory_order_relaxed),
          fifo_max_blocks.load(std::memory_order_relaxed)};
}

ThreadCpuTime ReadCurrentThreadCpuTime()
{
#ifdef __SWITCH__
  u64 ticks = 0;
  const InfoType type = hosversionAtLeast(13, 0, 0) ? InfoType_ThreadTickCount :
                                                  InfoType_ThreadTickCountDeprecated;
  const Result result = svcGetInfo(&ticks, type, CUR_THREAD_HANDLE, TickCountInfo_Total);
  return {R_SUCCEEDED(result) ? armTicksToNs(ticks) : 0, R_SUCCEEDED(result)};
#elif defined(__linux__)
  timespec time{};
  const bool valid = clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time) == 0;
  return {static_cast<std::uint64_t>(time.tv_sec) * 1000000000 + time.tv_nsec, valid};
#else
  return {};
#endif
}

#ifndef __SWITCH__
// Real host thread CPU time for sanitizer tests; no simulated Switch timings.
ThreadUsage ReadCurrentThreadUsage()
{
#ifdef __linux__
  const auto cpu = ReadCurrentThreadCpuTime();
  const auto wall = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  return {std::hash<std::thread::id>{}(std::this_thread::get_id()), cpu.ns,
          static_cast<std::uint64_t>(wall), 0, 0, cpu.valid};
#else
  return {};
#endif
}
#endif

PerfCounters& GetPerfCounters()
{
  static PerfCounters counters;
  return counters;
}
WorkerUsage& GetVulkanWorkerUsage()
{
  static WorkerUsage usage;
  return usage;
}

void ResetVulkanOverlapStats()
{
  vulkan_enqueued.store(0, std::memory_order_relaxed);
  vulkan_completed.store(0, std::memory_order_relaxed);
  vulkan_acquire_overlaps.store(0, std::memory_order_relaxed);
  vulkan_acquire_drains.store(0, std::memory_order_relaxed);
  vulkan_acquire_guards.store(0, std::memory_order_relaxed);
  vulkan_pending.store(0, std::memory_order_relaxed);
  vulkan_peak_pending.store(0, std::memory_order_relaxed);
}

void RecordVulkanWorkerQueued()
{
  vulkan_enqueued.fetch_add(1, std::memory_order_relaxed);
  const std::uint64_t pending = vulkan_pending.fetch_add(1, std::memory_order_relaxed) + 1;
  std::uint64_t peak = vulkan_peak_pending.load(std::memory_order_relaxed);
  while (peak < pending && !vulkan_peak_pending.compare_exchange_weak(
                               peak, pending, std::memory_order_relaxed))
  {
  }
}

void RecordVulkanWorkerCompleted()
{
  vulkan_completed.fetch_add(1, std::memory_order_relaxed);
  const std::uint64_t previous = vulkan_pending.fetch_sub(1, std::memory_order_relaxed);
  if (previous == 0)
    vulkan_pending.store(0, std::memory_order_relaxed);
}

void RecordVulkanAcquireMode(bool overlap)
{
  (overlap ? vulkan_acquire_overlaps : vulkan_acquire_drains)
      .fetch_add(1, std::memory_order_relaxed);
}

void RecordVulkanAcquireGuard()
{
  vulkan_acquire_guards.fetch_add(1, std::memory_order_relaxed);
}

VulkanOverlapSnapshot ReadVulkanOverlapStats()
{
  return {vulkan_enqueued.load(std::memory_order_relaxed),
          vulkan_completed.load(std::memory_order_relaxed),
          vulkan_acquire_overlaps.load(std::memory_order_relaxed),
          vulkan_acquire_drains.load(std::memory_order_relaxed),
          vulkan_acquire_guards.load(std::memory_order_relaxed),
          vulkan_pending.load(std::memory_order_relaxed),
          vulkan_peak_pending.load(std::memory_order_relaxed)};
}

void ResetVulkanDescriptorStats()
{
  vulkan_push_descriptor_writes.store(0, std::memory_order_relaxed);
  vulkan_legacy_sampler_allocations.store(0, std::memory_order_relaxed);
  vulkan_legacy_sampler_updates.store(0, std::memory_order_relaxed);
}

void RecordVulkanPushDescriptorWrite()
{
  vulkan_push_descriptor_writes.fetch_add(1, std::memory_order_relaxed);
}

void RecordVulkanLegacySamplerDescriptorWrite()
{
  vulkan_legacy_sampler_allocations.fetch_add(1, std::memory_order_relaxed);
  vulkan_legacy_sampler_updates.fetch_add(1, std::memory_order_relaxed);
}

VulkanDescriptorSnapshot ReadVulkanDescriptorStats()
{
  return {vulkan_push_descriptor_writes.load(std::memory_order_relaxed),
          vulkan_legacy_sampler_allocations.load(std::memory_order_relaxed),
          vulkan_legacy_sampler_updates.load(std::memory_order_relaxed)};
}

void SetVulkanPresentOverlapEnabled(bool enabled)
{
  vulkan_present_overlap.store(enabled, std::memory_order_relaxed);
}

bool IsVulkanPresentOverlapEnabled()
{
  return vulkan_present_overlap.load(std::memory_order_relaxed);
}

void SetVulkanPresentCompletionGuardEnabled(bool enabled)
{
  vulkan_present_completion_guard.store(enabled, std::memory_order_relaxed);
}

bool IsVulkanPresentCompletionGuardEnabled()
{
  return vulkan_present_completion_guard.load(std::memory_order_relaxed);
}

void SetVulkanTripleFrameBufferingEnabled(bool enabled)
{
  vulkan_triple_frame_buffering.store(enabled, std::memory_order_relaxed);
}

bool IsVulkanTripleFrameBufferingEnabled()
{
  return vulkan_triple_frame_buffering.load(std::memory_order_relaxed);
}

void SetVulkanPushDescriptorsRequested(bool enabled)
{
  vulkan_push_descriptors_requested.store(enabled, std::memory_order_relaxed);
}

bool AreVulkanPushDescriptorsRequested()
{
  return vulkan_push_descriptors_requested.load(std::memory_order_relaxed);
}

void SetFifoBatchingEnabled(bool enabled)
{
  SetFifoBatchBlocks(enabled ? Fifo::HORIZON_FIFO_LEGACY_BATCH_BLOCKS : 1);
}

bool IsFifoBatchingEnabled()
{
  return GetFifoBatchBlocks() > 1;
}

void SetFifoBatchBlocks(std::uint32_t blocks)
{
  fifo_batch_blocks.store(Fifo::NormalizeFifoBatchBlocks(blocks), std::memory_order_relaxed);
}

std::uint32_t GetFifoBatchBlocks()
{
  return fifo_batch_blocks.load(std::memory_order_relaxed);
}
}

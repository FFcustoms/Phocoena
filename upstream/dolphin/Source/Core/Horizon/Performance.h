// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>

namespace Horizon
{
enum class PerfMetric : std::size_t
{
  EGLSwap, ShaderCompile, PipelineCreate, JitCompile, VertexLoaderJitCompile,
  DiscRead, DiscWait, GpuWait, LogWrite,
  VulkanAcquire, VulkanSubmit, VulkanPresent, VulkanWorkerWait, VulkanFenceWait,
  VulkanAcquireDrain, VulkanPresentGuard, VulkanSyncSubmit, VulkanFrameReuse, VulkanCommandReuse,
  VulkanCounterWait, VulkanReadback, VulkanUpload, VulkanDeviceIdle,
  FifoSync, CpuGpuBlocking, XfbEvent, EfbPeek,
  EfbPokeSampled, GpuFifoSampled, VertexDecodeSampled, VertexFlushSampled,
  TextureLoadSampled, EfbCopySampled, VulkanDrawSampled, VulkanDispatchSampled, Count
};
inline constexpr std::size_t PERF_METRICS = static_cast<std::size_t>(PerfMetric::Count);
inline constexpr std::array<const char*, PERF_METRICS> PERF_NAMES = {
    "egl_swap", "shader_compile", "pipeline_create", "jit_compile", "vertex_loader_jit_compile",
    "disc_read", "disc_wait", "gpu_wait", "log_write", "vk_acquire", "vk_submit", "vk_present",
    "vk_worker_wait", "vk_fence_wait", "vk_acquire_drain", "vk_present_guard", "vk_sync_submit",
    "vk_frame_reuse", "vk_command_reuse", "vk_counter_wait", "vk_readback", "vk_upload",
    "vk_device_idle", "fifo_sync", "cpu_gpu_blocking", "xfb_event", "efb_peek",
    "efb_poke_sampled", "gpu_fifo_sampled", "vertex_decode_sampled", "vertex_flush_sampled",
    "texture_load_sampled", "efb_copy_sampled", "vk_draw_sampled", "vk_dispatch_sampled"};

inline constexpr bool IsSampledMetric(PerfMetric metric)
{
  return metric >= PerfMetric::EfbPokeSampled && metric < PerfMetric::Count;
}

struct PerfCounter
{
  std::uint64_t calls = 0, ns = 0;
  std::uint64_t cpu_calls = 0, cpu_ns = 0;
};
using PerfSnapshot = std::array<PerfCounter, PERF_METRICS>;

// No logging or allocation in these counters. They are process cumulative;
// readers take differences, so a new boot cannot reset an in-flight writer.
class PerfCounters
{
public:
  void Record(PerfMetric metric, std::uint64_t ns, std::uint64_t cpu_ns = 0, bool cpu_valid = false)
  {
    std::lock_guard lock(m_mutex);
    auto& counter = m_counters[static_cast<std::size_t>(metric)];
    ++counter.calls;
    counter.ns += ns;
    if (cpu_valid)
    {
      ++counter.cpu_calls;
      counter.cpu_ns += cpu_ns;
    }
  }
  PerfSnapshot Read() const
  {
    std::lock_guard lock(m_mutex);
    return m_counters;
  }
private:
  mutable std::mutex m_mutex;
  PerfSnapshot m_counters{};
};

inline PerfSnapshot PerformanceDelta(const PerfSnapshot& previous, const PerfSnapshot& current)
{
  PerfSnapshot result{};
  for (std::size_t i = 0; i < PERF_METRICS; ++i)
  {
    if (current[i].calls >= previous[i].calls && current[i].ns >= previous[i].ns)
      result[i] = {current[i].calls - previous[i].calls, current[i].ns - previous[i].ns};
    if (current[i].cpu_calls >= previous[i].cpu_calls && current[i].cpu_ns >= previous[i].cpu_ns)
    {
      result[i].cpu_calls = current[i].cpu_calls - previous[i].cpu_calls;
      result[i].cpu_ns = current[i].cpu_ns - previous[i].cpu_ns;
    }
  }
  return result;
}

PerfCounters& GetPerfCounters();

struct VertexWorkSnapshot
{
  std::uint64_t calls = 0;
  std::uint64_t vertices = 0;
  std::uint64_t native_fallbacks = 0;
};
void RecordVertexWork(std::uint64_t vertices);
void RecordVertexLoaderFallback();
VertexWorkSnapshot ReadVertexWork();
inline VertexWorkSnapshot VertexWorkDelta(const VertexWorkSnapshot& previous,
                                          const VertexWorkSnapshot& current)
{
  return {current.calls >= previous.calls ? current.calls - previous.calls : 0,
          current.vertices >= previous.vertices ? current.vertices - previous.vertices : 0,
          current.native_fallbacks >= previous.native_fallbacks ?
              current.native_fallbacks - previous.native_fallbacks : 0};
}

struct FifoBatchSnapshot
{
  std::uint64_t decoder_calls = 0;
  std::uint64_t gather_blocks = 0;
  std::uint64_t max_blocks = 0;
};
void ResetFifoBatchStats();
void RecordFifoBatch(std::uint64_t blocks);
FifoBatchSnapshot ReadFifoBatchStats();
inline FifoBatchSnapshot FifoBatchDelta(const FifoBatchSnapshot& previous,
                                        const FifoBatchSnapshot& current)
{
  return {current.decoder_calls >= previous.decoder_calls ?
              current.decoder_calls - previous.decoder_calls : 0,
          current.gather_blocks >= previous.gather_blocks ?
              current.gather_blocks - previous.gather_blocks : 0,
          current.max_blocks};
}

struct ThreadCpuTime
{
  std::uint64_t ns = 0;
  bool valid = false;
};
ThreadCpuTime ReadCurrentThreadCpuTime();

class ScopedPerfSample
{
public:
  explicit ScopedPerfSample(PerfMetric metric, bool measure_cpu = false)
      : m_metric(metric), m_cpu(measure_cpu ? ReadCurrentThreadCpuTime() : ThreadCpuTime{}),
        m_started(Clock::now()) {}
  ~ScopedPerfSample()
  {
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - m_started).count();
    const auto cpu = m_cpu.valid ? ReadCurrentThreadCpuTime() : ThreadCpuTime{};
    const bool valid = cpu.valid && cpu.ns >= m_cpu.ns;
    GetPerfCounters().Record(m_metric, static_cast<std::uint64_t>(ns),
                             valid ? cpu.ns - m_cpu.ns : 0, valid);
  }
  ScopedPerfSample(const ScopedPerfSample&) = delete;
  ScopedPerfSample& operator=(const ScopedPerfSample&) = delete;
private:
  using Clock = std::chrono::steady_clock;
  PerfMetric m_metric;
  ThreadCpuTime m_cpu;
  Clock::time_point m_started;
};

inline constexpr std::uint32_t PERF_SAMPLE_SCALE_LEGACY = 64;
inline constexpr std::uint32_t PERF_SAMPLE_SCALE_DEFAULT = 1024;

constexpr std::uint32_t NormalizePerfSampleScale(std::uint32_t scale)
{
  return scale == PERF_SAMPLE_SCALE_LEGACY ? PERF_SAMPLE_SCALE_LEGACY :
                                             PERF_SAMPLE_SCALE_DEFAULT;
}

void SetPerfSampleScale(std::uint32_t scale);
std::uint32_t GetPerfSampleScale();

// Hot paths are sampled at a configurable power-of-two probability. A
// per-thread PRNG avoids selecting the same draw in every frame. Unsampled
// calls take no clock, syscall, allocation or mutex. The scale is cached when
// an emulation thread first enters a sampled site; settings are applied before
// those threads are created. Report raw sampled totals, never pretend they are
// exact whole-frame times or add nested scopes together.
class PerfSampleGate
{
public:
  explicit PerfSampleGate(std::uint32_t scale = PERF_SAMPLE_SCALE_DEFAULT)
      : m_mask(NormalizePerfSampleScale(scale) - 1)
  {
  }
  bool Next()
  {
    m_state ^= m_state << 13;
    m_state ^= m_state >> 17;
    m_state ^= m_state << 5;
    return (m_state & m_mask) == 0;
  }
private:
  std::uint32_t m_state = 0x72b5a901;
  std::uint32_t m_mask;
};

template <PerfMetric Metric>
class SampledPerfSample
{
public:
  SampledPerfSample()
  {
    static thread_local PerfSampleGate gate{GetPerfSampleScale()};
    if (gate.Next())
      m_sample.emplace(Metric, true);
  }
  SampledPerfSample(const SampledPerfSample&) = delete;
  SampledPerfSample& operator=(const SampledPerfSample&) = delete;
private:
  static_assert(IsSampledMetric(Metric));
  std::optional<ScopedPerfSample> m_sample;
};

struct ThreadUsage
{
  std::uint64_t id = 0, cpu_ns = 0, wall_ns = 0;
  std::uint32_t core = 0, result = 0;
  bool valid = false;
};
ThreadUsage ReadCurrentThreadUsage();

// Worker publishes only its own OS thread counters, never accesses core state.
// Boot time and thread identity let readers reject a stale previous-boot sample.
class WorkerUsage
{
public:
  void Publish(ThreadUsage usage)
  {
    std::lock_guard lock(m_mutex);
    m_usage = usage;
  }
  ThreadUsage Read() const
  {
    std::lock_guard lock(m_mutex);
    return m_usage;
  }
private:
  mutable std::mutex m_mutex;
  ThreadUsage m_usage;
};
WorkerUsage& GetVulkanWorkerUsage();

struct VulkanOverlapSnapshot
{
  std::uint64_t enqueued = 0;
  std::uint64_t completed = 0;
  std::uint64_t acquire_overlaps = 0;
  std::uint64_t acquire_drains = 0;
  std::uint64_t acquire_guards = 0;
  std::uint64_t pending = 0;
  std::uint64_t peak_pending = 0;
};

struct VulkanDescriptorSnapshot
{
  std::uint64_t push_writes = 0;
  std::uint64_t legacy_allocations = 0;
  std::uint64_t legacy_updates = 0;
};

void ResetVulkanDescriptorStats();
void RecordVulkanPushDescriptorWrite();
void RecordVulkanLegacySamplerDescriptorWrite();
VulkanDescriptorSnapshot ReadVulkanDescriptorStats();
inline VulkanDescriptorSnapshot VulkanDescriptorDelta(const VulkanDescriptorSnapshot& previous,
                                                       const VulkanDescriptorSnapshot& current)
{
  return {current.push_writes >= previous.push_writes ?
              current.push_writes - previous.push_writes : 0,
          current.legacy_allocations >= previous.legacy_allocations ?
              current.legacy_allocations - previous.legacy_allocations : 0,
          current.legacy_updates >= previous.legacy_updates ?
              current.legacy_updates - previous.legacy_updates : 0};
}

void ResetVulkanOverlapStats();
void RecordVulkanWorkerQueued();
void RecordVulkanWorkerCompleted();
void RecordVulkanAcquireMode(bool overlap);
void RecordVulkanAcquireGuard();
VulkanOverlapSnapshot ReadVulkanOverlapStats();
inline VulkanOverlapSnapshot VulkanOverlapDelta(const VulkanOverlapSnapshot& previous,
                                                 const VulkanOverlapSnapshot& current)
{
  return {current.enqueued >= previous.enqueued ? current.enqueued - previous.enqueued : 0,
          current.completed >= previous.completed ? current.completed - previous.completed : 0,
          current.acquire_overlaps >= previous.acquire_overlaps ?
              current.acquire_overlaps - previous.acquire_overlaps : 0,
          current.acquire_drains >= previous.acquire_drains ?
              current.acquire_drains - previous.acquire_drains : 0,
          current.acquire_guards >= previous.acquire_guards ?
              current.acquire_guards - previous.acquire_guards : 0,
          current.pending,
          current.peak_pending};
}

void SetVulkanPresentOverlapEnabled(bool enabled);
bool IsVulkanPresentOverlapEnabled();
void SetVulkanPresentCompletionGuardEnabled(bool enabled);
bool IsVulkanPresentCompletionGuardEnabled();
void SetVulkanTripleFrameBufferingEnabled(bool enabled);
bool IsVulkanTripleFrameBufferingEnabled();
void SetVulkanPushDescriptorsRequested(bool enabled);
bool AreVulkanPushDescriptorsRequested();
void SetFifoBatchingEnabled(bool enabled);
bool IsFifoBatchingEnabled();
void SetFifoBatchBlocks(std::uint32_t blocks);
std::uint32_t GetFifoBatchBlocks();

// -1 means unavailable, no time elapsed, or a different thread. In single-core
// mode CPU/GPU samples describe the SAME thread and must never be added together.
inline double ThreadBusyPercent(const ThreadUsage& previous, const ThreadUsage& current)
{
  if (!previous.valid || !current.valid || previous.id != current.id ||
      current.wall_ns <= previous.wall_ns || current.cpu_ns < previous.cpu_ns)
    return -1.0;
  return 100.0 * static_cast<double>(current.cpu_ns - previous.cpu_ns) /
         static_cast<double>(current.wall_ns - previous.wall_ns);
}
}

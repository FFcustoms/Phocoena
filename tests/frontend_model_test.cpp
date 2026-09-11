// SPDX-License-Identifier: GPL-2.0-or-later
#include "Horizon/FrontendModel.h"
#include "Horizon/TwilightPrincessBenchmark.h"
#include "Horizon/Time.h"
#include "Horizon/GraphicsChecks.h"
#include "Horizon/BootProgress.h"
#include "Horizon/LogReport.h"
#include "Horizon/VulkanDescriptorPolicy.h"
#include <cassert>
#include <iostream>
#include <thread>
namespace fs = std::filesystem;
void NoOverwriteRename(const fs::path& from, const fs::path& to, std::error_code& error)
{
  if (fs::exists(to)) error = std::make_error_code(std::errc::file_exists);
  else fs::rename(from, to, error);
}
void FailingReplacement(const fs::path& from, const fs::path& to, std::error_code& error)
{
  if (from.filename() == "replacement.tmp") error = std::make_error_code(std::errc::io_error);
  else NoOverwriteRename(from, to, error);
}
int main(int argc, char** argv)
{
  using namespace Horizon;
  assert(argc == 2);
  const std::filesystem::path root(argv[1]);
  assert(GetFifoBatchBlocks() == 8);
  SetFifoBatchBlocks(15);
  assert(GetFifoBatchBlocks() == 8 && IsFifoBatchingEnabled());
  SetFifoBatchBlocks(99);
  assert(GetFifoBatchBlocks() == 8);
  SetFifoBatchingEnabled(false);
  assert(GetFifoBatchBlocks() == 1 && !IsFifoBatchingEnabled());
  SetFifoBatchingEnabled(true);
  assert(GetFifoBatchBlocks() == 8 && IsFifoBatchingEnabled());
  SetFifoBatchBlocks(16);
  assert(GetFifoBatchBlocks() == 8);
  // Snapshot accounting remains coherent with concurrent producers; delta
  // snapshots allow boot baselines without resetting live counters.
  PerfCounters counters;
  const auto initial = counters.Read();
  std::thread perf_cpu([&] {
    for (unsigned i = 0; i < 1000; ++i) counters.Record(PerfMetric::JitCompile, 7);
  });
  std::thread perf_gpu([&] {
    for (unsigned i = 0; i < 1000; ++i) counters.Record(PerfMetric::EGLSwap, 11);
  });
  for (unsigned i = 0; i < 1000; ++i)
  {
    const auto snapshot = counters.Read();
    const auto jit = snapshot[static_cast<std::size_t>(PerfMetric::JitCompile)];
    const auto gl = snapshot[static_cast<std::size_t>(PerfMetric::EGLSwap)];
    assert(jit.ns == jit.calls * 7 && gl.ns == gl.calls * 11);
  }
  perf_cpu.join(); perf_gpu.join();
  const auto counted = counters.Read();
  assert(PerformanceDelta(initial, counted)[static_cast<std::size_t>(PerfMetric::EGLSwap)].ns == 11000);
  assert(PerformanceDelta(counted, counted)[0].calls == 0);
  assert(PerformanceDelta(counted, initial)[0].calls == 0); // never unsigned underflow
  counters.Record(PerfMetric::EGLSwap, 99);
  const auto delta = PerformanceDelta(counted, counters.Read());
  assert(delta[0].calls == 1 && delta[0].ns == 99);
  counters.Record(PerfMetric::VulkanDrawSampled, 100, 75, true);
  counters.Record(PerfMetric::VulkanDrawSampled, 90); // unavailable CPU query
  const auto draw = counters.Read()[static_cast<std::size_t>(PerfMetric::VulkanDrawSampled)];
  assert(draw.calls == 2 && draw.ns == 190 && draw.cpu_calls == 1 && draw.cpu_ns == 75);
  assert(PerformanceDelta(counters.Read(), initial)[static_cast<std::size_t>(PerfMetric::VulkanDrawSampled)].cpu_ns == 0);
  // Sampling is probabilistic, deterministic for tests, and does not select a
  // fixed draw index every frame. Raw sampled totals are not scaled in the log.
  PerfSampleGate gate(PERF_SAMPLE_SCALE_LEGACY);
  unsigned selected = 0;
  for (unsigned i = 0; i < 64000; ++i)
    selected += gate.Next();
  assert(selected > 850 && selected < 1150);
  SetPerfSampleScale(PERF_SAMPLE_SCALE_LEGACY);
  const auto sampled_before = GetPerfCounters().Read();
  for (unsigned i = 0; i < 64000; ++i)
  {
    const SampledPerfSample<PerfMetric::VertexDecodeSampled> timer;
  }
  const auto sampled = PerformanceDelta(sampled_before, GetPerfCounters().Read())[static_cast<std::size_t>(PerfMetric::VertexDecodeSampled)];
  assert(sampled.calls == selected);
#ifdef __linux__
  assert(sampled.cpu_calls == sampled.calls); // actual CLOCK_THREAD_CPUTIME_ID
#endif
  PerfSampleGate sparse_gate(PERF_SAMPLE_SCALE_DEFAULT);
  unsigned sparse_selected = 0;
  for (unsigned i = 0; i < 1024000; ++i)
    sparse_selected += sparse_gate.Next();
  assert(sparse_selected > 850 && sparse_selected < 1150);
  SetPerfSampleScale(PERF_SAMPLE_SCALE_DEFAULT);
  const auto sparse_before = GetPerfCounters().Read();
  for (unsigned i = 0; i < 1024000; ++i)
  {
    const SampledPerfSample<PerfMetric::TextureLoadSampled> timer;
  }
  const auto sparse = PerformanceDelta(sparse_before, GetPerfCounters().Read())
                          [static_cast<std::size_t>(PerfMetric::TextureLoadSampled)];
  assert(sparse.calls == sparse_selected);
  assert(GetPerfSampleScale() == PERF_SAMPLE_SCALE_DEFAULT);
  SetPerfSampleScale(128);
  assert(GetPerfSampleScale() == PERF_SAMPLE_SCALE_DEFAULT);
  // Whole periodic reports retain every line, including reports larger than
  // the legacy per-message 4096-byte buffer. Empty input is safe too.
  FILE* report_file = std::tmpfile();
  assert(report_file);
  const std::string long_line(5000, 'x');
  assert(WriteInfoReport(report_file, 42, "Performance scope: first\n" + long_line + "\nlast"));
  assert(WriteInfoReport(report_file, 43, {}));
  std::rewind(report_file);
  std::string saved_report(6000, '\0');
  saved_report.resize(std::fread(saved_report.data(), 1, saved_report.size(), report_file));
  assert(saved_report == "[42 ms] INFO: Performance scope: first\n[42 ms] INFO: " + long_line + "\n[42 ms] INFO: last\n");
  std::fclose(report_file);
#ifdef __linux__
  // Count actual stdio sink writes under the same buffer size, rather than
  // claiming a host timing ratio predicts Switch SD latency.
  struct Sink { std::string bytes; unsigned writes = 0; } individual, batched;
  const auto write_reports = [](Sink& sink, bool batch) {
    cookie_io_functions_t io{};
    io.write = [](void* cookie, const char* bytes, size_t count) -> ssize_t {
      auto& target = *static_cast<Sink*>(cookie);
      ++target.writes;
      target.bytes.append(bytes, count);
      return static_cast<ssize_t>(count);
    };
    FILE* file = fopencookie(&sink, "w", io);
    assert(file);
    char buffer[16384];
    assert(std::setvbuf(file, buffer, _IOFBF, sizeof(buffer)) == 0);
    std::string report;
    for (unsigned i = 0; i < 28; ++i)
    {
      const auto line = "Performance scope: heartbeat metric=synthetic boot_ms=" + std::to_string(i) + "\n";
      if (batch) report += line;
      else assert(WriteInfoReport(file, 42, line));
    }
    if (batch) assert(WriteInfoReport(file, 42, report));
    assert(std::fclose(file) == 0);
  };
  write_reports(individual, false);
  write_reports(batched, true);
  assert(individual.bytes == batched.bytes);
  assert(individual.writes == 28 && batched.writes == 1);
  std::cout << "PASS: identical 28-line timing report, 28 -> 1 stdio sink writes (host controlled buffer, not Switch latency)\n";
#endif
  const auto global_before = GetPerfCounters().Read();
  { const ScopedPerfSample timer(PerfMetric::DiscRead); }
  assert(PerformanceDelta(global_before, GetPerfCounters().Read())[static_cast<std::size_t>(PerfMetric::DiscRead)].calls == 1);
  ResetVulkanOverlapStats();
  const auto overlap_before = ReadVulkanOverlapStats();
  RecordVulkanWorkerQueued();
  RecordVulkanAcquireMode(true);
  RecordVulkanWorkerCompleted();
  RecordVulkanAcquireMode(false);
  const auto overlap_now = ReadVulkanOverlapStats();
  const auto overlap = VulkanOverlapDelta(overlap_before, overlap_now);
  assert(overlap.enqueued == 1 && overlap.completed == 1 && overlap.pending == 0);
  assert(overlap.acquire_overlaps == 1 && overlap.acquire_drains == 1 && overlap.peak_pending == 1);
  SetVulkanPresentOverlapEnabled(false);
  SetVulkanTripleFrameBufferingEnabled(false);
  assert(!IsVulkanPresentOverlapEnabled() && !IsVulkanTripleFrameBufferingEnabled());
  SetVulkanPresentOverlapEnabled(true);
  SetVulkanTripleFrameBufferingEnabled(true);
  assert(SelectVulkanPushDescriptorMode(false, true, true, 32) ==
         VulkanPushDescriptorDecision::DisabledBySetting);
  assert(SelectVulkanPushDescriptorMode(true, false, true, 32) ==
         VulkanPushDescriptorDecision::MissingExtension);
  assert(SelectVulkanPushDescriptorMode(true, true, false, 32) ==
         VulkanPushDescriptorDecision::MissingPropertiesQuery);
  assert(SelectVulkanPushDescriptorMode(true, true, true, 15) ==
         VulkanPushDescriptorDecision::InsufficientLimit);
  assert(SelectVulkanPushDescriptorMode(true, true, true, 16) ==
         VulkanPushDescriptorDecision::Enabled);
  SetVulkanPushDescriptorsRequested(false);
  assert(!AreVulkanPushDescriptorsRequested());
  SetVulkanPushDescriptorsRequested(true);
  assert(AreVulkanPushDescriptorsRequested());
  ResetVulkanDescriptorStats();
  const auto descriptors_before = ReadVulkanDescriptorStats();
  RecordVulkanPushDescriptorWrite();
  RecordVulkanLegacySamplerDescriptorWrite();
  const auto descriptors = VulkanDescriptorDelta(descriptors_before, ReadVulkanDescriptorStats());
  assert(descriptors.push_writes == 1 && descriptors.legacy_allocations == 1 &&
         descriptors.legacy_updates == 1);
  const ThreadUsage usage_before{42, 100, 1000, 0, 0, true};
  const ThreadUsage usage_after{42, 350, 2000, 2, 0, true};
  WorkerUsage worker_usage;
  assert(!worker_usage.Read().valid);
  std::thread worker_publisher([&] {
    for (unsigned i = 0; i < 2000; ++i)
      worker_usage.Publish({77, i, i * 10ULL, 1, 0, true});
  });
  for (unsigned i = 0; i < 2000; ++i)
  {
    const auto worker = worker_usage.Read();
    assert(!worker.valid || (worker.id == 77 && worker.wall_ns == worker.cpu_ns * 10));
  }
  worker_publisher.join();
  assert(ThreadBusyPercent(usage_before, usage_after) == 25.0); // migration preserves thread identity
  assert(ThreadBusyPercent({}, usage_after) == -1.0);
  assert(ThreadBusyPercent(usage_before, usage_before) == -1.0);
  assert(ThreadBusyPercent(usage_after, usage_before) == -1.0);
  assert(ThreadBusyPercent(usage_before, {43, 350, 2000, 1, 0, true}) == -1.0);
  assert(ThreadBusyPercent(usage_before, {42, 350, 2000, 0, 123, false}) == -1.0);
  // Multiple producer threads publish whole samples; snapshots must never mix
  // registers from different VI callbacks. Fresh instances reset every boot.
  BootProgress progress;
  assert(progress.Read().vi_fields == 0 && progress.Read().sampled_at == std::chrono::steady_clock::time_point{});
  std::thread cpu([&] {
    for (std::uint32_t i = 1; i <= 2000; ++i)
      progress.OnVI(i, i + 42, static_cast<std::uint64_t>(i) * 1000, 486000000,
                    {42, i, i * 10ULL, 1, 0, true});
  });
  std::thread gpu([&] {
    for (unsigned i = 0; i < 1000; ++i)
    {
      progress.OnXFB();
      progress.OnBeforePresent(640, 480, i, false);
      progress.OnAfterPresent(i, false, {43, i, i * 10ULL, 2, 0, true});
    }
  });
  for (unsigned i = 0; i < 2000; ++i)
  {
    const auto sample = progress.Read();
    if (sample.vi_fields)
    {
      assert(sample.r3 == sample.pc + 42 && sample.ticks == sample.pc * 1000ULL);
      assert(sample.cpu_usage.id == 42 && sample.cpu_usage.cpu_ns == sample.pc);
    }
    if (sample.presents_completed)
      assert(sample.gpu_usage.id == 43 && sample.gpu_usage.cpu_ns == sample.presents_completed - 1);
    assert(sample.presents_completed <= sample.presents_queued);
    assert(sample.presents_queued <= sample.xfb_copies);
  }
  cpu.join(); gpu.join();
  const auto completed = progress.Read();
  assert(completed.vi_fields == 2000 && completed.xfb_copies == 1000 && completed.presents_completed == 1000);
  assert(completed.unique_presents_completed == 1000 && completed.duplicate_presents_completed == 0);
  assert(completed.xfb_width == 640 && completed.xfb_height == 480);
  assert(BootProgress{}.Read().presents_queued == 0);
  // Exact cadence accounting uses a fixed callback-side ring, excludes duplicate
  // frames, and reports wrap loss rather than silently mixing old entries.
  BootProgress cadence;
  const auto cadence_start = std::chrono::steady_clock::time_point{} + std::chrono::seconds(1);
  for (std::uint64_t i = 0; i < 100; ++i)
  {
    cadence.OnBeforePresent(640, 480, i, false);
    cadence.OnAfterPresent(i, false, {}, cadence_start + std::chrono::milliseconds(i * 40));
    cadence.OnBeforePresent(640, 480, i, true); // suppressed duplicate: never completed
  }
  auto cadence_snapshot = cadence.Read();
  auto cadence_summary = SummarizeFrameCadence(cadence_snapshot, 0);
  assert(cadence_snapshot.unique_presents_completed == 100);
  assert(cadence_snapshot.duplicate_presents_queued == 100);
  assert(cadence_snapshot.duplicate_presents_completed == 0);
  assert(cadence_summary.samples == 99 && cadence_summary.lost_samples == 0);
  assert(cadence_summary.mean_ms == 40.0 && cadence_summary.p95_ms == 40.0 &&
         cadence_summary.p99_ms == 40.0 && cadence_summary.one_percent_low_fps == 25.0);
  const auto previous_sequence = cadence_snapshot.present_interval_sequence;
  cadence.OnAfterPresent(100, false, {}, cadence_start + std::chrono::milliseconds(5000));
  cadence_summary = SummarizeFrameCadence(cadence.Read(), previous_sequence);
  assert(cadence_summary.samples == 1 && cadence_summary.mean_ms == 1040.0);
  for (std::uint64_t i = 101; i <= 700; ++i)
    cadence.OnAfterPresent(i, false, {}, cadence_start + std::chrono::milliseconds(5000 + i));
  cadence_snapshot = cadence.Read();
  cadence_summary = SummarizeFrameCadence(cadence_snapshot, previous_sequence);
  assert(cadence_summary.samples == BootProgress::CADENCE_CAPACITY);
  assert(cadence_summary.lost_samples == 89);
  // Hardware regression: successful EGL queries returned 0x0 in switch Mesa.
  for (const SurfaceSize reported : {SurfaceSize{0, 0}, {1280, 0}, {0, 720}, {-1, 720}})
  {
    const auto resolved = ResolveSurfaceSize(true, reported, {1280, 720});
    assert(resolved.width == 1280 && resolved.height == 720);
  }
  const auto failed_query = ResolveSurfaceSize(false, {999, 999}, {1280, 720});
  assert(failed_query.width == 1280 && failed_query.height == 720);
  const auto valid_query = ResolveSurfaceSize(true, {1920, 1080}, {1280, 720});
  assert(valid_query.width == 1920 && valid_query.height == 1080);
  assert(!ResolveSurfaceSize(true, {}, {}).Valid());
  assert(!ResolveSurfaceSize(false, {1280, 720}, {}).Valid());
  const std::array<std::uint8_t, 4> green{26, 204, 128, 255}, background{8, 15, 26, 255};
  assert(TrianglePixelsMatch(green, background));
  assert(!TrianglePixelsMatch({}, {}));
  assert(!TrianglePixelsMatch(background, background)); // clear-only, no triangle
  assert(!TrianglePixelsMatch(green, green));           // no background
  assert(!TrianglePixelsMatch({128, 204, 26, 255}, background)); // channel swap
  std::tm epoch{}; epoch.tm_year = 70; epoch.tm_mday = 1;
  assert(HorizonTimegm(&epoch) == 0 && epoch.tm_wday == 4);
  std::tm leap{}; leap.tm_year = 100; leap.tm_mon = 1; leap.tm_mday = 29;
  assert(HorizonTimegm(&leap) == 951782400);
  std::tm rollover{}; rollover.tm_year = 69; rollover.tm_mon = 12; rollover.tm_mday = 1;
  assert(HorizonTimegm(&rollover) == 0);
  std::filesystem::create_directories(root / "folder");
  assert(IsGameImage("WINDWAKER.ISO"));
  assert(IsGameImage("twilight.gcm"));
  assert(IsGameImage("sample.RvZ"));
  assert(IsGameImage("selftest.dol"));
  assert(!IsGameImage("game.iso.txt"));
  assert(!IsGameImage("game.wbfs"));
  assert(IsSDPath("sdmc:/switch/dolphin/games/a game.iso"));
  assert(IsSDPath("sdmc:/"));
  assert(!IsSDPath("sdmc:/../escape"));
  assert(!IsSDPath("romfs:/test"));
  assert(!IsSDPath("sdmc:/switch/../../bad"));
  assert(!IsSDPath("sdmc:/other:/test"));
  assert(!IsSDPath("sdmc:/test\\bad"));
  assert(ParentSDPath("sdmc:/switch/dolphin/games/") == "sdmc:/switch/dolphin/");
  assert(ParentSDPath("sdmc:/switch") == "sdmc:/");
  assert(ParentSDPath("sdmc:/") == "sdmc:/");
  assert(DisplayText("file\x1b[2J\n") == "file?[2J?");
  Settings original;
  assert(!original.cached_interpreter && !original.request_jit);
  assert(original.native_vertex_loader);
  assert(!original.vulkan_present_overlap && original.vulkan_present_completion_guard &&
         !original.vulkan_triple_buffering && original.fifo_batching &&
          original.jit_small_code_pooling && original.horizon_ppc_fastmem &&
          original.phocoena_auto_clocks && !original.tp_benchmark_save &&
          original.horizon_vulkan_push_descriptors);
  assert(original.phocoena_clock_profile == "performance");
  assert(original.horizon_perf_sample_scale == PERF_SAMPLE_SCALE_DEFAULT);
  assert(original.EffectiveFifoBatchSize() == 8);
  Settings fifo_cycle;
  fifo_cycle.CycleFifoBatchSize();
  assert(fifo_cycle.EffectiveFifoBatchSize() == 1);
  fifo_cycle.CycleFifoBatchSize();
  assert(fifo_cycle.EffectiveFifoBatchSize() == 4);
  fifo_cycle.CycleFifoBatchSize();
  assert(fifo_cycle.EffectiveFifoBatchSize() == 8);
  fifo_cycle.CycleFifoBatchSize();
  assert(fifo_cycle.EffectiveFifoBatchSize() == 1);
  original.CycleCPU();
  assert(original.cached_interpreter && !original.request_jit);
  assert(std::string(original.CPUName()) == "Cached Interpreter (experimental)");
  original.CycleCPU();
  assert(!original.cached_interpreter && original.native_jit && !original.request_jit);
  assert(std::string(original.CPUName()) == "AArch64 JIT (experimental)");
  original.CycleCPU();
  assert(!original.cached_interpreter && !original.request_jit);
  original.request_jit = true; original.vsync = false; original.audio = false;
  original.dual_core = true; original.native_vertex_loader = false;
  const auto settings_path = (root / "switch.ini").string();
  assert(original.Save(settings_path));
  Settings loaded;
  assert(loaded.Load(settings_path));
  assert(loaded.request_jit && !loaded.vsync && !loaded.audio && loaded.dual_core);
  assert(!loaded.native_vertex_loader);
  assert(loaded.vulkan_submit_thread);
  loaded.vulkan_present_overlap = false;
  loaded.vulkan_present_completion_guard = false;
  loaded.vulkan_triple_buffering = false;
  loaded.horizon_vulkan_push_descriptors = false;
  loaded.vulkan_submit_thread = false;
  loaded.fifo_batching = false;
  loaded.fifo_batch_size = 16;
  loaded.jit_small_code_pooling = false;
  loaded.horizon_ppc_fastmem = false;
  loaded.phocoena_auto_clocks = false;
  loaded.phocoena_clock_profile = "legacy";
  loaded.tp_benchmark_save = true;
  loaded.horizon_perf_sample_scale = PERF_SAMPLE_SCALE_LEGACY;
  assert(!loaded.cached_interpreter && !loaded.native_jit); // legacy jit is not native opt-in
  loaded.native_jit = true;
  assert(loaded.Save(settings_path));
  Settings native;
  assert(native.Load(settings_path) && native.native_jit);
  assert(!native.vulkan_submit_thread); // explicit rollback survives save/reload
  assert(!native.vulkan_present_overlap && !native.vulkan_present_completion_guard &&
         !native.vulkan_triple_buffering && !native.horizon_vulkan_push_descriptors);
  assert(!native.fifo_batching);
  assert(native.EffectiveFifoBatchSize() == 1);
  assert(!native.jit_small_code_pooling);
  assert(!native.horizon_ppc_fastmem);
  assert(!native.phocoena_auto_clocks);
  assert(native.phocoena_clock_profile == "legacy");
  assert(native.tp_benchmark_save);
  assert(native.horizon_perf_sample_scale == PERF_SAMPLE_SCALE_LEGACY);
  loaded.native_jit = false;
  loaded.request_jit = false;
  loaded.cached_interpreter = true;
  loaded.vsync = true;
  assert(loaded.Save(settings_path)); // replacement of existing settings
  Settings cached;
  assert(cached.Load(settings_path) && cached.cached_interpreter && !cached.request_jit);
  const auto replacement = root / "replacement.tmp";
  std::ofstream(replacement) << "replacement";
  std::error_code rename_error;
  ReplaceFile(replacement, settings_path, rename_error, NoOverwriteRename);
  assert(!rename_error && !fs::exists(replacement));
  std::ofstream(replacement) << "must not replace";
  ReplaceFile(replacement, settings_path, rename_error, FailingReplacement);
  assert(rename_error && fs::exists(replacement));
  std::ifstream preserved(settings_path);
  std::string saved; std::getline(preserved, saved);
  assert(saved == "replacement");
  assert(!fs::exists(settings_path + ".horizon-old"));
  std::ofstream(root / "bad.ini") << "jit=nonsense\nvsync=0\r\naudio=2\nunknown=1\n";
  Settings bad;
  assert(bad.Load((root / "bad.ini").string()));
  assert(!bad.request_jit && !bad.vsync && bad.audio);
  assert(!bad.cached_interpreter); // legacy settings without the new key retain plain interpreter
  assert(bad.vulkan_submit_thread); // old configs select the new submission trial
  assert(bad.native_vertex_loader); // old configs select the new AArch64 vertex-loader trial
  assert(!bad.vulkan_present_overlap && bad.vulkan_present_completion_guard &&
         !bad.vulkan_triple_buffering && bad.fifo_batching &&
         bad.horizon_vulkan_push_descriptors);
  assert(bad.EffectiveFifoBatchSize() == 8); // v0.1.26 defaults to the hardware-safe ceiling
  assert(bad.horizon_ppc_fastmem);
  assert(bad.phocoena_auto_clocks);
  assert(bad.phocoena_clock_profile == "performance");
  assert(!bad.tp_benchmark_save);
  assert(bad.horizon_perf_sample_scale == PERF_SAMPLE_SCALE_DEFAULT);
  std::ofstream(root / "fifo.ini") << "fifo_batching=1\nfifo_batch_size=8\n";
  Settings legacy_fifo;
  assert(legacy_fifo.Load((root / "fifo.ini").string()));
  assert(legacy_fifo.fifo_batching && legacy_fifo.EffectiveFifoBatchSize() == 8);
  assert(std::string(legacy_fifo.FifoBatchName()) == "8 (hardware-safe)");
  std::ofstream(root / "fifo16.ini") << "fifo_batching=1\nfifo_batch_size=16\n";
  Settings migrated_fifo;
  assert(migrated_fifo.Load((root / "fifo16.ini").string()));
  assert(migrated_fifo.fifo_batching && migrated_fifo.EffectiveFifoBatchSize() == 8);
  assert(TP_CURRENT_STAGE_ADDRESS == 0x8040afc0);
  assert(TP_NEXT_STAGE_ADDRESS == 0x8040afce);
  assert(TP_RESTART_LAST_MODE_ADDRESS == 0x80406f90);
  assert(TP_WARP_POINTS.size() == 4);
  assert(std::string_view(TP_WARP_POINTS[0].stage.data()) == "F_SP121");
  assert(TP_WARP_POINTS[0].room == 10 && TP_WARP_POINTS[0].spawn == 0);
  const auto benchmark_template = root / "benchmark-template.gci";
  const auto benchmark_runtime = root / "benchmark-runtime.gci";
  const auto benchmark_staging = fs::path(benchmark_runtime.string() + ".tmp");
  const std::string benchmark_bytes = "protected benchmark template";
  std::ofstream(benchmark_template, std::ios::binary) << benchmark_bytes;
  std::ofstream(benchmark_runtime, std::ios::binary) << "old runtime";
  std::ofstream(benchmark_staging, std::ios::binary) << "stale staging";
  std::string benchmark_error;
  assert(ResetDisposableBenchmarkFile(benchmark_template, benchmark_runtime,
                                      benchmark_bytes.size(), benchmark_error));
  assert(benchmark_error.empty() && !fs::exists(benchmark_staging));
  std::ifstream benchmark_result(benchmark_runtime, std::ios::binary);
  assert(std::string(std::istreambuf_iterator<char>(benchmark_result), {}) == benchmark_bytes);
  // A second boot must be idempotent when the runtime file already exists.
  assert(ResetDisposableBenchmarkFile(benchmark_template, benchmark_runtime,
                                      benchmark_bytes.size(), benchmark_error));
  // A failed staging verification leaves the last valid runtime copy intact.
  assert(!ResetDisposableBenchmarkFile(benchmark_template, benchmark_runtime,
                                       benchmark_bytes.size() + 1, benchmark_error));
  std::ifstream benchmark_preserved(benchmark_runtime, std::ios::binary);
  assert(std::string(std::istreambuf_iterator<char>(benchmark_preserved), {}) == benchmark_bytes);
  assert(!ResetDisposableBenchmarkFile(benchmark_template, benchmark_template,
                                       benchmark_bytes.size(), benchmark_error));
  TPMenuState tp_menu;
  auto menu_result = tp_menu.Update({.toggle = true});
  assert(menu_result.changed && tp_menu.IsOpen() && tp_menu.Selected() == 0);
  menu_result = tp_menu.Update({.down = true});
  assert(menu_result.changed && tp_menu.Selected() == 1);
  menu_result = tp_menu.Update({.up = true});
  assert(menu_result.changed && tp_menu.Selected() == 0);
  menu_result = tp_menu.Update({.stick_y = -20000});
  assert(menu_result.changed && tp_menu.Selected() == 1);
  menu_result = tp_menu.Update({.stick_y = -20000});
  assert(!menu_result.changed && tp_menu.Selected() == 1);  // held-stick debounce
  tp_menu.Update({.stick_y = 0});
  menu_result = tp_menu.Update({.activate = true});
  assert(menu_result.warp == 1 && menu_result.closed && !tp_menu.IsOpen());
  tp_menu.Update({.toggle = true});
  menu_result = tp_menu.Update({.close = true});
  assert(menu_result.closed && !tp_menu.IsOpen());
  assert(!bad.Load((root / "missing.ini").string()));
  std::ofstream(root / "z.iso") << "not a real image";
  std::ofstream(root / "a.RVZ") << "not a real image";
  std::ofstream(root / "ignore.txt") << "text";
  std::string error;
  const auto entries = ReadDirectory(root.string(), error);
  assert(error.empty());
  assert(entries.size() == 3 && entries[0].directory && entries[0].name == "folder");
  assert(entries[1].name == "a.RVZ" && entries[2].name == "z.iso");
  ReadDirectory((root / "nonexistent").string(), error);
  assert(!error.empty());
  std::cout << "PASS: frontend, filesystem/UTC, EGL extent fallback, triangle readback, concurrent boot progress, performance accounting and thread usage\n";
}

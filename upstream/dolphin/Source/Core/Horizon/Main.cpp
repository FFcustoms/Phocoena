// SPDX-License-Identifier: GPL-2.0-or-later
#include <switch.h>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <thread>
#include <fmt/format.h>
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/MsgHandler.h"
#include "Core/Boot/Boot.h"
#include "Core/BootManager.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/GCPad.h"
#include "Core/HW/SystemTimers.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "DiscIO/Volume.h"
#include "DiscIO/Blob.h"
#include "DiscIO/Enums.h"
#include "Horizon/BuildInfo.h"
#include "Horizon/Audio.h"
#include "Horizon/BootProgress.h"
#include "Horizon/ClockControl.h"
#include "Horizon/Console.h"
#include "Horizon/Display.h"
#include "Horizon/FrontendModel.h"
#include "Horizon/Fastmem.h"
#include "Horizon/Input.h"
#include "Horizon/JitMemory.h"
#include "Horizon/Log.h"
#include "Horizon/Performance.h"
#include "Horizon/Renderer.h"
#include "Horizon/RuntimeDiagnostics.h"
#include "Horizon/SupportDiagnostics.h"
#include "Horizon/TwilightPrincessBenchmark.h"
#include "UICommon/UICommon.h"
#include "VideoCommon/VideoBackendBase.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/VideoEvents.h"
#include "VideoCommon/FifoBatch.h"
#include "VideoCommon/OnScreenDisplay.h"
#include "VideoCommon/PerformanceMetrics.h"

// NRO main stack is supplied by the loader; __stacksize__ does not resize it.
extern "C" bool HorizonOpenCrashLog();
extern "C" void HorizonCloseCrashLog();
namespace Horizon
{
extern std::atomic<bool> menu_active;
void RunDiagnostics(PadState& pad);
namespace
{
bool app_running = true;
bool PollRaw(PadState& pad)
{
  app_running = appletMainLoop();
  padUpdate(&pad);
  return app_running;
}
bool Poll(PadState& pad)
{
  PollRaw(pad);
  UpdateController(pad);
  return app_running;
}
void Header(const char* page)
{
  printf("\x1b[2J\x1b[H\n  Phocoena  v%s\n  -----------------------\n  %s\n\n", DOLPHIN_PORT_VERSION, page);
}
void WaitBack(PadState& pad)
{
  printf("\n  B: Back\n");
  while (Poll(pad) && !(padGetButtonsDown(&pad) & HidNpadButton_B)) consoleUpdate(nullptr);
}
bool Alert(const char* caption, const char* text, bool, Common::MsgType)
{
  // This can run on the emulation thread. Never draw a console over a GL surface,
  // and never block that thread waiting on UI. Questions conservatively return No.
  Error("Core alert", "%s: %s", caption, text);
  return false;
}
void ApplySettings(const Settings& settings, bool tp_benchmark_save_active)
{
  if (settings.request_jit && !settings.native_jit)
    Error("JIT", "legacy jit=1 does not opt into native execution; select AArch64 JIT explicitly in Settings");
  const bool cached = settings.cached_interpreter && !settings.request_jit;
  const auto cpu = settings.native_jit ? PowerPC::CPUCore::JITARM64 :
                   cached ? PowerPC::CPUCore::CachedInterpreter : PowerPC::CPUCore::Interpreter;
  Config::SetBase(Config::MAIN_CPU_CORE, cpu);
  const bool fastmem = settings.native_jit && settings.horizon_ppc_fastmem;
  SetPpcFastmemRequested(fastmem);
  Config::SetBase(Config::MAIN_FASTMEM, fastmem);
  Config::SetBase(Config::MAIN_PAGE_TABLE_FASTMEM, fastmem);
  Config::SetBase(Config::MAIN_FASTMEM_ARENA, fastmem);
  Config::SetBase(Config::MAIN_LARGE_ENTRY_POINTS_MAP, false);
  Config::SetBase(Config::MAIN_CPU_THREAD, settings.dual_core);
  Config::SetBase(Config::MAIN_DSP_HLE, true);
  Config::SetBase(Config::MAIN_DSP_JIT, false);
  Config::SetBase(Config::MAIN_SKIP_IPL, true);
  Config::SetBase(Config::MAIN_GCI_FOLDER_A_PATH_OVERRIDE,
                  tp_benchmark_save_active ? TPBenchmarkRuntimeGCIFolder(ROOT) : std::string{});
  Config::SetBase(Config::MAIN_GFX_BACKEND, std::string(RENDERER_ID));
  Config::SetBase(Config::MAIN_AUDIO_BACKEND,
                  std::string(settings.audio && AUDIO_AVAILABLE ? "Horizon AudioOut" :
                                                                  "No Audio Output"));
  Config::SetBase(Config::GFX_EFB_SCALE, 1);
  Config::SetBase(Config::GFX_MSAA, 1u);
  Config::SetBase(Config::GFX_SSAA, false);
  Config::SetBase(Config::GFX_VSYNC, settings.vsync);
  Config::SetBase(Config::GFX_SHADER_CACHE, settings.shader_cache);
  Config::SetBase(Config::GFX_SHADER_COMPILATION_MODE, ShaderCompilationMode::Synchronous);
  Config::SetBase(Config::GFX_SHADER_COMPILER_THREADS, 0);
  Config::SetBase(Config::GFX_SHADER_PRECOMPILER_THREADS, 0);
  Config::SetBase(Config::GFX_WAIT_FOR_SHADERS_BEFORE_STARTING, false);
  Config::SetBase(Config::GFX_VERTEX_LOADER_TYPE,
                  settings.native_vertex_loader ? VertexLoaderType::Native :
                                                  VertexLoaderType::Software);
  Config::SetBase(Config::GFX_SHOW_FPS, true);
  Config::SetBase(Config::GFX_SHOW_SPEED, true);
  Config::SetBase(Config::GFX_SHOW_SPEED_COLORS, false);
  Config::SetBase(Config::GFX_MOVABLE_PERFORMANCE_METRICS, false);
  SetSmallJitPoolingEnabled(settings.jit_small_code_pooling);
  SetFifoBatchBlocks(settings.EffectiveFifoBatchSize());
  SetPerfSampleScale(settings.horizon_perf_sample_scale);
#ifdef HORIZON_VULKAN
  SetVulkanPushDescriptorsRequested(settings.horizon_vulkan_push_descriptors);
#endif
  VideoBackendBase::ActivateBackend(RENDERER_ID);
  Log("CPU", "configured=%s dual_core=%d horizon_ppc_fastmem=%s DSP=HLE; "
      "runtime capability probe may fall back safely",
      settings.CPUName(), settings.dual_core, fastmem ? "on" : "off");
  Log("Phocoena clocks", "configured=%s profile=%s target_CPU=%s target_GPU=%s "
      "MEM=READ_ONLY; requests begin only with emulation",
      settings.phocoena_auto_clocks ? "on" : "off", settings.phocoena_clock_profile.c_str(),
      settings.phocoena_clock_profile == "legacy" ? "1581000000" : "1785000000",
      settings.phocoena_clock_profile == "legacy" ? "614400000" : "768000000");
  Log("TP benchmark save", "environment=%s GCI_folder_A=%s normal_GC_directory_touched=0",
      tp_benchmark_save_active ? "isolated-disposable" : "normal",
      tp_benchmark_save_active ? TPBenchmarkRuntimeGCIFolder(ROOT).c_str() : "default");
  if (settings.native_jit && settings.dual_core)
    Log("CPU", "experimental native dual-core enabled; CPU and graphics use separate upstream threads");
  Log("Graphics", "backend=%s internal=1x MSAA=1 vsync=%d shader_cache=%d", RENDERER_NAME, settings.vsync, settings.shader_cache);
  Log("Graphics", "vertex_loader=%s", settings.native_vertex_loader ? "AArch64 JIT" : "Software");
  Log("JIT memory", "small_code_pooling=%s pool_size=%zu max_pools=%zu "
      "(A/B setting jit_small_code_pooling)",
      settings.jit_small_code_pooling ? "on" : "off", SMALL_JIT_POOL_SIZE,
      MAX_SMALL_JIT_POOLS);
  Log("Graphics FIFO", "gather_batching=%s configured_blocks=%u max_supported=%u "
      "(A/B settings fifo_batching/fifo_batch_size)",
      settings.EffectiveFifoBatchSize() > 1 ? "on" : "off",
      settings.EffectiveFifoBatchSize(), Fifo::HORIZON_FIFO_MAX_BATCH_BLOCKS);
  Log("Performance", "hot_scope_sample_scale=1/%u legacy_A_B=64 "
      "(setting horizon_perf_sample_scale)", GetPerfSampleScale());
  Log("Audio", "requested=%s available=%d backend=libnx audout DSP=HLE",
      settings.audio ? "on" : "off", AUDIO_AVAILABLE);
#ifdef HORIZON_VULKAN
  Config::SetBase(Config::GFX_BACKEND_MULTITHREADING, settings.vulkan_submit_thread);
  SetVulkanPresentOverlapEnabled(settings.vulkan_present_overlap);
  SetVulkanPresentCompletionGuardEnabled(settings.vulkan_present_completion_guard);
  SetVulkanTripleFrameBufferingEnabled(settings.vulkan_triple_buffering);
  Config::SetBase(Config::GFX_ENHANCE_HDR_OUTPUT, false);
  Log("Vulkan submission", "worker=%s; CPU dual_core=%d is independent; shader workers remain off",
      settings.vulkan_submit_thread ? "on" : "off", settings.dual_core);
  Log("Vulkan descriptors", "gx_sampler_mode_requested=%s (A/B setting "
      "horizon_vulkan_push_descriptors; runtime capability probe may fall back safely)",
      settings.horizon_vulkan_push_descriptors ? "push" : "legacy-pooled");
  Log("Vulkan overlap", "present_overlap=%s completion_guard=%s frame_resources=%u "
      "(v0.1.13 failed mode: overlap=on guard=off frames=3)",
      settings.vulkan_present_overlap ? "on" : "off",
      settings.vulkan_present_completion_guard ? "on" : "off",
      settings.vulkan_triple_buffering ? 3u : 2u);
  setenv("MESA_SHADER_CACHE_DISABLE", settings.shader_cache ? "false" : "true", 1);
#endif
}
void SettingsMenu(PadState& pad, Settings& settings)
{
  size_t selected = 0;
  while (Poll(pad))
  {
    const char* on = "On"; const char* off = "Off";
    const std::vector<std::string> rows = {
      std::string("CPU requested: ") + settings.CPUName(),
      std::string("Vsync: ") + (settings.vsync ? on : off),
      std::string("Shader cache: ") + (settings.shader_cache ? on : off),
      std::string("Audio: ") + (settings.audio ? on : off),
      std::string("Dual core (experimental): ") + (settings.dual_core ? on : off),
      std::string("Vertex loader: ") + (settings.native_vertex_loader ? "AArch64 JIT" : "Software"),
      std::string("Small JIT CodeMemory pooling: ") +
          (settings.jit_small_code_pooling ? on : off),
      std::string("FIFO batch blocks: ") + settings.FifoBatchName(),
      std::string("Horizon PPC fastmem: ") + (settings.horizon_ppc_fastmem ? on : off),
      std::string("Phocoena auto clocks (CPU/GPU only): ") +
          (settings.phocoena_auto_clocks ? on : off),
      std::string("Phocoena clock profile: ") +
          (settings.phocoena_clock_profile == "legacy" ? "Legacy 1581/614" :
                                                           "Performance 1785/768"),
      std::string("TP benchmark save environment: ") +
          (settings.tp_benchmark_save ? "Benchmark" : "Normal"),
#ifdef HORIZON_VULKAN
      std::string("NXVK GX sampler descriptors: ") +
          (settings.horizon_vulkan_push_descriptors ? "Push (candidate)" : "Legacy pooled"),
      std::string("Vulkan submit worker (experimental): ") + (settings.vulkan_submit_thread ? on : off),
      std::string("Acquire/present overlap: ") + (settings.vulkan_present_overlap ? on : off),
      std::string("Present completion guard: ") +
          (settings.vulkan_present_completion_guard ? "Safe" : "UNSAFE v0.1.13"),
      std::string("Vulkan frame resources: ") + (settings.vulkan_triple_buffering ? "3" : "2"),
#endif
      "Save and return"};
    const u64 down = padGetButtonsDown(&pad);
    if (down & HidNpadButton_Up) selected = (selected + rows.size() - 1) % rows.size();
    if (down & HidNpadButton_Down) selected = (selected + 1) % rows.size();
    Header(RENDERER_SETTINGS);
    for (size_t i = 0; i < rows.size(); ++i) printf("  %c %s\n", i == selected ? '>' : ' ', rows[i].c_str());
    printf("\n  Tester baseline LOCKED.  B: Back\n\n  Controller mapping: config/GCPadNew.ini\n"
           "  CPU/GPU settings are fixed for comparable tester results; MEM is never written.\n");
    if ((down & HidNpadButton_B) || ((down & HidNpadButton_A) && selected == rows.size() - 1))
    {
      // Re-assert and save the baseline on exit in case an old config was loaded.
      settings.ApplyFrozenTesterBaseline();
      if (!settings.Save(std::string(ROOT) + "config/switch.ini"))
        Error("Settings", "failed writing frozen tester baseline to switch.ini");
      return;
    }
    // All tester-facing emulator settings are intentionally read-only. This keeps
    // every support bundle comparable to the same validated runtime baseline.
    consoleUpdate(nullptr);
  }
}
void BootGame(const std::string& path, PadState& pad, const Settings& settings)
{
  ClearLastError();
  Header("Preparing GameCube boot");
  Log("Boot stage 1", "selected %s", path.c_str());
  if (!IsSDPath(path)) { Error("Browser", "refused path outside SD"); return; }
  for (const char* resource : {"GC/font_western.bin", "GC/font_japanese.bin", "Resources/OSD_Font.ttf"})
  {
    const auto file = std::string(ROOT) + "Sys/" + resource;
    if (!File::Exists(file))
    {
      Error("Resources", "missing %s; copy the complete release Sys directory alongside Phocoena.nro", file.c_str());
      printf("  %s\n", LastError().c_str()); WaitBack(pad); return;
    }
  }
  auto extension = std::filesystem::path(path).extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return std::tolower(c); });
  std::string game_id;
  std::string internal_name;
  std::string region = "unknown";
  std::string image_format = extension == ".dol" ? "DOL" : (extension == ".elf" ? "ELF" : "unknown");
  int revision = -1;
  int disc_number = -1;
  if (extension != ".dol" && extension != ".elf")
  {
    auto volume = DiscIO::CreateVolume(path);
    if (!volume || volume->GetVolumeType() != DiscIO::Platform::GameCubeDisc)
    {
      Error("DiscIO", "not a readable GameCube image; Wii boot is deferred");
      printf("  %s\n", LastError().c_str()); WaitBack(pad); return;
    }
    game_id = volume->GetGameID();
    internal_name = volume->GetInternalName();
    region = DiscIO::GetName(volume->GetRegion(), false);
    image_format = DiscIO::GetName(volume->GetBlobType(), false);
    if (const auto value = volume->GetRevision()) revision = static_cast<int>(*value);
    if (const auto value = volume->GetDiscNumber()) disc_number = static_cast<int>(*value);
    Log("DiscIO", "GameCube id=%s internal_name=%s region=%s revision=%d disc=%d format=%s validated",
        game_id.c_str(), internal_name.c_str(), region.c_str(), revision, disc_number,
        image_format.c_str());
  }
  const bool tp_benchmark_save_active = settings.tp_benchmark_save && game_id == "GZ2E01";
  if (settings.tp_benchmark_save && !tp_benchmark_save_active)
  {
    Log("TP benchmark save",
        "requested for game id=%s; benchmark environment is GZ2E01-only, continuing with normal save environment",
        game_id.empty() ? "executable" : game_id.c_str());
  }
  if (tp_benchmark_save_active)
  {
    std::string error;
    if (!PrepareTPBenchmarkSaveEnvironment(ROOT, error))
    {
      Error("TP benchmark save", "%s", error.c_str());
      printf("  %s\n", LastError().c_str()); WaitBack(pad); return;
    }
    Log("TP benchmark save", "template copied to disposable runtime card; normal save remains isolated");
  }
  ApplySettings(settings, tp_benchmark_save_active);
  Log("Boot stage 2", "BootParameters::GenerateFromFile");
  auto boot = BootParameters::GenerateFromFile(path);
  if (!boot)
  {
    Error("Boot", "GenerateFromFile rejected the selected file");
    printf("  %s\n", LastError().c_str()); WaitBack(pad); return;
  }
  if (const auto* executable = std::get_if<BootParameters::Executable>(&boot->parameters))
  {
    // GenerateFromFile can return a BootParameters object with an invalid reader.
    // Validate before handing the console to GL so malformed files get a useful error.
    if (!executable->reader || !executable->reader->IsValid())
    {
      Error("Boot executable", "upstream core rejected %s (%llu bytes): invalid DOL/ELF or truncated section data; inspect reader messages above",
            path.c_str(), static_cast<unsigned long long>(File::GetSize(path)));
      printf("  %s\n", DisplayText(LastError()).c_str()); WaitBack(pad); return;
    }
    if (executable->reader->IsWii())
    {
      Error("Boot executable", "Wii DOL/ELF detected; v0.1 targets GameCube homebrew");
      printf("  %s\n", LastError().c_str()); WaitBack(pad); return;
    }
    Log("Boot executable", "reader valid; GameCube; size=%llu entry=0x%08x",
        static_cast<unsigned long long>(File::GetSize(path)), executable->reader->GetEntryPoint());
  }
  const auto clock_profile = settings.phocoena_clock_profile == "legacy" ?
                                 PhocoenaClock::Profile::Legacy :
                                 PhocoenaClock::Profile::Performance;
  const auto clock_targets = PhocoenaClock::TargetsForProfile(clock_profile);
  SupportSessionMetadata support_metadata{
      .game_id = game_id.empty() ? "EXEC" : game_id,
      .game_name = std::filesystem::path(path).filename().string(),
      .internal_name = internal_name,
      .region = region,
      .image_format = image_format,
      .source_extension = extension,
      .revision = revision,
      .disc_number = disc_number,
      .renderer = RENDERER_NAME,
      .cpu_mode = settings.CPUName(),
      .shader_mode = "Sync specialized",
      .fifo_blocks = settings.EffectiveFifoBatchSize(),
      .requested_cpu_hz = clock_targets.cpu_hz,
      .requested_gpu_hz = clock_targets.gpu_hz,
      .memory_clock_written = false,
  };
  std::string support_error;
  if (!BeginSupportSession(support_metadata, &support_error))
    Error("Support diagnostics", "could not begin per-game session: %s", support_error.c_str());

  printf("  Starting Phocoena core...\n  Hold Plus + Minus to stop.\n");
  consoleUpdate(nullptr);
  stop_requested = false;
  menu_active = false;
  AutoClockSession auto_clocks;
  Log("Phocoena clocks", "configured profile=%s auto=%d CPU_target_hz=%u GPU_target_hz=%u "
      "MEM_policy=READ_ONLY",
      PhocoenaClock::ProfileName(clock_profile).data(), settings.phocoena_auto_clocks,
      clock_targets.cpu_hz, clock_targets.gpu_hz);
  const auto& clock_report = auto_clocks.Start(settings.phocoena_auto_clocks, clock_profile);
  const auto effective_memory_hz = clock_report.effective.memory_valid ?
                                       clock_report.effective.memory_hz : clock_report.before.memory_hz;
  UpdateSupportClockReadings(clock_report.selected_cpu_hz, clock_report.selected_gpu_hz,
                             clock_report.effective.cpu_hz, clock_report.effective.cpu_valid,
                             clock_report.effective.gpu_hz, clock_report.effective.gpu_valid,
                             effective_memory_hz,
                             clock_report.effective.memory_valid || clock_report.before.memory_valid,
                             clock_report.mechanism, clock_report.external_manager);
  SuspendConsole();
  auto& system = Core::System::GetInstance();
  const auto boot_started = std::chrono::steady_clock::now();
  const auto swaps_before_boot = GetGLSwapCount();
  BootProgress progress;
  ResetVulkanOverlapStats();
  ResetVulkanDescriptorStats();
  ResetFifoBatchStats();
  ResetPpcFastmemStats();
  ResetRuntimeDiagnostics();
  auto overlap_previous = ReadVulkanOverlapStats();
  auto descriptor_previous = ReadVulkanDescriptorStats();
  auto fifo_batch_previous = ReadFifoBatchStats();
  auto progress_previous = progress.Read();
  const auto perf_at_boot = GetPerfCounters().Read();
  auto perf_previous = perf_at_boot;
  const auto vertex_at_boot = ReadVertexWork();
  auto vertex_previous = vertex_at_boot;
  auto perf_previous_time = boot_started;
  ThreadUsage previous_cpu, previous_gpu, previous_worker;
  const auto boot_wall_ns = armTicksToNs(armGetSystemTick());
  double last_guest_ms = 0.0;
  // Hooks live until after Shutdown joins all emulation threads. In particular
  // the host never takes CPUThreadGuard while waiting on a possibly stuck GPU.
  auto& events = system.GetVideoEvents();
  auto vi_hook = events.vi_end_field_event.Register([&] {
    const auto& ppc = system.GetPPCState();
    progress.OnVI(ppc.pc, ppc.gpr[3], system.GetCoreTiming().GetTicks(),
                  system.GetSystemTimers().GetTicksPerSecond(), ReadCurrentThreadUsage());
  });
  auto xfb_hook = events.after_frame_event.Register([&](Core::System&) { progress.OnXFB(); });
  auto before_present_hook = events.before_present_event.Register([&](PresentInfo& info) {
    progress.OnBeforePresent(
        info.frame_buffer_width, info.frame_buffer_height, info.frame_count,
        info.reason == PresentInfo::PresentReason::VideoInterfaceDuplicate);
  });
  auto after_present_hook = events.after_present_event.Register([&](PresentInfo& info) {
    progress.OnAfterPresent(
        info.frame_count, info.reason == PresentInfo::PresentReason::VideoInterfaceDuplicate,
        ReadCurrentThreadUsage());
  });
  const auto report_progress = [&](const char* phase) {
    const auto sample = progress.Read();
    const auto now = std::chrono::steady_clock::now();
    const auto wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - boot_started).count();
    const auto age_ms = sample.vi_fields ?
        std::chrono::duration_cast<std::chrono::milliseconds>(now - sample.sampled_at).count() : -1;
    const double guest_ms = sample.ticks_per_second ?
        static_cast<double>(sample.ticks) * 1000.0 / sample.ticks_per_second : 0.0;
    last_guest_ms = guest_ms;
    Log("Boot progress", "%s wall_ms=%lld core_state=%d VI=%llu guest_ms=%.1f "
        "sample_age_ms=%lld PC=0x%08x r3=0x%08x XFB_copies=%llu "
        "presents_queued=%llu presents_completed=%llu XFB=%ux%u EGL_swaps=%llu",
        phase, static_cast<long long>(wall_ms), static_cast<int>(Core::GetState(system)),
        static_cast<unsigned long long>(sample.vi_fields), guest_ms,
        static_cast<long long>(age_ms), sample.pc, sample.r3,
        static_cast<unsigned long long>(sample.xfb_copies),
        static_cast<unsigned long long>(sample.presents_queued),
        static_cast<unsigned long long>(sample.presents_completed), sample.xfb_width, sample.xfb_height,
        static_cast<unsigned long long>(GetGLSwapCount() - swaps_before_boot));
    const auto perf_now = GetPerfCounters().Read();
    const auto delta = PerformanceDelta(perf_previous, perf_now);
    const auto total = PerformanceDelta(perf_at_boot, perf_now);
    const auto interval_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - perf_previous_time).count();
    const auto gpu_sample_age_ms = sample.gpu_usage.valid ?
        static_cast<long long>((armTicksToNs(armGetSystemTick()) - sample.gpu_usage.wall_ns) / 1000000) : -1LL;
    const auto cpu_busy_pct = ThreadBusyPercent(previous_cpu, sample.cpu_usage);
    const auto gpu_busy_pct = ThreadBusyPercent(previous_gpu, sample.gpu_usage);
    const bool same_cpu_gpu_thread = sample.cpu_usage.valid && sample.gpu_usage.valid &&
                                     sample.cpu_usage.id == sample.gpu_usage.id;
    Log("Performance threads", "%s interval_ms=%lld CPU_id=%llu core=%u busy_pct=%.1f result=0x%08x "
        "GPU_id=%llu core=%u busy_pct=%.1f result=0x%08x same_thread=%d GPU_sample_age_ms=%lld",
        phase, static_cast<long long>(interval_ms), static_cast<unsigned long long>(sample.cpu_usage.id),
        sample.cpu_usage.core, cpu_busy_pct, sample.cpu_usage.result,
        static_cast<unsigned long long>(sample.gpu_usage.id), sample.gpu_usage.core,
        gpu_busy_pct, sample.gpu_usage.result, same_cpu_gpu_thread, gpu_sample_age_ms);
    const std::string support_thread_report = fmt::format(
        "Performance threads: {} interval_ms={} CPU_id={} core={} busy_pct={:.1f} result=0x{:08x} "
        "GPU_id={} core={} busy_pct={:.1f} result=0x{:08x} same_thread={} GPU_sample_age_ms={}\n",
        phase, interval_ms, sample.cpu_usage.id, sample.cpu_usage.core, cpu_busy_pct,
        sample.cpu_usage.result, sample.gpu_usage.id, sample.gpu_usage.core, gpu_busy_pct,
        sample.gpu_usage.result, same_cpu_gpu_thread, gpu_sample_age_ms);
    auto worker = GetVulkanWorkerUsage().Read();
    if (worker.wall_ns < boot_wall_ns)
      worker = {}; // no worker sample from this boot (including worker Off)
    const double worker_busy_pct = ThreadBusyPercent(previous_worker, worker);
    std::string report = fmt::format(
        "Performance worker: {} id={} core={} busy_pct={:.1f} result=0x{:08x} sample_age_ms={}\n",
        phase, worker.id, worker.core, worker_busy_pct, worker.result,
        worker.valid ? static_cast<long long>((armTicksToNs(armGetSystemTick()) - worker.wall_ns) / 1000000) : -1LL);
    const auto vertex_now = ReadVertexWork();
    const auto vertex_delta = VertexWorkDelta(vertex_previous, vertex_now);
    const auto vertex_total = VertexWorkDelta(vertex_at_boot, vertex_now);
    const auto jit_memory = ReadJitMemoryStats();
    const auto fastmem_stats = ReadPpcFastmemStats();
    const auto overlap_now = ReadVulkanOverlapStats();
    const auto overlap_delta = VulkanOverlapDelta(overlap_previous, overlap_now);
    const auto descriptor_now = ReadVulkanDescriptorStats();
    const auto descriptor_delta = VulkanDescriptorDelta(descriptor_previous, descriptor_now);
    const auto fifo_batch_now = ReadFifoBatchStats();
    const auto fifo_batch_delta = FifoBatchDelta(fifo_batch_previous, fifo_batch_now);
    const auto cadence = SummarizeFrameCadence(sample, progress_previous.present_interval_sequence);
    const auto count_delta = [](std::uint64_t previous, std::uint64_t current) {
      return current >= previous ? current - previous : 0;
    };
    report += fmt::format(
        "Performance vertex: {} interval_calls={} interval_vertices={} boot_calls={} boot_vertices={} "
        "interval_native_fallbacks={} boot_native_fallbacks={}\n"
        "Performance jit_memory: {} allocations={} releases={} failures={} active={} peak_active={} "
        "pooled_allocations={} pool_pages_active={} pool_pages_peak={} "
        "kernel_regions_active={} kernel_regions_peak={}\n",
        phase, vertex_delta.calls, vertex_delta.vertices, vertex_total.calls, vertex_total.vertices,
        vertex_delta.native_fallbacks, vertex_total.native_fallbacks, phase, jit_memory.allocations,
        jit_memory.releases, jit_memory.failures, jit_memory.active, jit_memory.peak_active,
        jit_memory.pooled_allocations, jit_memory.pooled_pages_active, jit_memory.pooled_pages_peak,
        jit_memory.kernel_regions_active, jit_memory.kernel_regions_peak);
    report += fmt::format(
        "Performance ppc_fastmem: {} requested={} supported={} active={} read_only={} "
        "compiled_direct_sites={} compiled_fallback_sites={} compiled_translated_sites={} "
        "compiled_mmio_sites={} runtime_slow_accesses={} runtime_mmio_accesses={} "
        "faults_received={} faults_handled={} faults_unhandled={} mappings_created={} "
        "mappings_released={} mapping_failures={}\n",
        phase, IsPpcFastmemRequested(), IsPpcFastmemArenaSupported(),
        IsPpcFastmemArenaActive(), ArePpcFastmemReadOnlyMappingsSupported(),
        fastmem_stats.compiled_direct_sites, fastmem_stats.compiled_fallback_sites,
        fastmem_stats.compiled_translated_sites, fastmem_stats.compiled_mmio_sites,
        fastmem_stats.runtime_slow_accesses, fastmem_stats.runtime_mmio_accesses,
        fastmem_stats.faults_received, fastmem_stats.faults_handled,
        fastmem_stats.faults_unhandled, fastmem_stats.mappings_created,
        fastmem_stats.mappings_released, fastmem_stats.mapping_failures);
    report += fmt::format(
        "Performance cadence: {} interval_unique_queued={} interval_duplicate_queued={} "
        "interval_unique_presented={} interval_duplicate_presented={} interval_samples={} "
        "interval_lost_samples={} mean_ms={:.3f} p95_ms={:.3f} p99_ms={:.3f} "
        "one_percent_low_fps={:.2f} interval_frame_sequence_gaps={} "
        "boot_unique_presented={} boot_duplicate_presented={} boot_frame_sequence_gaps={}\n",
        phase,
        count_delta(progress_previous.unique_presents_queued, sample.unique_presents_queued),
        count_delta(progress_previous.duplicate_presents_queued, sample.duplicate_presents_queued),
        count_delta(progress_previous.unique_presents_completed,
                    sample.unique_presents_completed),
        count_delta(progress_previous.duplicate_presents_completed,
                    sample.duplicate_presents_completed),
        cadence.samples, cadence.lost_samples, cadence.mean_ms, cadence.p95_ms, cadence.p99_ms,
        cadence.one_percent_low_fps,
        count_delta(progress_previous.frame_sequence_gaps, sample.frame_sequence_gaps),
        sample.unique_presents_completed, sample.duplicate_presents_completed,
        sample.frame_sequence_gaps);
    report += fmt::format(
        "Performance vulkan_overlap: {} interval_enqueued={} interval_completed={} "
        "interval_acquire_overlap={} interval_acquire_drain={} interval_acquire_guard={} "
        "pending={} peak_pending={} "
        "boot_enqueued={} boot_completed={}\n",
        phase, overlap_delta.enqueued, overlap_delta.completed, overlap_delta.acquire_overlaps,
        overlap_delta.acquire_drains, overlap_delta.acquire_guards, overlap_delta.pending,
        overlap_delta.peak_pending,
        overlap_now.enqueued, overlap_now.completed);
    report += fmt::format(
        "Performance vulkan_descriptors: {} interval_push_writes={} "
        "interval_legacy_allocations={} interval_legacy_updates={} "
        "boot_push_writes={} boot_legacy_allocations={} boot_legacy_updates={}\n",
        phase, descriptor_delta.push_writes, descriptor_delta.legacy_allocations,
        descriptor_delta.legacy_updates, descriptor_now.push_writes,
        descriptor_now.legacy_allocations, descriptor_now.legacy_updates);
    report += fmt::format(
        "Performance fifo_batch: {} configured_blocks={} interval_decoder_calls={} "
        "interval_gather_blocks={} interval_average_blocks={:.2f} max_blocks={} "
        "boot_decoder_calls={} boot_gather_blocks={}\n",
        phase, GetFifoBatchBlocks(), fifo_batch_delta.decoder_calls, fifo_batch_delta.gather_blocks,
        fifo_batch_delta.decoder_calls ?
            static_cast<double>(fifo_batch_delta.gather_blocks) / fifo_batch_delta.decoder_calls :
            0.0,
        fifo_batch_delta.max_blocks, fifo_batch_now.decoder_calls, fifo_batch_now.gather_blocks);
    for (std::size_t i = 0; i < PERF_METRICS; ++i)
    {
      report += fmt::format(
          "Performance scope: {} metric={} interval_calls={} interval_ms={:.3f} boot_calls={} boot_ms={:.3f} "
          "interval_cpu_calls={} interval_cpu_ms={:.3f} boot_cpu_calls={} boot_cpu_ms={:.3f} sample_scale={}\n",
          phase, PERF_NAMES[i], delta[i].calls, delta[i].ns / 1e6, total[i].calls, total[i].ns / 1e6,
          delta[i].cpu_calls, delta[i].cpu_ns / 1e6, total[i].cpu_calls, total[i].cpu_ns / 1e6,
          IsSampledMetric(static_cast<PerfMetric>(i)) ? GetPerfSampleScale() : 1);
    }
    LogReport(report);
    const auto audio_stats = ReadAudioDiagnosticsStats();
    const auto failure_stats = ReadRuntimeFailureStats();
    const auto& shader_compile = delta[static_cast<std::size_t>(PerfMetric::ShaderCompile)];
    const auto& pipeline_create = delta[static_cast<std::size_t>(PerfMetric::PipelineCreate)];
    const auto& jit_compile = delta[static_cast<std::size_t>(PerfMetric::JitCompile)];
    const auto& vertex_loader_jit =
        delta[static_cast<std::size_t>(PerfMetric::VertexLoaderJitCompile)];
    const auto& disc_read = delta[static_cast<std::size_t>(PerfMetric::DiscRead)];
    const auto& disc_wait = delta[static_cast<std::size_t>(PerfMetric::DiscWait)];
    const auto& gpu_wait = delta[static_cast<std::size_t>(PerfMetric::GpuWait)];
    const auto& fifo_sync = delta[static_cast<std::size_t>(PerfMetric::FifoSync)];
    const auto& vk_fence_wait = delta[static_cast<std::size_t>(PerfMetric::VulkanFenceWait)];
    const auto& vk_present_guard = delta[static_cast<std::size_t>(PerfMetric::VulkanPresentGuard)];
    const auto& vk_frame_reuse = delta[static_cast<std::size_t>(PerfMetric::VulkanFrameReuse)];
    const auto& log_write = delta[static_cast<std::size_t>(PerfMetric::LogWrite)];
    const SupportTelemetryMetrics support_metrics{
        .performance_window = std::string_view(phase) == "heartbeat",
        .gmsp_percent = system.GetPerfMetrics().GetSpeed() * 100.0,
        .fps = system.GetPerfMetrics().GetFPS(),
        .cpu_busy_percent = cpu_busy_pct,
        .gpu_busy_percent = gpu_busy_pct,
        .frame_mean_ms = cadence.mean_ms,
        .frame_p95_ms = cadence.p95_ms,
        .frame_p99_ms = cadence.p99_ms,
        .one_percent_low_fps = cadence.one_percent_low_fps,
        .vulkan_worker_busy_percent = worker_busy_pct,
        .shader_compile_calls = shader_compile.calls,
        .shader_compile_ms = shader_compile.ns / 1e6,
        .pipeline_create_calls = pipeline_create.calls,
        .pipeline_create_ms = pipeline_create.ns / 1e6,
        .jit_compile_calls = jit_compile.calls,
        .jit_compile_ms = jit_compile.ns / 1e6,
        .vertex_loader_jit_calls = vertex_loader_jit.calls,
        .vertex_loader_jit_ms = vertex_loader_jit.ns / 1e6,
        .disc_read_calls = disc_read.calls,
        .disc_read_ms = disc_read.ns / 1e6,
        .disc_wait_calls = disc_wait.calls,
        .disc_wait_ms = disc_wait.ns / 1e6,
        .gpu_wait_calls = gpu_wait.calls,
        .gpu_wait_ms = gpu_wait.ns / 1e6,
        .fifo_sync_calls = fifo_sync.calls,
        .fifo_sync_ms = fifo_sync.ns / 1e6,
        .vk_fence_wait_calls = vk_fence_wait.calls,
        .vk_fence_wait_ms = vk_fence_wait.ns / 1e6,
        .vk_present_guard_calls = vk_present_guard.calls,
        .vk_present_guard_ms = vk_present_guard.ns / 1e6,
        .vk_frame_reuse_calls = vk_frame_reuse.calls,
        .vk_frame_reuse_ms = vk_frame_reuse.ns / 1e6,
        .log_write_calls = log_write.calls,
        .log_write_ms = log_write.ns / 1e6,
        .fifo_decoder_calls = fifo_batch_delta.decoder_calls,
        .fifo_gather_blocks = fifo_batch_delta.gather_blocks,
        .guest_gpu_wait_timeouts = failure_stats.gpu_wait_timeouts,
        .guest_pi_fifo_resets = failure_stats.pi_fifo_resets,
        .guest_pi_reset_codes = failure_stats.pi_reset_codes,
        .last_guest_pi_reset_code = failure_stats.last_pi_reset_code,
        .audio_release_events = audio_stats.release_events,
        .audio_requeues = audio_stats.requeues,
        .audio_wait_failures = audio_stats.wait_failures,
        .audio_append_failures = audio_stats.append_failures,
    };
    AppendSupportTelemetry(support_thread_report + report, wall_ms, guest_ms,
                           sample.unique_presents_completed,
                           static_cast<int>(Core::GetState(system)), support_metrics);
    previous_cpu = sample.cpu_usage;
    previous_gpu = sample.gpu_usage;
    previous_worker = worker;
    vertex_previous = vertex_now;
    perf_previous = perf_now;
    overlap_previous = overlap_now;
    descriptor_previous = descriptor_now;
    fifo_batch_previous = fifo_batch_now;
    progress_previous = sample;
    perf_previous_time = now;
  };
  Log("Boot progress", "CPU samples come from VI on CPU thread; age=-1 means no sample. "
      "Presents/EGL swaps do not prove visible pixels; EGL includes loading screens.");
  Log("Performance", "scope times are inclusive wall time of completed calls, not GPU hardware time; "
      "overlapping scopes/threads must not be added. Thread busy=-1 means unavailable/first sample; "
      "CPU/GPU are the same thread in single-core. No per-frame SD logging; "
      "10 Hz CP/FIFO failure history is RAM-only until a critical event.");
  Log("Performance", "v0.1.23: *_sampled are raw 1/%u-probability samples, not exact totals; "
      "CPU times are OS thread execution, valid only when cpu_calls matches calls; "
      "CPU role includes JIT, HLE and core services, not pure JIT execution. "
      "Vertex and FIFO batch counts are exact relaxed counters; periodic scope lines share one flush. "
      "PPC fastmem compiled-site counters are JIT generation counts, not dynamic access totals; "
      "runtime_slow/mmio count only helper calls while the fastmem trial is requested. "
      "vk_present_guard is the exact CPU-side WSI ownership wait; GPU resource reuse remains "
      "protected by command-buffer fences. Cadence counts only unique delivered frames and "
      "uses a fixed 512-interval ring; lost_samples reports reporter overrun.",
      GetPerfSampleScale());
  const WindowSystemInfo wsi{WindowSystemType::Horizon, nullptr, nwindowGetDefault(), nwindowGetDefault()};
  auto state_hook = Core::AddOnStateChangedCallback([](Core::State state) {
    const char* name = "unknown";
    switch (state) {
      case Core::State::Uninitialized: name = "Uninitialized"; stop_requested = true; break;
      case Core::State::Starting: name = "Starting"; break;
      case Core::State::Running: name = "Running"; break;
      case Core::State::Paused: name = "Paused"; break;
      case Core::State::Stopping: name = "Stopping"; break;
    }
    Log("Core state", "%s", name);
  });
  Log("Boot stage 3", "BootManager::BootCore (CPU/GPU init runs asynchronously)");
  bool accepted = BootManager::BootCore(system, std::move(boot), wsi);
  if (!accepted)
  {
    // Keep any specific core failure as the menu's Last error.
    if (LastError().empty()) Error("Boot", "BootCore returned false; inspect boot-stage messages above");
    else Log("Boot", "BootCore returned false; retaining the preceding subsystem error");
  }
  TPMenuState tp_menu;
  auto heartbeat = std::chrono::steady_clock::now();
  bool frontend_hotkey_exit = false;
  while (accepted && !stop_requested && PollRaw(pad))
  {
    const auto held = padGetButtons(&pad);
    if ((held & (HidNpadButton_Plus | HidNpadButton_Minus)) == (HidNpadButton_Plus | HidNpadButton_Minus))
    {
      frontend_hotkey_exit = true;
      stop_requested = true;
    }
    u64 suppressed_buttons = 0;
    bool suppress_sticks = false;
    if (tp_benchmark_save_active && !stop_requested)
    {
      const u64 down = padGetButtonsDown(&pad);
      const bool was_open = tp_menu.IsOpen();
      const auto stick = padGetStickPos(&pad, 0);
      const TPMenuInput input{
          .toggle = (down & HidNpadButton_Minus) != 0,
          .up = (down & HidNpadButton_Up) != 0,
          .down = (down & HidNpadButton_Down) != 0,
          .activate = (down & HidNpadButton_A) != 0,
          .close = (down & HidNpadButton_B) != 0,
          .stick_y = stick.y,
      };
      const TPMenuResult result = tp_menu.Update(input);
      const bool capture = was_open || input.toggle || tp_menu.IsOpen();
      if (capture)
      {
        suppressed_buttons = HidNpadButton_Minus | HidNpadButton_A | HidNpadButton_B |
                             HidNpadButton_Up | HidNpadButton_Down | HidNpadButton_Left |
                             HidNpadButton_Right;
        suppress_sticks = true;
      }
      if (result.changed)
      {
        if (tp_menu.IsOpen())
          ShowTPDeveloperMenu(tp_menu);
        else
          HideTPDeveloperMenu();
      }
      if (result.warp)
      {
        const auto& warp = TP_WARP_POINTS[*result.warp];
        if (RequestTPWarp(system, warp))
          OSD::AddMessage(fmt::format("TP benchmark: loading {}", warp.label), 3000,
                          0xff39ff14);
      }
    }
    UpdateController(pad, suppressed_buttons, suppress_sticks);
    Core::HostDispatchJobs(system);
    CaptureRuntimeSample(system);
    const auto now = std::chrono::steady_clock::now();
    if (now - heartbeat > std::chrono::seconds(5))
    {
      heartbeat = now;
      Log("Boot heartbeat", "core_state=%d; Plus+Minus stops", static_cast<int>(Core::GetState(system)));
      report_progress("heartbeat");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(8));
  }
  HideTPDeveloperMenu();
  report_progress("before-stop");

  // HOME itself is intentionally not delivered to applications. When Horizon asks the
  // application to exit (normally HOME -> Close Software), persist the tester session
  // before the potentially slower core shutdown/join path. This makes a normal HOME close
  // distinguishable from a hard crash on the next launch and avoids losing RAM telemetry.
  bool support_finalized = false;
  if (accepted && !app_running && !frontend_hotkey_exit)
  {
    EndSupportSession("clean_exit", last_guest_ms, LastError(), "system_exit_request",
                      "HOME menu / Close Software (Horizon exit request)");
    support_finalized = true;
    FlushLog();
  }

  Log("Boot stage 4", "stop and join Phocoena threads");
  Core::Stop(system);
  Core::Shutdown(system);
  report_progress("after-join");

  if (!support_finalized)
  {
    std::string_view exit_reason = "core_stopped";
    std::string_view exit_input = "none";
    if (!accepted)
    {
      exit_reason = "boot_failed";
    }
    else if (frontend_hotkey_exit)
    {
      exit_reason = "frontend_hotkey";
      exit_input = "Plus+Minus (held)";
    }
    else if (!LastError().empty())
    {
      exit_reason = "runtime_error";
    }
    EndSupportSession(accepted ? "clean_exit" : "boot_failed", last_guest_ms, LastError(),
                      exit_reason, exit_input);
  }
  auto_clocks.Stop();
  menu_active = true;
  if (!app_running)
  {
    Log("Shutdown", "Horizon exit request finalized; skipping console restore");
    return;
  }
  ResumeConsole();
  Header("Emulation stopped");
  printf("  Log: /switch/Phocoena/logs/phocoena.log\n  Last error: %.160s\n", DisplayText(LastError()).c_str());
  if (app_running) WaitBack(pad);
}
void GameBrowser(PadState& pad, const Settings& settings)
{
  std::string path = std::string(ROOT) + "games/";
  size_t selected = 0;
  bool rescan = true;
  std::vector<Entry> entries;
  std::string error;
  while (Poll(pad))
  {
    if (rescan)
    {
      entries = ReadDirectory(path, error);
      entries.insert(entries.begin(), {"..", ParentSDPath(path), true});
      selected = 0; rescan = false;
      Log("Browser", "%s entries=%zu", path.c_str(), entries.size() - 1);
      if (!error.empty()) Error("SD", "listing %s: %s", path.c_str(), error.c_str());
    }
    const u64 down = padGetButtonsDown(&pad);
    if (down & HidNpadButton_Up) selected = (selected + entries.size() - 1) % entries.size();
    if (down & HidNpadButton_Down) selected = (selected + 1) % entries.size();
    Header("Games");
    printf("  %.110s\n\n", DisplayText(path).c_str());
    const size_t first = selected / 17 * 17;
    for (size_t i = first; i < std::min(first + 17, entries.size()); ++i)
      printf("  %c %s%.100s\n", i == selected ? '>' : ' ', entries[i].directory ? "[DIR] " : "", DisplayText(entries[i].name).c_str());
    if (!error.empty()) printf("  ERROR: SD: %s\n", error.c_str());
    printf("\n  A: Open  B: Menu  X: SD root  Y: Rescan\n  .iso .gcm .rvz  |  .dol/.elf homebrew tests\n");
    if (down & HidNpadButton_B) return;
    if (down & HidNpadButton_X) { path = "sdmc:/"; rescan = true; }
    if (down & HidNpadButton_Y) rescan = true;
    if (down & HidNpadButton_A)
    {
      const Entry entry = entries[selected];
      if (entry.directory) { path = entry.path; rescan = true; }
      else { BootGame(entry.path, pad, settings); rescan = true; }
      if (!app_running) return;
    }
    consoleUpdate(nullptr);
  }
}
}
int Run()
{
  const bool log_open = OpenLog();
  InitializeConsole();
  if (!log_open)
  {
    printf("ERROR: SD: cannot open sdmc:/switch/Phocoena/logs/phocoena.log\n");
    PadState pad{}; padConfigureInput(1, HidNpadStyleSet_NpadStandard); padInitializeDefault(&pad);
    WaitBack(pad); ShutdownConsole(); return 1;
  }
  LogEnvironment();
  RecoverInterruptedSupportSession();
#ifdef HORIZON_VULKAN
  // Set before any temporary adapter enumeration or device creation.
  File::CreateFullPath(std::string(ROOT) + "cache/nxvk/");
  setenv("NVK_I_WANT_A_BROKEN_VULKAN_DRIVER", "1", 1);
  setenv("MESA_SHADER_CACHE_DIR", (std::string(ROOT) + "cache/nxvk").c_str(), 1);
  setenv("MESA_SHADER_CACHE_MAX_SIZE", "64M", 1);
  Log("Vulkan", "static NXVK experimental driver opt-in; shader cache=cache/nxvk; no OpenGL driver linked");
#endif
  if (!HorizonOpenCrashLog())
    Error("Crash logging", "cannot open logs/crash.log: %s", std::strerror(errno));
  const auto applet_type = appletGetAppletType();
  if (applet_type != AppletType_Application && applet_type != AppletType_SystemApplication)
  {
    Error("Launch", "full application memory required; use Atmosphere title takeover, not Album");
    printf("  Use title takeover (hold R while launching a game), not Album.\n");
    PadState pad{}; padConfigureInput(1, HidNpadStyleSet_NpadStandard); padInitializeDefault(&pad);
    WaitBack(pad); ShutdownConsole(); CloseLog(); return 1;
  }
  // Socket API is used by linked upstream libraries; no server or telemetry is enabled.
  const Result socket_result = socketInitializeDefault();
  if (R_FAILED(socket_result)) Error("Sockets", "initialization failed: 0x%08x; network features unavailable", socket_result);
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  PadState pad{}; padInitializeDefault(&pad);
  UICommon::SetUserDirectory(ROOT);
  File::SetUserPath(D_CONFIG_IDX, std::string(ROOT) + "config/");
  File::SetUserPath(D_CACHE_IDX, std::string(ROOT) + "cache/");
  File::SetUserPath(D_SHADERCACHE_IDX, std::string(ROOT) + "cache/shaders/");
  File::SetUserPath(D_SHADERS_IDX, std::string(ROOT) + "shaders/");
  File::SetUserPath(D_LOGS_IDX, std::string(ROOT) + "logs/");
  UICommon::CreateDirectories();
  File::CreateFullPath(std::string(ROOT) + "cache/tmp/");
  setenv("TMPDIR", (std::string(ROOT) + "cache/tmp").c_str(), 1);
  Common::RegisterMsgAlertHandler(Alert);
  Log("Init stage 1", "UICommon::Init");
  UICommon::Init();
  AttachCoreLogger();
  Log("Resources", "Sys=%s", File::GetSysDirectory().c_str());
  Settings settings;
  settings.Load(std::string(ROOT) + "config/switch.ini");
  // Tester measurements must always use the frozen hardware-validated baseline,
  // even when an older switch.ini is already present on the SD card.
  settings.ApplyFrozenTesterBaseline();
  if (!settings.Save(std::string(ROOT) + "config/switch.ini"))
    Error("Settings", "failed writing frozen tester baseline to switch.ini");
  ApplySettings(settings, false);
  if (!WriteDefaultMapping()) Error("Input", "cannot write default GCPadNew.ini");
  Log("Init stage 2", "UICommon::InitControllers");
  UICommon::InitControllers(WindowSystemInfo{WindowSystemType::Horizon, nullptr, nwindowGetDefault(), nwindowGetDefault()});
  InstallController();
  Pad::LoadConfig();
  Log("Init stage 3", "frontend ready; core boot waits for game selection");
  const std::array<const char*, 4> menu = {"Games", "Settings", "About / Diagnostics", "Exit"};
  size_t selected = 0;
  int exit_status = 0;
  try
  {
    while (Poll(pad))
    {
      const auto down = padGetButtonsDown(&pad);
      if (down & HidNpadButton_Up) selected = (selected + 3) % 4;
      if (down & HidNpadButton_Down) selected = (selected + 1) % 4;
      Header("GameCube / Atmosphere application takeover");
      for (size_t i = 0; i < menu.size(); ++i) printf("  %c %s\n", i == selected ? '>' : ' ', menu[i]);
      printf("\n  A: Select\n\n  CPU requested: %s\n  Graphics: %s / 1x\n"
              "  Auto clocks: %s (%s; MEM untouched)\n"
             "  Phocoena tester build; diagnostics are stored per game session.\n",
              settings.CPUName(), RENDERER_NAME,
              settings.phocoena_auto_clocks ? "On" : "Off",
              settings.phocoena_clock_profile == "legacy" ? "CPU 1581 / GPU 614" :
                                                              "CPU 1785 / GPU 768");
      if (down & HidNpadButton_A)
      {
        if (selected == 0) GameBrowser(pad, settings);
        if (selected == 1) SettingsMenu(pad, settings);
        if (selected == 2) RunDiagnostics(pad);
        if (selected == 3) break;
        if (!app_running) break;
      }
      consoleUpdate(nullptr);
    }
  }
  catch (const std::exception& e)
  {
    // In particular, a failed graphics-to-console handoff must leave stdout
    // redirected and shut down normally, never draw into a missing framebuffer.
    Error("Frontend", "%s", e.what());
    exit_status = 1;
  }
  Log("Shutdown", "controllers and configuration");
  UICommon::ShutdownControllers();
  UICommon::Shutdown();
  if (R_SUCCEEDED(socket_result)) socketExit();
  ShutdownConsole();
  Log("Shutdown", "cleanup complete; exit status=%d", exit_status);
  HorizonCloseCrashLog();
  CloseLog();
  return exit_status;
}
}
int main(int, char**)
{
  std::set_terminate([] {
    Horizon::Error("Fatal", "uncaught exception/terminate; inspect preceding subsystem logs");
    std::abort();
  });
  try { return Horizon::Run(); }
  catch (const std::exception& e)
  {
    Horizon::Error("Fatal", "%s", e.what());
    Horizon::ShutdownConsole();
    HorizonCloseCrashLog();
    Horizon::CloseLog();
    return 1;
  }
}

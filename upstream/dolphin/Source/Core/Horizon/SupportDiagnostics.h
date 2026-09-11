// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace Horizon
{
struct SupportSessionMetadata
{
  std::string game_id;
  std::string game_name;
  std::string internal_name;
  std::string region;
  std::string image_format;
  std::string source_extension;
  int revision = -1;
  int disc_number = -1;
  std::string renderer;
  std::string cpu_mode;
  std::string shader_mode;
  std::uint32_t fifo_blocks = 1;
  std::uint32_t requested_cpu_hz = 0;
  std::uint32_t requested_gpu_hz = 0;
  std::uint32_t selected_cpu_hz = 0;
  std::uint32_t selected_gpu_hz = 0;
  std::uint32_t effective_cpu_hz = 0;
  std::uint32_t effective_gpu_hz = 0;
  std::uint32_t effective_memory_hz = 0;
  bool effective_cpu_valid = false;
  bool effective_gpu_valid = false;
  bool effective_memory_valid = false;
  std::string clock_mechanism;
  std::string external_clock_manager;
  bool memory_clock_written = false;
};

struct SupportTelemetryMetrics
{
  bool performance_window = false;
  double gmsp_percent = 0.0;
  double fps = 0.0;
  double cpu_busy_percent = -1.0;
  double gpu_busy_percent = -1.0;
  double frame_mean_ms = 0.0;
  double frame_p95_ms = 0.0;
  double frame_p99_ms = 0.0;
  double one_percent_low_fps = 0.0;
  double vulkan_worker_busy_percent = -1.0;

  std::uint64_t shader_compile_calls = 0;
  double shader_compile_ms = 0.0;
  std::uint64_t pipeline_create_calls = 0;
  double pipeline_create_ms = 0.0;
  std::uint64_t jit_compile_calls = 0;
  double jit_compile_ms = 0.0;
  std::uint64_t vertex_loader_jit_calls = 0;
  double vertex_loader_jit_ms = 0.0;
  std::uint64_t disc_read_calls = 0;
  double disc_read_ms = 0.0;
  std::uint64_t disc_wait_calls = 0;
  double disc_wait_ms = 0.0;
  std::uint64_t gpu_wait_calls = 0;
  double gpu_wait_ms = 0.0;
  std::uint64_t fifo_sync_calls = 0;
  double fifo_sync_ms = 0.0;
  std::uint64_t vk_fence_wait_calls = 0;
  double vk_fence_wait_ms = 0.0;
  std::uint64_t vk_present_guard_calls = 0;
  double vk_present_guard_ms = 0.0;
  std::uint64_t vk_frame_reuse_calls = 0;
  double vk_frame_reuse_ms = 0.0;
  std::uint64_t log_write_calls = 0;
  double log_write_ms = 0.0;
  std::uint64_t fifo_decoder_calls = 0;
  std::uint64_t fifo_gather_blocks = 0;

  std::uint64_t guest_gpu_wait_timeouts = 0;
  std::uint64_t guest_pi_fifo_resets = 0;
  std::uint64_t guest_pi_reset_codes = 0;
  std::uint32_t last_guest_pi_reset_code = 0;

  std::uint64_t audio_release_events = 0;
  std::uint64_t audio_requeues = 0;
  std::uint64_t audio_wait_failures = 0;
  std::uint64_t audio_append_failures = 0;
};

// Finalizes a prior session as crash/unfinalized if Phocoena disappeared before cleanup.
// This runs before the crash log is rotated, so a non-empty crash.log can be associated
// with the interrupted session.
void RecoverInterruptedSupportSession();

bool BeginSupportSession(const SupportSessionMetadata& metadata, std::string* error = nullptr);
void UpdateSupportClockReadings(std::uint32_t selected_cpu_hz, std::uint32_t selected_gpu_hz,
                                std::uint32_t effective_cpu_hz, bool effective_cpu_valid,
                                std::uint32_t effective_gpu_hz, bool effective_gpu_valid,
                                std::uint32_t effective_memory_hz, bool effective_memory_valid,
                                std::string_view mechanism, std::string_view external_manager);

// Called by the existing ~5 s performance reporter. Samples are kept in RAM during emulation;
// only a small recovery checkpoint is written every ~30 s.
void AppendSupportTelemetry(std::string_view report, std::int64_t wall_ms, double guest_ms,
                            std::uint64_t presented_frames, int core_state,
                            const SupportTelemetryMetrics& metrics);

// status is the broad lifecycle result (clean_exit, boot_failed, abnormal_exit).
// exit_reason/exit_input preserve why/how the run ended without guessing when the OS
// did not expose a specific button event.
void EndSupportSession(std::string_view status, double guest_ms, std::string_view last_error,
                       std::string_view exit_reason, std::string_view exit_input);

// Creates one ZIP containing every not-yet-bundled session plus Phocoena's current logs/config.
// Games, saves, keys and arbitrary SD files are never added.
bool GenerateSupportBundle(std::string* output_path, std::string* error = nullptr);
std::size_t PendingSupportSessionCount();
}

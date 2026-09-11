// SPDX-License-Identifier: GPL-2.0-or-later
#include "Horizon/SupportDiagnostics.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <chrono>
#include <cmath>
#include <optional>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <switch.h>

#include "Horizon/BuildInfo.h"
#include "Horizon/Log.h"


namespace Horizon
{
namespace
{
namespace fs = std::filesystem;

constexpr std::int64_t CHECKPOINT_INTERVAL_MS = 30'000;
constexpr std::size_t MAX_BUFFERED_TELEMETRY = 8 * 1024 * 1024;

struct ActiveSession
{
  fs::path dir;
  std::string id;
  std::time_t started_epoch = 0;
  std::chrono::steady_clock::time_point started_mono{};
  SupportSessionMetadata metadata;
  std::string telemetry;
  std::string metrics_csv;
  std::int64_t last_checkpoint_wall_ms = 0;
  double last_guest_ms = 0.0;
  std::uint64_t last_presented_frames = 0;
  int last_core_state = 0;
  std::size_t telemetry_samples = 0;
  bool telemetry_truncated = false;

  std::size_t structured_samples = 0;
  double gmsp_sum = 0.0;
  double gmsp_min = 0.0;
  double gmsp_max = 0.0;
  double gmsp_last = 0.0;
  double fps_sum = 0.0;
  double fps_min = 0.0;
  double fps_max = 0.0;
  double fps_last = 0.0;
  double cpu_busy_sum = 0.0;
  double cpu_busy_max = -1.0;
  std::size_t cpu_busy_samples = 0;
  double gpu_busy_sum = 0.0;
  double gpu_busy_max = -1.0;
  std::size_t gpu_busy_samples = 0;
  double frame_mean_sum = 0.0;
  double frame_p95_max = 0.0;
  double frame_p99_max = 0.0;
  double one_percent_low_min = 0.0;
  double vulkan_worker_busy_sum = 0.0;
  double vulkan_worker_busy_max = -1.0;
  std::size_t vulkan_worker_busy_samples = 0;

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

  std::uint64_t gmsp_below_90_samples = 0;
  std::uint64_t gmsp_below_75_samples = 0;
  std::uint64_t gmsp_below_50_samples = 0;
  SupportTelemetryMetrics last_metrics{};
  bool active = false;
};

ActiveSession s_session;

fs::path DiagnosticsRoot() { return fs::path(ROOT) / "diagnostics"; }
fs::path SessionsRoot() { return DiagnosticsRoot() / "sessions"; }
fs::path BundlesRoot() { return DiagnosticsRoot() / "bundles"; }
fs::path ActiveMarker() { return DiagnosticsRoot() / "active-session.txt"; }

std::string Sanitize(std::string value)
{
  for (char& c : value)
  {
    const unsigned char u = static_cast<unsigned char>(c);
    if (!(std::isalnum(u) || c == '-' || c == '_' || c == '.'))
      c = '_';
  }
  if (value.empty())
    value = "unknown";
  if (value.size() > 64)
    value.resize(64);
  return value;
}

std::string JsonEscape(std::string_view value)
{
  std::string out;
  out.reserve(value.size() + 16);
  for (unsigned char c : value)
  {
    switch (c)
    {
    case '\\': out += "\\\\"; break;
    case '"': out += "\\\""; break;
    case '\n': out += "\\n"; break;
    case '\r': out += "\\r"; break;
    case '\t': out += "\\t"; break;
    default:
      if (c < 0x20)
      {
        char temp[7];
        std::snprintf(temp, sizeof(temp), "\\u%04x", c);
        out += temp;
      }
      else
      {
        out += static_cast<char>(c);
      }
      break;
    }
  }
  return out;
}

bool WriteTextAtomic(const fs::path& path, const std::string& text)
{
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  if (ec)
    return false;
  const fs::path temp = path.string() + ".tmp";
  {
    std::ofstream stream(temp, std::ios::binary | std::ios::trunc);
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    stream.flush();
    if (!stream)
      return false;
  }
  fs::remove(path, ec);
  ec.clear();
  fs::rename(temp, path, ec);
  return !ec;
}

std::string ReadKey(const fs::path& file, std::string_view wanted)
{
  std::ifstream stream(file);
  std::string line;
  const std::string prefix = std::string(wanted) + "=";
  while (std::getline(stream, line))
  {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (line.starts_with(prefix))
      return line.substr(prefix.size());
  }
  return {};
}

std::string ReadText(const fs::path& file)
{
  std::ifstream stream(file, std::ios::binary);
  return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

std::optional<double> ReadJsonNumber(const std::string& text, std::string_view key)
{
  const std::string token = "\"" + std::string(key) + "\":";
  const std::size_t at = text.find(token);
  if (at == std::string::npos)
    return std::nullopt;
  const std::size_t start = text.find_first_not_of(" \t", at + token.size());
  if (start == std::string::npos)
    return std::nullopt;
  const std::size_t end = text.find_first_of(",}\r\n", start);
  try
  {
    return std::stod(text.substr(start, end - start));
  }
  catch (...)
  {
    return std::nullopt;
  }
}

bool ReplaceJsonNumber(std::string& text, std::string_view key, const double value,
                       const bool integer = false)
{
  const std::string token = "\"" + std::string(key) + "\":";
  const std::size_t at = text.find(token);
  if (at == std::string::npos)
    return false;
  const std::size_t start = text.find_first_not_of(" \t", at + token.size());
  if (start == std::string::npos)
    return false;
  const std::size_t end = text.find_first_of(",}\r\n", start);
  std::ostringstream number;
  if (integer)
    number << static_cast<long long>(value);
  else
    number << value;
  text.replace(start, end - start, number.str());
  return true;
}

bool ReplaceJsonString(std::string& text, std::string_view key, std::string_view value)
{
  const std::string token = "\"" + std::string(key) + "\":";
  const std::size_t at = text.find(token);
  if (at == std::string::npos)
    return false;
  const std::size_t quote = text.find('"', at + token.size());
  if (quote == std::string::npos)
    return false;
  std::size_t end = quote + 1;
  bool escaped = false;
  for (; end < text.size(); ++end)
  {
    const char c = text[end];
    if (c == '"' && !escaped)
      break;
    escaped = (c == '\\') ? !escaped : false;
  }
  if (end >= text.size())
    return false;
  text.replace(quote + 1, end - quote - 1, JsonEscape(value));
  return true;
}

std::string RecoverSummary(std::string summary, const std::string& checkpoint,
                           std::string_view exit_reason, std::string_view exit_input,
                           const bool crash_evidence)
{
  ReplaceJsonString(summary, "status", "abnormal_exit");
  ReplaceJsonString(summary, "exit_reason", exit_reason);
  ReplaceJsonString(summary, "exit_input", exit_input);

  const auto copy_number = [&](std::string_view key, bool integer = false) {
    if (const auto value = ReadJsonNumber(checkpoint, key))
      ReplaceJsonNumber(summary, key, *value, integer);
  };
  copy_number("wall_ms", true);
  copy_number("guest_ms");
  copy_number("presented_frames", true);
  copy_number("last_core_state", true);
  copy_number("telemetry_samples", true);
  copy_number("structured_samples", true);
  copy_number("gmsp_avg");
  copy_number("gmsp_min");
  copy_number("gmsp_max");
  copy_number("gmsp_last");
  copy_number("fps_avg");
  copy_number("fps_min");
  copy_number("fps_max");
  copy_number("fps_last");
  copy_number("cpu_busy_percent_avg");
  copy_number("cpu_busy_percent_max");
  copy_number("gpu_busy_percent_avg");
  copy_number("gpu_busy_percent_max");
  copy_number("frame_mean_ms_avg");
  copy_number("vulkan_worker_busy_percent_avg");
  copy_number("vulkan_worker_busy_percent_max");
  copy_number("shader_compile_calls", true);
  copy_number("shader_compile_ms");
  copy_number("pipeline_create_calls", true);
  copy_number("pipeline_create_ms");
  copy_number("jit_compile_calls", true);
  copy_number("jit_compile_ms");
  copy_number("vertex_loader_jit_calls", true);
  copy_number("vertex_loader_jit_ms");
  copy_number("disc_read_calls", true);
  copy_number("disc_read_ms");
  copy_number("disc_wait_calls", true);
  copy_number("disc_wait_ms");
  copy_number("gpu_wait_calls", true);
  copy_number("gpu_wait_ms");
  copy_number("fifo_sync_calls", true);
  copy_number("fifo_sync_ms");
  copy_number("vk_fence_wait_calls", true);
  copy_number("vk_fence_wait_ms");
  copy_number("vk_present_guard_calls", true);
  copy_number("vk_present_guard_ms");
  copy_number("vk_frame_reuse_calls", true);
  copy_number("vk_frame_reuse_ms");
  copy_number("log_write_calls", true);
  copy_number("log_write_ms");
  copy_number("fifo_decoder_calls", true);
  copy_number("fifo_gather_blocks", true);
  copy_number("guest_gpu_wait_timeouts", true);
  copy_number("guest_pi_fifo_resets", true);
  copy_number("guest_pi_reset_codes", true);
  copy_number("last_guest_pi_reset_code", true);
  copy_number("frame_p95_ms_max");
  copy_number("frame_p99_ms_max");
  copy_number("one_percent_low_fps_min");
  copy_number("audio_release_events", true);
  copy_number("audio_requeues", true);
  copy_number("audio_wait_failures", true);
  copy_number("audio_append_failures", true);
  copy_number("gmsp_below_90_samples", true);
  copy_number("gmsp_below_75_samples", true);
  copy_number("gmsp_below_50_samples", true);

  if (const auto wall = ReadJsonNumber(checkpoint, "wall_ms"); wall && *wall > 0)
  {
    if (const auto guest = ReadJsonNumber(checkpoint, "guest_ms"))
      ReplaceJsonNumber(summary, "guest_speed_percent_approx", *guest * 100.0 / *wall);
  }

  const std::string close = "\n}\n";
  if (const std::size_t pos = summary.rfind(close); pos != std::string::npos)
  {
    std::ostringstream recovery;
    recovery << ",\n  \"recovered_on_next_launch\": true,\n"
             << "  \"crash_log_evidence\": " << (crash_evidence ? "true" : "false") << ",\n"
             << "  \"recovery_note\": \"Session was reconstructed from the last ~30 s checkpoint and preserved previous-run logs. Final seconds may be absent only when the process could not finalize.\"";
    summary.insert(pos, recovery.str());
  }
  return summary;
}

std::int64_t CurrentWallMs()
{
  if (!s_session.active)
    return 0;
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - s_session.started_mono)
      .count();
}

std::string SummaryJson(std::string_view status, double guest_ms, std::string_view last_error,
                        std::int64_t wall_ms, std::string_view exit_reason,
                        std::string_view exit_input, std::time_t ended_epoch = 0)
{
  const auto& m = s_session.metadata;
  const double guest_speed_percent =
      wall_ms > 0 ? guest_ms * 100.0 / static_cast<double>(wall_ms) : 0.0;
  const double gmsp_avg = s_session.structured_samples ?
      s_session.gmsp_sum / static_cast<double>(s_session.structured_samples) : 0.0;
  const double fps_avg = s_session.structured_samples ?
      s_session.fps_sum / static_cast<double>(s_session.structured_samples) : 0.0;
  const double cpu_busy_avg = s_session.cpu_busy_samples ?
      s_session.cpu_busy_sum / static_cast<double>(s_session.cpu_busy_samples) : -1.0;
  const double gpu_busy_avg = s_session.gpu_busy_samples ?
      s_session.gpu_busy_sum / static_cast<double>(s_session.gpu_busy_samples) : -1.0;
  const double frame_mean_avg = s_session.structured_samples ?
      s_session.frame_mean_sum / static_cast<double>(s_session.structured_samples) : 0.0;
  const double vulkan_worker_busy_avg = s_session.vulkan_worker_busy_samples ?
      s_session.vulkan_worker_busy_sum / static_cast<double>(s_session.vulkan_worker_busy_samples) : -1.0;

  std::ostringstream out;
  out << "{\n"
      << "  \"session_id\": \"" << JsonEscape(s_session.id) << "\",\n"
      << "  \"diagnostics_schema\": 2,\n"
      << "  \"checkpoint_interval_ms\": " << CHECKPOINT_INTERVAL_MS << ",\n"
      << "  \"phocoena_version\": \"" << JsonEscape(DOLPHIN_PORT_VERSION) << "\",\n"
      << "  \"phocoena_commit\": \"" << JsonEscape(PHOCOENA_SOURCE_COMMIT) << "\",\n"
      << "  \"phocoena_source_state\": \"" << JsonEscape(PHOCOENA_SOURCE_STATE) << "\",\n"
      << "  \"upstream_commit\": \"" << JsonEscape(DOLPHIN_UPSTREAM_COMMIT) << "\",\n"
      << "  \"build_date\": \"" << JsonEscape(DOLPHIN_BUILD_DATE) << "\",\n"
      << "  \"source_lock_sha256\": \"" << JsonEscape(DOLPHIN_PATCHSET_SHA256) << "\",\n"
      << "  \"game_id\": \"" << JsonEscape(m.game_id) << "\",\n"
      << "  \"game_name\": \"" << JsonEscape(m.game_name) << "\",\n"
      << "  \"internal_name\": \"" << JsonEscape(m.internal_name) << "\",\n"
      << "  \"region\": \"" << JsonEscape(m.region) << "\",\n"
      << "  \"revision\": " << m.revision << ",\n"
      << "  \"disc_number\": " << m.disc_number << ",\n"
      << "  \"image_format\": \"" << JsonEscape(m.image_format) << "\",\n"
      << "  \"source_extension\": \"" << JsonEscape(m.source_extension) << "\",\n"
      << "  \"renderer\": \"" << JsonEscape(m.renderer) << "\",\n"
      << "  \"cpu_mode\": \"" << JsonEscape(m.cpu_mode) << "\",\n"
      << "  \"shader_mode\": \"" << JsonEscape(m.shader_mode) << "\",\n"
      << "  \"fifo_blocks\": " << m.fifo_blocks << ",\n"
      << "  \"requested_cpu_hz\": " << m.requested_cpu_hz << ",\n"
      << "  \"requested_gpu_hz\": " << m.requested_gpu_hz << ",\n"
      << "  \"selected_cpu_hz\": " << m.selected_cpu_hz << ",\n"
      << "  \"selected_gpu_hz\": " << m.selected_gpu_hz << ",\n"
      << "  \"effective_cpu_hz\": " << m.effective_cpu_hz << ",\n"
      << "  \"effective_gpu_hz\": " << m.effective_gpu_hz << ",\n"
      << "  \"effective_memory_hz\": " << m.effective_memory_hz << ",\n"
      << "  \"effective_cpu_valid\": " << (m.effective_cpu_valid ? "true" : "false") << ",\n"
      << "  \"effective_gpu_valid\": " << (m.effective_gpu_valid ? "true" : "false") << ",\n"
      << "  \"effective_memory_valid\": " << (m.effective_memory_valid ? "true" : "false") << ",\n"
      << "  \"clock_mechanism\": \"" << JsonEscape(m.clock_mechanism) << "\",\n"
      << "  \"external_clock_manager\": \"" << JsonEscape(m.external_clock_manager) << "\",\n"
      << "  \"memory_clock_written\": " << (m.memory_clock_written ? "true" : "false") << ",\n"
      << "  \"started_epoch\": " << static_cast<long long>(s_session.started_epoch) << ",\n"
      << "  \"ended_epoch\": " << static_cast<long long>(ended_epoch) << ",\n"
      << "  \"wall_ms\": " << wall_ms << ",\n"
      << "  \"guest_ms\": " << guest_ms << ",\n"
      << "  \"guest_speed_percent_approx\": " << guest_speed_percent << ",\n"
      << "  \"presented_frames\": " << s_session.last_presented_frames << ",\n"
      << "  \"last_core_state\": " << s_session.last_core_state << ",\n"
      << "  \"telemetry_samples\": " << s_session.telemetry_samples << ",\n"
      << "  \"telemetry_truncated\": " << (s_session.telemetry_truncated ? "true" : "false") << ",\n"
      << "  \"structured_samples\": " << s_session.structured_samples << ",\n"
      << "  \"gmsp_avg\": " << gmsp_avg << ",\n"
      << "  \"gmsp_min\": " << (s_session.structured_samples ? s_session.gmsp_min : 0.0) << ",\n"
      << "  \"gmsp_max\": " << (s_session.structured_samples ? s_session.gmsp_max : 0.0) << ",\n"
      << "  \"gmsp_last\": " << s_session.gmsp_last << ",\n"
      << "  \"fps_avg\": " << fps_avg << ",\n"
      << "  \"fps_min\": " << (s_session.structured_samples ? s_session.fps_min : 0.0) << ",\n"
      << "  \"fps_max\": " << (s_session.structured_samples ? s_session.fps_max : 0.0) << ",\n"
      << "  \"fps_last\": " << s_session.fps_last << ",\n"
      << "  \"cpu_busy_percent_avg\": " << cpu_busy_avg << ",\n"
      << "  \"cpu_busy_percent_max\": " << s_session.cpu_busy_max << ",\n"
      << "  \"gpu_busy_percent_avg\": " << gpu_busy_avg << ",\n"
      << "  \"gpu_busy_percent_max\": " << s_session.gpu_busy_max << ",\n"
      << "  \"frame_mean_ms_avg\": " << frame_mean_avg << ",\n"
      << "  \"vulkan_worker_busy_percent_avg\": " << vulkan_worker_busy_avg << ",\n"
      << "  \"vulkan_worker_busy_percent_max\": " << s_session.vulkan_worker_busy_max << ",\n"
      << "  \"shader_compile_calls\": " << s_session.shader_compile_calls << ",\n"
      << "  \"shader_compile_ms\": " << s_session.shader_compile_ms << ",\n"
      << "  \"pipeline_create_calls\": " << s_session.pipeline_create_calls << ",\n"
      << "  \"pipeline_create_ms\": " << s_session.pipeline_create_ms << ",\n"
      << "  \"jit_compile_calls\": " << s_session.jit_compile_calls << ",\n"
      << "  \"jit_compile_ms\": " << s_session.jit_compile_ms << ",\n"
      << "  \"vertex_loader_jit_calls\": " << s_session.vertex_loader_jit_calls << ",\n"
      << "  \"vertex_loader_jit_ms\": " << s_session.vertex_loader_jit_ms << ",\n"
      << "  \"disc_read_calls\": " << s_session.disc_read_calls << ",\n"
      << "  \"disc_read_ms\": " << s_session.disc_read_ms << ",\n"
      << "  \"disc_wait_calls\": " << s_session.disc_wait_calls << ",\n"
      << "  \"disc_wait_ms\": " << s_session.disc_wait_ms << ",\n"
      << "  \"gpu_wait_calls\": " << s_session.gpu_wait_calls << ",\n"
      << "  \"gpu_wait_ms\": " << s_session.gpu_wait_ms << ",\n"
      << "  \"fifo_sync_calls\": " << s_session.fifo_sync_calls << ",\n"
      << "  \"fifo_sync_ms\": " << s_session.fifo_sync_ms << ",\n"
      << "  \"vk_fence_wait_calls\": " << s_session.vk_fence_wait_calls << ",\n"
      << "  \"vk_fence_wait_ms\": " << s_session.vk_fence_wait_ms << ",\n"
      << "  \"vk_present_guard_calls\": " << s_session.vk_present_guard_calls << ",\n"
      << "  \"vk_present_guard_ms\": " << s_session.vk_present_guard_ms << ",\n"
      << "  \"vk_frame_reuse_calls\": " << s_session.vk_frame_reuse_calls << ",\n"
      << "  \"vk_frame_reuse_ms\": " << s_session.vk_frame_reuse_ms << ",\n"
      << "  \"log_write_calls\": " << s_session.log_write_calls << ",\n"
      << "  \"log_write_ms\": " << s_session.log_write_ms << ",\n"
      << "  \"fifo_decoder_calls\": " << s_session.fifo_decoder_calls << ",\n"
      << "  \"fifo_gather_blocks\": " << s_session.fifo_gather_blocks << ",\n"
      << "  \"guest_gpu_wait_timeouts\": " << s_session.last_metrics.guest_gpu_wait_timeouts << ",\n"
      << "  \"guest_pi_fifo_resets\": " << s_session.last_metrics.guest_pi_fifo_resets << ",\n"
      << "  \"guest_pi_reset_codes\": " << s_session.last_metrics.guest_pi_reset_codes << ",\n"
      << "  \"last_guest_pi_reset_code\": " << s_session.last_metrics.last_guest_pi_reset_code << ",\n"
      << "  \"frame_p95_ms_max\": " << s_session.frame_p95_max << ",\n"
      << "  \"frame_p99_ms_max\": " << s_session.frame_p99_max << ",\n"
      << "  \"one_percent_low_fps_min\": " << s_session.one_percent_low_min << ",\n"
      << "  \"gmsp_below_90_samples\": " << s_session.gmsp_below_90_samples << ",\n"
      << "  \"gmsp_below_75_samples\": " << s_session.gmsp_below_75_samples << ",\n"
      << "  \"gmsp_below_50_samples\": " << s_session.gmsp_below_50_samples << ",\n"
      << "  \"audio_release_events\": " << s_session.last_metrics.audio_release_events << ",\n"
      << "  \"audio_requeues\": " << s_session.last_metrics.audio_requeues << ",\n"
      << "  \"audio_wait_failures\": " << s_session.last_metrics.audio_wait_failures << ",\n"
      << "  \"audio_append_failures\": " << s_session.last_metrics.audio_append_failures << ",\n"
      << "  \"status\": \"" << JsonEscape(status) << "\",\n"
      << "  \"exit_reason\": \"" << JsonEscape(exit_reason) << "\",\n"
      << "  \"exit_input\": \"" << JsonEscape(exit_input) << "\",\n"
      << "  \"last_error\": \"" << JsonEscape(last_error) << "\"\n"
      << "}\n";
  return out.str();
}

void CopyIfUseful(const fs::path& source, const fs::path& destination)

{
  std::error_code ec;
  if (!fs::is_regular_file(source, ec) || fs::file_size(source, ec) == 0)
    return;
  fs::copy_file(source, destination, fs::copy_options::overwrite_existing, ec);
}

class StoredZipWriter
{
public:
  explicit StoredZipWriter(const fs::path& path)
      : m_stream(path, std::ios::binary | std::ios::trunc)
  {
    if (!m_stream)
      m_error = "could not create support ZIP";
  }

  bool IsOpen() const { return m_stream.is_open() && m_stream.good(); }
  const std::string& Error() const { return m_error; }

  bool AddBuffer(std::string name, const std::string& text)
  {
    NormalizeName(name);
    if (name.empty())
      return Fail("empty ZIP entry name");
    if (text.size() > std::numeric_limits<std::uint32_t>::max())
      return Fail("ZIP entry is too large: " + name);

    const auto* bytes = reinterpret_cast<const std::uint8_t*>(text.data());
    const std::uint32_t crc = Crc32(bytes, text.size());
    const std::uint32_t size = static_cast<std::uint32_t>(text.size());
    if (!WriteLocalHeader(name, crc, size))
      return false;
    if (size && !WriteBytes(text.data(), size))
      return Fail("failed writing ZIP entry: " + name);
    return true;
  }

  bool AddFile(const fs::path& disk_path, std::string name)
  {
    NormalizeName(name);
    if (name.empty())
      return Fail("empty ZIP entry name");

    std::ifstream input(disk_path, std::ios::binary);
    if (!input)
      return Fail("could not read: " + disk_path.string());

    std::array<char, 64 * 1024> buffer{};
    std::uint64_t total = 0;
    std::uint32_t crc_state = 0xffffffffu;
    while (input)
    {
      input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
      const std::streamsize got = input.gcount();
      if (got > 0)
      {
        total += static_cast<std::uint64_t>(got);
        if (total > std::numeric_limits<std::uint32_t>::max())
          return Fail("ZIP entry is too large: " + name);
        crc_state = Crc32Update(crc_state,
                                reinterpret_cast<const std::uint8_t*>(buffer.data()),
                                static_cast<std::size_t>(got));
      }
    }
    if (!input.eof())
      return Fail("failed reading: " + disk_path.string());

    const std::uint32_t size = static_cast<std::uint32_t>(total);
    const std::uint32_t crc = crc_state ^ 0xffffffffu;
    if (!WriteLocalHeader(name, crc, size))
      return false;

    input.close();
    input.open(disk_path, std::ios::binary);
    if (!input)
      return Fail("could not reopen: " + disk_path.string());
    while (input)
    {
      input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
      const std::streamsize got = input.gcount();
      if (got > 0 && !WriteBytes(buffer.data(), static_cast<std::size_t>(got)))
        return Fail("failed writing ZIP entry: " + name);
    }
    if (!input.eof())
      return Fail("failed reading: " + disk_path.string());
    return true;
  }

  bool Finish()
  {
    if (m_finished)
      return m_error.empty();
    if (!IsOpen())
      return Fail(m_error.empty() ? "support ZIP is not open" : m_error);
    if (m_entries.size() > std::numeric_limits<std::uint16_t>::max())
      return Fail("too many files in support ZIP");

    const std::uint32_t central_offset = Tell32("ZIP central directory offset");
    if (!m_error.empty())
      return false;

    for (const Entry& entry : m_entries)
    {
      // Central directory file header.
      WriteU32(0x02014b50u);
      WriteU16(20);               // version made by
      WriteU16(20);               // version needed
      WriteU16(0x0800);           // UTF-8 names
      WriteU16(0);                // stored (no compression)
      WriteU16(0);                // DOS time
      WriteU16(0);                // DOS date
      WriteU32(entry.crc);
      WriteU32(entry.size);
      WriteU32(entry.size);
      WriteU16(static_cast<std::uint16_t>(entry.name.size()));
      WriteU16(0);                // extra length
      WriteU16(0);                // comment length
      WriteU16(0);                // disk number
      WriteU16(0);                // internal attributes
      WriteU32(0);                // external attributes
      WriteU32(entry.local_offset);
      if (!WriteBytes(entry.name.data(), entry.name.size()))
        return Fail("failed writing ZIP directory entry: " + entry.name);
    }

    const std::uint32_t central_end = Tell32("ZIP central directory end");
    if (!m_error.empty())
      return false;
    const std::uint32_t central_size = central_end - central_offset;
    const auto count = static_cast<std::uint16_t>(m_entries.size());

    // End of central directory record.
    WriteU32(0x06054b50u);
    WriteU16(0);
    WriteU16(0);
    WriteU16(count);
    WriteU16(count);
    WriteU32(central_size);
    WriteU32(central_offset);
    WriteU16(0);
    m_stream.flush();
    if (!m_stream)
      return Fail("failed finalizing support ZIP");
    m_stream.close();
    m_finished = true;
    return true;
  }

private:
  struct Entry
  {
    std::string name;
    std::uint32_t crc = 0;
    std::uint32_t size = 0;
    std::uint32_t local_offset = 0;
  };

  static void NormalizeName(std::string& name)
  {
    std::replace(name.begin(), name.end(), '\\', '/');
    while (!name.empty() && name.front() == '/')
      name.erase(name.begin());
  }

  static const std::array<std::uint32_t, 256>& CrcTable()
  {
    static const std::array<std::uint32_t, 256> table = [] {
      std::array<std::uint32_t, 256> values{};
      for (std::uint32_t i = 0; i < values.size(); ++i)
      {
        std::uint32_t c = i;
        for (int bit = 0; bit < 8; ++bit)
          c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
        values[i] = c;
      }
      return values;
    }();
    return table;
  }

  static std::uint32_t Crc32Update(std::uint32_t crc, const std::uint8_t* data,
                                   const std::size_t size)
  {
    const auto& table = CrcTable();
    for (std::size_t i = 0; i < size; ++i)
      crc = table[(crc ^ data[i]) & 0xffu] ^ (crc >> 8);
    return crc;
  }

  static std::uint32_t Crc32(const std::uint8_t* data, const std::size_t size)
  {
    return Crc32Update(0xffffffffu, data, size) ^ 0xffffffffu;
  }

  bool WriteLocalHeader(const std::string& name, const std::uint32_t crc,
                        const std::uint32_t size)
  {
    if (name.size() > std::numeric_limits<std::uint16_t>::max())
      return Fail("ZIP entry name is too long");
    const std::uint32_t offset = Tell32("ZIP local header offset");
    if (!m_error.empty())
      return false;

    WriteU32(0x04034b50u);
    WriteU16(20);
    WriteU16(0x0800);
    WriteU16(0);                  // stored (no compression)
    WriteU16(0);
    WriteU16(0);
    WriteU32(crc);
    WriteU32(size);
    WriteU32(size);
    WriteU16(static_cast<std::uint16_t>(name.size()));
    WriteU16(0);
    if (!WriteBytes(name.data(), name.size()))
      return Fail("failed writing ZIP header: " + name);

    m_entries.push_back({name, crc, size, offset});
    return true;
  }

  std::uint32_t Tell32(const char* what)
  {
    const std::streampos pos = m_stream.tellp();
    if (pos < 0 || static_cast<std::uint64_t>(pos) > std::numeric_limits<std::uint32_t>::max())
    {
      Fail(std::string(what) + " exceeds classic ZIP limits");
      return 0;
    }
    return static_cast<std::uint32_t>(pos);
  }

  bool WriteBytes(const void* data, const std::size_t size)
  {
    if (size == 0)
      return true;
    m_stream.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    return static_cast<bool>(m_stream);
  }

  void WriteU16(const std::uint16_t value)
  {
    const char bytes[2] = {static_cast<char>(value & 0xffu),
                           static_cast<char>((value >> 8) & 0xffu)};
    WriteBytes(bytes, sizeof(bytes));
  }

  void WriteU32(const std::uint32_t value)
  {
    const char bytes[4] = {static_cast<char>(value & 0xffu),
                           static_cast<char>((value >> 8) & 0xffu),
                           static_cast<char>((value >> 16) & 0xffu),
                           static_cast<char>((value >> 24) & 0xffu)};
    WriteBytes(bytes, sizeof(bytes));
  }

  bool Fail(std::string message)
  {
    if (m_error.empty())
      m_error = std::move(message);
    return false;
  }

  std::ofstream m_stream;
  std::vector<Entry> m_entries;
  std::string m_error;
  bool m_finished = false;
};

std::string CheckpointJson()
{
  const double gmsp_avg = s_session.structured_samples ?
      s_session.gmsp_sum / static_cast<double>(s_session.structured_samples) : 0.0;
  const double fps_avg = s_session.structured_samples ?
      s_session.fps_sum / static_cast<double>(s_session.structured_samples) : 0.0;
  const double cpu_busy_avg = s_session.cpu_busy_samples ?
      s_session.cpu_busy_sum / static_cast<double>(s_session.cpu_busy_samples) : -1.0;
  const double gpu_busy_avg = s_session.gpu_busy_samples ?
      s_session.gpu_busy_sum / static_cast<double>(s_session.gpu_busy_samples) : -1.0;
  const double frame_mean_avg = s_session.structured_samples ?
      s_session.frame_mean_sum / static_cast<double>(s_session.structured_samples) : 0.0;
  const double vulkan_worker_busy_avg = s_session.vulkan_worker_busy_samples ?
      s_session.vulkan_worker_busy_sum / static_cast<double>(s_session.vulkan_worker_busy_samples) : -1.0;
  std::ostringstream out;
  out << "{\n"
      << "  \"wall_ms\": " << CurrentWallMs() << ",\n"
      << "  \"guest_ms\": " << s_session.last_guest_ms << ",\n"
      << "  \"presented_frames\": " << s_session.last_presented_frames << ",\n"
      << "  \"last_core_state\": " << s_session.last_core_state << ",\n"
      << "  \"telemetry_samples\": " << s_session.telemetry_samples << ",\n"
      << "  \"structured_samples\": " << s_session.structured_samples << ",\n"
      << "  \"gmsp_avg\": " << gmsp_avg << ",\n"
      << "  \"gmsp_min\": " << (s_session.structured_samples ? s_session.gmsp_min : 0.0) << ",\n"
      << "  \"gmsp_max\": " << (s_session.structured_samples ? s_session.gmsp_max : 0.0) << ",\n"
      << "  \"gmsp_last\": " << s_session.gmsp_last << ",\n"
      << "  \"fps_avg\": " << fps_avg << ",\n"
      << "  \"fps_min\": " << (s_session.structured_samples ? s_session.fps_min : 0.0) << ",\n"
      << "  \"fps_max\": " << (s_session.structured_samples ? s_session.fps_max : 0.0) << ",\n"
      << "  \"fps_last\": " << s_session.fps_last << ",\n"
      << "  \"cpu_busy_percent_avg\": " << cpu_busy_avg << ",\n"
      << "  \"cpu_busy_percent_max\": " << s_session.cpu_busy_max << ",\n"
      << "  \"gpu_busy_percent_avg\": " << gpu_busy_avg << ",\n"
      << "  \"gpu_busy_percent_max\": " << s_session.gpu_busy_max << ",\n"
      << "  \"frame_mean_ms_avg\": " << frame_mean_avg << ",\n"
      << "  \"vulkan_worker_busy_percent_avg\": " << vulkan_worker_busy_avg << ",\n"
      << "  \"vulkan_worker_busy_percent_max\": " << s_session.vulkan_worker_busy_max << ",\n"
      << "  \"shader_compile_calls\": " << s_session.shader_compile_calls << ",\n"
      << "  \"shader_compile_ms\": " << s_session.shader_compile_ms << ",\n"
      << "  \"pipeline_create_calls\": " << s_session.pipeline_create_calls << ",\n"
      << "  \"pipeline_create_ms\": " << s_session.pipeline_create_ms << ",\n"
      << "  \"jit_compile_calls\": " << s_session.jit_compile_calls << ",\n"
      << "  \"jit_compile_ms\": " << s_session.jit_compile_ms << ",\n"
      << "  \"vertex_loader_jit_calls\": " << s_session.vertex_loader_jit_calls << ",\n"
      << "  \"vertex_loader_jit_ms\": " << s_session.vertex_loader_jit_ms << ",\n"
      << "  \"disc_read_calls\": " << s_session.disc_read_calls << ",\n"
      << "  \"disc_read_ms\": " << s_session.disc_read_ms << ",\n"
      << "  \"disc_wait_calls\": " << s_session.disc_wait_calls << ",\n"
      << "  \"disc_wait_ms\": " << s_session.disc_wait_ms << ",\n"
      << "  \"gpu_wait_calls\": " << s_session.gpu_wait_calls << ",\n"
      << "  \"gpu_wait_ms\": " << s_session.gpu_wait_ms << ",\n"
      << "  \"fifo_sync_calls\": " << s_session.fifo_sync_calls << ",\n"
      << "  \"fifo_sync_ms\": " << s_session.fifo_sync_ms << ",\n"
      << "  \"vk_fence_wait_calls\": " << s_session.vk_fence_wait_calls << ",\n"
      << "  \"vk_fence_wait_ms\": " << s_session.vk_fence_wait_ms << ",\n"
      << "  \"vk_present_guard_calls\": " << s_session.vk_present_guard_calls << ",\n"
      << "  \"vk_present_guard_ms\": " << s_session.vk_present_guard_ms << ",\n"
      << "  \"vk_frame_reuse_calls\": " << s_session.vk_frame_reuse_calls << ",\n"
      << "  \"vk_frame_reuse_ms\": " << s_session.vk_frame_reuse_ms << ",\n"
      << "  \"log_write_calls\": " << s_session.log_write_calls << ",\n"
      << "  \"log_write_ms\": " << s_session.log_write_ms << ",\n"
      << "  \"fifo_decoder_calls\": " << s_session.fifo_decoder_calls << ",\n"
      << "  \"fifo_gather_blocks\": " << s_session.fifo_gather_blocks << ",\n"
      << "  \"guest_gpu_wait_timeouts\": " << s_session.last_metrics.guest_gpu_wait_timeouts << ",\n"
      << "  \"guest_pi_fifo_resets\": " << s_session.last_metrics.guest_pi_fifo_resets << ",\n"
      << "  \"guest_pi_reset_codes\": " << s_session.last_metrics.guest_pi_reset_codes << ",\n"
      << "  \"last_guest_pi_reset_code\": " << s_session.last_metrics.last_guest_pi_reset_code << ",\n"
      << "  \"frame_p95_ms_max\": " << s_session.frame_p95_max << ",\n"
      << "  \"frame_p99_ms_max\": " << s_session.frame_p99_max << ",\n"
      << "  \"one_percent_low_fps_min\": " << s_session.one_percent_low_min << ",\n"
      << "  \"gmsp_below_90_samples\": " << s_session.gmsp_below_90_samples << ",\n"
      << "  \"gmsp_below_75_samples\": " << s_session.gmsp_below_75_samples << ",\n"
      << "  \"gmsp_below_50_samples\": " << s_session.gmsp_below_50_samples << ",\n"
      << "  \"audio_release_events\": " << s_session.last_metrics.audio_release_events << ",\n"
      << "  \"audio_requeues\": " << s_session.last_metrics.audio_requeues << ",\n"
      << "  \"audio_wait_failures\": " << s_session.last_metrics.audio_wait_failures << ",\n"
      << "  \"audio_append_failures\": " << s_session.last_metrics.audio_append_failures << "\n"
      << "}\n";
  return out.str();
}
}

void RecoverInterruptedSupportSession()
{
  std::error_code ec;
  fs::create_directories(SessionsRoot(), ec);
  fs::create_directories(BundlesRoot(), ec);
  if (!fs::is_regular_file(ActiveMarker(), ec))
    return;

  const std::string relative = ReadKey(ActiveMarker(), "session_dir");
  if (!relative.empty())
  {
    const fs::path dir = DiagnosticsRoot() / relative;
    const fs::path summary = dir / "summary.json";
    const std::string prior_summary = ReadText(summary);
    const std::string checkpoint = ReadText(dir / "checkpoint.json");

    ec.clear();
    const fs::path crash_log = fs::path(ROOT) / "logs/crash.log";
    const bool crash_evidence = fs::is_regular_file(crash_log, ec) &&
                                fs::file_size(crash_log, ec) > 0;
    const std::string exit_reason = crash_evidence ? "crash" : "unfinalized_process_exit";
    const std::string exit_input = crash_evidence ? "none" : "unknown";

    if (!prior_summary.empty())
    {
      std::string recovered = RecoverSummary(prior_summary, checkpoint, exit_reason, exit_input,
                                             crash_evidence);
      ReplaceJsonNumber(recovered, "ended_epoch", static_cast<double>(std::time(nullptr)), true);
      WriteTextAtomic(summary, recovered);
    }
    else
    {
      std::ostringstream fallback;
      fallback << "{\n  \"status\": \"abnormal_exit\",\n"
               << "  \"exit_reason\": \"" << exit_reason << "\",\n"
               << "  \"exit_input\": \"" << exit_input << "\",\n"
               << "  \"recovered_on_next_launch\": true\n}\n";
      WriteTextAtomic(summary, fallback.str());
    }

    CopyIfUseful(fs::path(ROOT) / "logs/phocoena.previous.log", dir / "phocoena-recovered.log");
    CopyIfUseful(fs::path(ROOT) / "logs/failure-tail.log", dir / "failure-tail.log");
    if (crash_evidence)
      CopyIfUseful(crash_log, dir / "crash.log");

    std::ofstream event(dir / "events.log", std::ios::binary | std::ios::app);
    event << "session_recovered status=abnormal_exit exit_reason=" << exit_reason
          << " checkpoint_present=" << (!checkpoint.empty() ? 1 : 0)
          << " crash_log_evidence=" << (crash_evidence ? 1 : 0) << "\n";
  }
  fs::remove(ActiveMarker(), ec);
}

bool BeginSupportSession(const SupportSessionMetadata& metadata, std::string* error)
{
  if (s_session.active)
    EndSupportSession("abnormal_exit", 0.0, "new session started before previous session finalized",
                      "superseded", "none");

  std::error_code ec;
  fs::create_directories(SessionsRoot(), ec);
  fs::create_directories(BundlesRoot(), ec);
  if (ec)
  {
    if (error) *error = ec.message();
    return false;
  }

  s_session = {};
  s_session.metadata = metadata;
  s_session.started_epoch = std::time(nullptr);
  s_session.started_mono = std::chrono::steady_clock::now();
  s_session.telemetry.reserve(512 * 1024);
  s_session.metrics_csv.reserve(64 * 1024);
  s_session.metrics_csv =
      "sample,wall_ms,guest_ms,performance_window,gmsp_percent,fps,cpu_busy_percent,gpu_busy_percent,"
      "vulkan_worker_busy_percent,frame_mean_ms,frame_p95_ms,frame_p99_ms,one_percent_low_fps,"
      "presented_frames,shader_compile_calls,shader_compile_ms,pipeline_create_calls,pipeline_create_ms,"
      "jit_compile_calls,jit_compile_ms,vertex_loader_jit_calls,vertex_loader_jit_ms,disc_read_calls,"
      "disc_read_ms,disc_wait_calls,disc_wait_ms,gpu_wait_calls,gpu_wait_ms,fifo_sync_calls,fifo_sync_ms,"
      "vk_fence_wait_calls,vk_fence_wait_ms,vk_present_guard_calls,vk_present_guard_ms,"
      "vk_frame_reuse_calls,vk_frame_reuse_ms,log_write_calls,log_write_ms,fifo_decoder_calls,"
      "fifo_gather_blocks,guest_gpu_wait_timeouts,guest_pi_fifo_resets,guest_pi_reset_codes,"
      "last_guest_pi_reset_code,audio_release_events,audio_requeues,audio_wait_failures,audio_append_failures\n";
  const auto tick = static_cast<unsigned long long>(
      std::chrono::steady_clock::now().time_since_epoch().count() & 0xffffff);
  s_session.id = std::to_string(static_cast<long long>(s_session.started_epoch)) + "-" +
                 Sanitize(metadata.game_id.empty() ? "EXEC" : metadata.game_id) + "-" +
                 std::to_string(tick);
  s_session.dir = SessionsRoot() / s_session.id;
  fs::create_directories(s_session.dir, ec);
  if (ec)
  {
    if (error) *error = ec.message();
    s_session = {};
    return false;
  }
  s_session.active = true;

  WriteTextAtomic(s_session.dir / "summary.json",
                  SummaryJson("running", 0.0, "", 0, "running", "none"));
  const std::string settings_snapshot = ReadText(fs::path(ROOT) / "config/switch.ini");
  if (!settings_snapshot.empty())
    WriteTextAtomic(s_session.dir / "settings.ini", settings_snapshot);
  else
    WriteTextAtomic(s_session.dir / "settings.ini", "# switch.ini snapshot unavailable\n");
  std::ofstream events(s_session.dir / "events.log", std::ios::binary | std::ios::app);
  events << "session_started\n";

  // Avoid std::filesystem::relative() here. Horizon/devoptab paths such as
  // sdmc:/... can make libc++ path resolution return EINVAL even though both
  // paths are valid. The session directory layout is fixed, so write the
  // known diagnostics-relative path directly.
  std::ostringstream marker;
  marker << "session_dir=sessions/" << s_session.id << "\n"
         << "session_id=" << s_session.id << "\n"
         << "started_epoch=" << static_cast<long long>(s_session.started_epoch) << "\n";
  if (!WriteTextAtomic(ActiveMarker(), marker.str()))
  {
    WriteTextAtomic(s_session.dir / "summary.json",
                    SummaryJson("diagnostics_error", 0.0, "could not write active-session marker", 0,
                                "diagnostics_error", "none"));
    if (error) *error = "could not write active-session marker";
    s_session = {};
    return false;
  }
  return true;
}

void UpdateSupportClockReadings(const std::uint32_t selected_cpu_hz,
                                const std::uint32_t selected_gpu_hz,
                                const std::uint32_t effective_cpu_hz,
                                const bool effective_cpu_valid,
                                const std::uint32_t effective_gpu_hz,
                                const bool effective_gpu_valid,
                                const std::uint32_t effective_memory_hz,
                                const bool effective_memory_valid,
                                const std::string_view mechanism,
                                const std::string_view external_manager)
{
  if (!s_session.active)
    return;
  auto& m = s_session.metadata;
  m.selected_cpu_hz = selected_cpu_hz;
  m.selected_gpu_hz = selected_gpu_hz;
  m.effective_cpu_hz = effective_cpu_hz;
  m.effective_gpu_hz = effective_gpu_hz;
  m.effective_memory_hz = effective_memory_hz;
  m.effective_cpu_valid = effective_cpu_valid;
  m.effective_gpu_valid = effective_gpu_valid;
  m.effective_memory_valid = effective_memory_valid;
  m.clock_mechanism = std::string(mechanism);
  m.external_clock_manager = std::string(external_manager);
  // One pre-emulation write captures the effective clock state for crash recovery.
  WriteTextAtomic(s_session.dir / "summary.json",
                  SummaryJson("running", 0.0, "", CurrentWallMs(), "running", "none"));
}

void AppendSupportTelemetry(std::string_view report, const std::int64_t wall_ms,
                            const double guest_ms, const std::uint64_t presented_frames,
                            const int core_state, const SupportTelemetryMetrics& metrics)
{
  if (!s_session.active)
    return;

  s_session.last_guest_ms = guest_ms;
  s_session.last_presented_frames = presented_frames;
  s_session.last_core_state = core_state;
  s_session.last_metrics = metrics;
  ++s_session.telemetry_samples;

  std::ostringstream header;
  header << "--- sample=" << s_session.telemetry_samples
         << " wall_ms=" << wall_ms
         << " guest_ms=" << guest_ms
         << " gmsp=" << metrics.gmsp_percent
         << " fps=" << metrics.fps
         << " presented_frames=" << presented_frames
         << " core_state=" << core_state << " ---\n";
  const std::string prefix = header.str();
  const std::size_t needed = prefix.size() + report.size() + 1;
  if (s_session.telemetry.size() + needed <= MAX_BUFFERED_TELEMETRY)
  {
    s_session.telemetry += prefix;
    s_session.telemetry.append(report.data(), report.size());
    if (s_session.telemetry.empty() || s_session.telemetry.back() != '\n')
      s_session.telemetry.push_back('\n');
  }
  else
  {
    s_session.telemetry_truncated = true;
  }

  if (metrics.performance_window && std::isfinite(metrics.gmsp_percent) &&
      metrics.gmsp_percent >= 0.0 && std::isfinite(metrics.fps) && metrics.fps >= 0.0)
  {
    ++s_session.structured_samples;
    if (s_session.structured_samples == 1)
    {
      s_session.gmsp_min = s_session.gmsp_max = metrics.gmsp_percent;
      s_session.fps_min = s_session.fps_max = metrics.fps;
      s_session.one_percent_low_min = metrics.one_percent_low_fps;
    }
    else
    {
      s_session.gmsp_min = std::min(s_session.gmsp_min, metrics.gmsp_percent);
      s_session.gmsp_max = std::max(s_session.gmsp_max, metrics.gmsp_percent);
      s_session.fps_min = std::min(s_session.fps_min, metrics.fps);
      s_session.fps_max = std::max(s_session.fps_max, metrics.fps);
      if (metrics.one_percent_low_fps > 0.0)
      {
        if (s_session.one_percent_low_min <= 0.0)
          s_session.one_percent_low_min = metrics.one_percent_low_fps;
        else
          s_session.one_percent_low_min =
              std::min(s_session.one_percent_low_min, metrics.one_percent_low_fps);
      }
    }
    s_session.gmsp_sum += metrics.gmsp_percent;
    s_session.gmsp_last = metrics.gmsp_percent;
    s_session.fps_sum += metrics.fps;
    s_session.fps_last = metrics.fps;
    s_session.frame_mean_sum += metrics.frame_mean_ms;
    s_session.frame_p95_max = std::max(s_session.frame_p95_max, metrics.frame_p95_ms);
    s_session.frame_p99_max = std::max(s_session.frame_p99_max, metrics.frame_p99_ms);
    if (metrics.gmsp_percent < 90.0) ++s_session.gmsp_below_90_samples;
    if (metrics.gmsp_percent < 75.0) ++s_session.gmsp_below_75_samples;
    if (metrics.gmsp_percent < 50.0) ++s_session.gmsp_below_50_samples;

    if (std::isfinite(metrics.cpu_busy_percent) && metrics.cpu_busy_percent >= 0.0)
    {
      s_session.cpu_busy_sum += metrics.cpu_busy_percent;
      s_session.cpu_busy_max = std::max(s_session.cpu_busy_max, metrics.cpu_busy_percent);
      ++s_session.cpu_busy_samples;
    }
    if (std::isfinite(metrics.gpu_busy_percent) && metrics.gpu_busy_percent >= 0.0)
    {
      s_session.gpu_busy_sum += metrics.gpu_busy_percent;
      s_session.gpu_busy_max = std::max(s_session.gpu_busy_max, metrics.gpu_busy_percent);
      ++s_session.gpu_busy_samples;
    }
    if (std::isfinite(metrics.vulkan_worker_busy_percent) && metrics.vulkan_worker_busy_percent >= 0.0)
    {
      s_session.vulkan_worker_busy_sum += metrics.vulkan_worker_busy_percent;
      s_session.vulkan_worker_busy_max =
          std::max(s_session.vulkan_worker_busy_max, metrics.vulkan_worker_busy_percent);
      ++s_session.vulkan_worker_busy_samples;
    }

    s_session.shader_compile_calls += metrics.shader_compile_calls;
    s_session.shader_compile_ms += metrics.shader_compile_ms;
    s_session.pipeline_create_calls += metrics.pipeline_create_calls;
    s_session.pipeline_create_ms += metrics.pipeline_create_ms;
    s_session.jit_compile_calls += metrics.jit_compile_calls;
    s_session.jit_compile_ms += metrics.jit_compile_ms;
    s_session.vertex_loader_jit_calls += metrics.vertex_loader_jit_calls;
    s_session.vertex_loader_jit_ms += metrics.vertex_loader_jit_ms;
    s_session.disc_read_calls += metrics.disc_read_calls;
    s_session.disc_read_ms += metrics.disc_read_ms;
    s_session.disc_wait_calls += metrics.disc_wait_calls;
    s_session.disc_wait_ms += metrics.disc_wait_ms;
    s_session.gpu_wait_calls += metrics.gpu_wait_calls;
    s_session.gpu_wait_ms += metrics.gpu_wait_ms;
    s_session.fifo_sync_calls += metrics.fifo_sync_calls;
    s_session.fifo_sync_ms += metrics.fifo_sync_ms;
    s_session.vk_fence_wait_calls += metrics.vk_fence_wait_calls;
    s_session.vk_fence_wait_ms += metrics.vk_fence_wait_ms;
    s_session.vk_present_guard_calls += metrics.vk_present_guard_calls;
    s_session.vk_present_guard_ms += metrics.vk_present_guard_ms;
    s_session.vk_frame_reuse_calls += metrics.vk_frame_reuse_calls;
    s_session.vk_frame_reuse_ms += metrics.vk_frame_reuse_ms;
    s_session.log_write_calls += metrics.log_write_calls;
    s_session.log_write_ms += metrics.log_write_ms;
    s_session.fifo_decoder_calls += metrics.fifo_decoder_calls;
    s_session.fifo_gather_blocks += metrics.fifo_gather_blocks;
  }

  std::ostringstream row;
  row << s_session.telemetry_samples << ',' << wall_ms << ',' << guest_ms << ','
      << (metrics.performance_window ? 1 : 0) << ',' << metrics.gmsp_percent << ',' << metrics.fps
      << ',' << metrics.cpu_busy_percent << ',' << metrics.gpu_busy_percent << ','
      << metrics.vulkan_worker_busy_percent << ',' << metrics.frame_mean_ms << ','
      << metrics.frame_p95_ms << ',' << metrics.frame_p99_ms << ',' << metrics.one_percent_low_fps
      << ',' << presented_frames << ',' << metrics.shader_compile_calls << ','
      << metrics.shader_compile_ms << ',' << metrics.pipeline_create_calls << ','
      << metrics.pipeline_create_ms << ',' << metrics.jit_compile_calls << ',' << metrics.jit_compile_ms
      << ',' << metrics.vertex_loader_jit_calls << ',' << metrics.vertex_loader_jit_ms << ','
      << metrics.disc_read_calls << ',' << metrics.disc_read_ms << ',' << metrics.disc_wait_calls
      << ',' << metrics.disc_wait_ms << ',' << metrics.gpu_wait_calls << ',' << metrics.gpu_wait_ms
      << ',' << metrics.fifo_sync_calls << ',' << metrics.fifo_sync_ms << ','
      << metrics.vk_fence_wait_calls << ',' << metrics.vk_fence_wait_ms << ','
      << metrics.vk_present_guard_calls << ',' << metrics.vk_present_guard_ms << ','
      << metrics.vk_frame_reuse_calls << ',' << metrics.vk_frame_reuse_ms << ','
      << metrics.log_write_calls << ',' << metrics.log_write_ms << ',' << metrics.fifo_decoder_calls
      << ',' << metrics.fifo_gather_blocks << ',' << metrics.guest_gpu_wait_timeouts << ','
      << metrics.guest_pi_fifo_resets << ',' << metrics.guest_pi_reset_codes << ','
      << metrics.last_guest_pi_reset_code << ',' << metrics.audio_release_events << ','
      << metrics.audio_requeues << ',' << metrics.audio_wait_failures << ','
      << metrics.audio_append_failures << '\n';
  s_session.metrics_csv += row.str();

  // Do not add SD traffic at the ~5 s telemetry cadence. The compact checkpoint is the only
  // periodic diagnostics write and exists solely to salvage useful progress after a hard exit.
  if (s_session.last_checkpoint_wall_ms == 0 ||
      wall_ms - s_session.last_checkpoint_wall_ms >= CHECKPOINT_INTERVAL_MS)
  {
    WriteTextAtomic(s_session.dir / "checkpoint.json", CheckpointJson());
    s_session.last_checkpoint_wall_ms = wall_ms;
  }
}

void EndSupportSession(const std::string_view status, const double guest_ms,
                       const std::string_view last_error, const std::string_view exit_reason,
                       const std::string_view exit_input)
{
  if (!s_session.active)
    return;
  const std::int64_t wall_ms = CurrentWallMs();
  s_session.last_guest_ms = guest_ms;
  if (!s_session.telemetry.empty())
    WriteTextAtomic(s_session.dir / "telemetry.log", s_session.telemetry);
  if (!s_session.metrics_csv.empty())
    WriteTextAtomic(s_session.dir / "metrics.csv", s_session.metrics_csv);
  WriteTextAtomic(s_session.dir / "checkpoint.json", CheckpointJson());
  WriteTextAtomic(s_session.dir / "summary.json",
                  SummaryJson(status, guest_ms, last_error, wall_ms, exit_reason, exit_input,
                              std::time(nullptr)));
  {
    std::ofstream events(s_session.dir / "events.log", std::ios::binary | std::ios::app);
    events << "session_ended status=" << status << " exit_reason=" << exit_reason
           << " exit_input=" << exit_input << " wall_ms=" << wall_ms << " guest_ms=" << guest_ms
           << " telemetry_samples=" << s_session.telemetry_samples
           << " structured_samples=" << s_session.structured_samples << "\n";
  }
  CopyIfUseful(fs::path(ROOT) / "logs/failure-tail.log", s_session.dir / "failure-tail.log");
  CopyIfUseful(fs::path(ROOT) / "logs/crash.log", s_session.dir / "crash.log");
  std::error_code ec;
  fs::remove(ActiveMarker(), ec);
  s_session = {};
}

std::size_t PendingSupportSessionCount()
{
  std::error_code ec;
  if (!fs::is_directory(SessionsRoot(), ec))
    return 0;
  std::size_t count = 0;
  for (const auto& entry : fs::directory_iterator(SessionsRoot(), ec))
  {
    if (ec) break;
    if (entry.is_directory(ec) && !fs::exists(entry.path() / "bundled.marker", ec))
      ++count;
  }
  return count;
}

bool GenerateSupportBundle(std::string* output_path, std::string* error)
{
  FlushLog();
  std::error_code ec;
  fs::create_directories(BundlesRoot(), ec);
  if (ec)
  {
    if (error) *error = ec.message();
    return false;
  }

  std::vector<fs::path> sessions;
  if (fs::is_directory(SessionsRoot(), ec))
  {
    for (const auto& entry : fs::directory_iterator(SessionsRoot(), ec))
    {
      if (ec) break;
      if (entry.is_directory(ec) && !fs::exists(entry.path() / "bundled.marker", ec))
        sessions.push_back(entry.path());
    }
  }
  if (sessions.empty())
  {
    if (error) *error = "no new game sessions to bundle";
    return false;
  }
  std::sort(sessions.begin(), sessions.end());

  const std::time_t now = std::time(nullptr);

  // Build the new archive under a temporary name first. Delivery-state filenames are
  // updated only after the ZIP has finalized successfully, so a failed generation
  // never makes an older UNSENT bundle look as though it was already sent.
  const std::string bundle_base =
      "Phocoena-Support-" + std::to_string(static_cast<long long>(now));
  const fs::path archive_temp = BundlesRoot() / ("." + bundle_base + ".zip.tmp");
  fs::remove(archive_temp, ec);
  ec.clear();
  StoredZipWriter writer(archive_temp);
  if (!writer.IsOpen())
  {
    if (error) *error = writer.Error();
    return false;
  }

  std::ostringstream manifest;
  manifest << "{\n"
           << "  \"product\": \"Phocoena\",\n"
           << "  \"diagnostics_schema\": 2,\n"
           << "  \"version\": \"" << JsonEscape(DOLPHIN_PORT_VERSION) << "\",\n"
           << "  \"phocoena_commit\": \"" << JsonEscape(PHOCOENA_SOURCE_COMMIT) << "\",\n"
           << "  \"phocoena_source_state\": \"" << JsonEscape(PHOCOENA_SOURCE_STATE) << "\",\n"
           << "  \"upstream_commit\": \"" << JsonEscape(DOLPHIN_UPSTREAM_COMMIT) << "\",\n"
           << "  \"build_date\": \"" << JsonEscape(DOLPHIN_BUILD_DATE) << "\",\n"
           << "  \"source_lock_sha256\": \"" << JsonEscape(DOLPHIN_PATCHSET_SHA256) << "\",\n"
           << "  \"generated_epoch\": " << static_cast<long long>(now) << ",\n"
           << "  \"session_count\": " << sessions.size() << ",\n"
           << "  \"archive_method\": \"store\",\n"
           << "  \"privacy\": \"No games, saves, keys, account data, or arbitrary SD files are included.\"\n"
           << "}\n";
  const std::string manifest_text = manifest.str();
  const std::string readme =
      "Phocoena support bundle\n\n"
      "sessions/ contains one directory per game run. summary.json is the compact overview;\n"
      "metrics.csv contains low-overhead structured ~5 second samples (GMSP/FPS/CPU/GPU/frame\n"
      "cadence/audio counters), and telemetry.log contains the detailed performance scopes.\n"
      "Recovered hard exits preserve checkpoint data plus the previous-run log when possible.\n"
      "Global logs are included under logs/. This bundle intentionally excludes games, saves,\n"
      "keys, account information and unrelated SD files.\n";

  const u32 hos = hosversionGet();
  u64 core_mask = 0, total_memory = 0, used_memory = 0;
  const Result core_rc = svcGetInfo(&core_mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0);
  const Result total_rc = svcGetInfo(&total_memory, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
  const Result used_rc = svcGetInfo(&used_memory, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
  std::ostringstream system_info;
  system_info << "{\n"
              << "  \"horizon_version\": \"" << HOSVER_MAJOR(hos) << '.' << HOSVER_MINOR(hos)
              << '.' << HOSVER_MICRO(hos) << "\",\n"
              << "  \"atmosphere_detected\": " << (hosversionIsAtmosphere() ? "true" : "false") << ",\n"
              << "  \"applet_type\": " << static_cast<int>(appletGetAppletType()) << ",\n"
              << "  \"core_mask_query_result\": " << core_rc << ",\n"
              << "  \"core_mask\": " << static_cast<unsigned long long>(core_mask) << ",\n"
              << "  \"memory_query_results\": [" << total_rc << ", " << used_rc << "],\n"
              << "  \"application_total_memory_bytes\": " << static_cast<unsigned long long>(total_memory) << ",\n"
              << "  \"application_used_memory_bytes\": " << static_cast<unsigned long long>(used_memory) << "\n"
              << "}\n";

  auto fail_bundle = [&](const std::string& prefix) {
    fs::remove(archive_temp, ec);
    if (error)
      *error = prefix + (writer.Error().empty() ? std::string{} : ": " + writer.Error());
    return false;
  };

  if (!writer.AddBuffer("manifest.json", manifest_text) ||
      !writer.AddBuffer("system-info.json", system_info.str()) ||
      !writer.AddBuffer("README.txt", readme))
    return fail_bundle("could not write support manifest");

  for (const fs::path& session : sessions)
  {
    for (const auto& file : fs::recursive_directory_iterator(session, ec))
    {
      if (ec)
        return fail_bundle("could not enumerate session files: " + ec.message());
      if (!file.is_regular_file(ec) || file.path().filename() == "bundled.marker")
        continue;
      // Do not use std::filesystem::relative() on Horizon. devoptab paths
      // (sdmc:/...) can report EINVAL during canonicalization. The iterator is
      // already rooted at `session`, so derive the archive name by stripping
      // that exact lexical prefix instead of touching the filesystem again.
      const std::string session_name = session.filename().generic_string();
      const std::string session_prefix = session.generic_string() + "/";
      const std::string disk_name = file.path().generic_string();
      if (!disk_name.starts_with(session_prefix))
        return fail_bundle("session file escaped its session directory");
      const std::string within_session = disk_name.substr(session_prefix.size());
      if (within_session.empty() || within_session == ".." ||
          within_session.starts_with("../") || within_session.find("/../") != std::string::npos)
        return fail_bundle("invalid session archive path");
      if (!writer.AddFile(file.path(),
                          "sessions/" + session_name + "/" + within_session))
        return fail_bundle("could not add session file");
    }
  }

  const std::vector<std::pair<fs::path, std::string>> extras = {
      {fs::path(ROOT) / "config/switch.ini", "config/switch.ini"},
      {fs::path(ROOT) / "logs/phocoena.log", "logs/phocoena.log"},
      {fs::path(ROOT) / "logs/phocoena.previous.log", "logs/phocoena.previous.log"},
      {fs::path(ROOT) / "logs/failure-tail.log", "logs/failure-tail.log"},
      {fs::path(ROOT) / "logs/crash.log", "logs/crash.log"},
      {fs::path(ROOT) / "logs/crash.previous.log", "logs/crash.previous.log"},
  };
  for (const auto& [disk, name] : extras)
  {
    ec.clear();
    if (fs::is_regular_file(disk, ec) && !writer.AddFile(disk, name))
      return fail_bundle("could not add global log/config file");
  }

  if (!writer.Finish())
    return fail_bundle("failed finalizing support ZIP");

  std::vector<std::pair<fs::path, fs::path>> delivery_renames;
  auto unique_bundle_path = [&](const fs::path& desired) {
    const auto path_is_taken = [&](const fs::path& candidate) {
      std::error_code exists_ec;
      if (fs::exists(candidate, exists_ec))
        return true;
      return std::any_of(delivery_renames.begin(), delivery_renames.end(),
                         [&](const auto& rename) { return rename.second == candidate; });
    };

    if (!path_is_taken(desired))
      return desired;

    const fs::path parent = desired.parent_path();
    const std::string stem = desired.stem().string();
    const std::string extension = desired.extension().string();
    for (std::size_t suffix = 2;; ++suffix)
    {
      const fs::path candidate =
          parent / (stem + "-" + std::to_string(suffix) + extension);
      if (!path_is_taken(candidate))
        return candidate;
    }
  };

  // Every older bundle that has not already been marked SENT becomes SENT when a
  // genuinely new support bundle is published. Legacy pre-status filenames are
  // treated as older bundles too, which makes upgrading existing tester SD cards
  // automatic. Already-SENT bundles are intentionally left untouched.
  ec.clear();
  for (const auto& entry : fs::directory_iterator(BundlesRoot(), ec))
  {
    if (ec)
      break;
    if (!entry.is_regular_file(ec))
      continue;

    const std::string name = entry.path().filename().string();
    std::string sent_name;
    if (name.starts_with("UNSENT_Phocoena-Support-") && name.ends_with(".zip"))
      sent_name = "SENT_" + name.substr(std::string("UNSENT_").size());
    else if (name.starts_with("Phocoena-Support-") && name.ends_with(".zip"))
      sent_name = "SENT_" + name;
    else
      continue;

    delivery_renames.emplace_back(entry.path(),
                                  unique_bundle_path(BundlesRoot() / sent_name));
  }
  if (ec)
    return fail_bundle("could not enumerate existing support bundles: " + ec.message());

  std::vector<std::pair<fs::path, fs::path>> completed_renames;
  for (const auto& [from, to] : delivery_renames)
  {
    ec.clear();
    fs::rename(from, to, ec);
    if (ec)
    {
      const std::string rename_error = ec.message();
      for (auto it = completed_renames.rbegin(); it != completed_renames.rend(); ++it)
      {
        std::error_code rollback_ec;
        fs::rename(it->second, it->first, rollback_ec);
      }
      fs::remove(archive_temp, ec);
      if (error)
        *error = "could not mark previous support bundle SENT: " + rename_error;
      return false;
    }
    completed_renames.emplace_back(from, to);
  }

  const fs::path archive = unique_bundle_path(
      BundlesRoot() / ("UNSENT_" + bundle_base + ".zip"));
  ec.clear();
  fs::rename(archive_temp, archive, ec);
  if (ec)
  {
    const std::string publish_error = ec.message();
    for (auto it = completed_renames.rbegin(); it != completed_renames.rend(); ++it)
    {
      std::error_code rollback_ec;
      fs::rename(it->second, it->first, rollback_ec);
    }
    fs::remove(archive_temp, ec);
    if (error)
      *error = "could not publish new UNSENT support bundle: " + publish_error;
    return false;
  }

  for (const fs::path& session : sessions)
    WriteTextAtomic(session / "bundled.marker", std::to_string(static_cast<long long>(now)) + "\n");

  if (output_path) *output_path = archive.string();
  return true;
}
}

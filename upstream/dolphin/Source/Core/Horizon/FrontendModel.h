// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include "Horizon/FileReplace.h"

namespace Horizon
{
inline std::string DisplayText(std::string text)
{
  for (char& c : text)
    if (static_cast<unsigned char>(c) < 32 || c == 127)
      c = '?';
  return text;
}
inline bool IsGameImage(const std::filesystem::path& path)
{
  std::string ext = path.extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
  return ext == ".iso" || ext == ".gcm" || ext == ".rvz" || ext == ".dol" || ext == ".elf";
}
// Lexical containment is sufficient on an SD FAT/exFAT volume without symlinks.
// Refuse other devices, upward traversal, and backslash ambiguity.
inline bool IsSDPath(const std::string& path)
{
  if (!path.starts_with("sdmc:/") || path.find('\\') != std::string::npos || path.find('\0') != std::string::npos)
    return false;
  const auto tail = path.substr(6);
  if (tail.find(':') != std::string::npos)
    return false;
  for (const auto& component : std::filesystem::path(tail))
    if (component == "..")
      return false;
  return true;
}
inline std::string ParentSDPath(std::string path)
{
  if (!IsSDPath(path))
    return "sdmc:/";
  while (path.size() > 6 && path.back() == '/') path.pop_back();
  const auto slash = path.find_last_of('/');
  return slash <= 5 ? "sdmc:/" : path.substr(0, slash + 1);
}
struct Entry { std::string name; std::string path; bool directory; };
inline std::vector<Entry> ReadDirectory(const std::string& path, std::string& error)
{
  std::vector<Entry> entries;
  std::error_code ec;
  std::filesystem::directory_iterator it(path, ec), end;
  while (!ec && it != end)
  {
    const bool dir = it->is_directory(ec);
    if (ec) break;
    if (dir || IsGameImage(it->path()))
      entries.push_back({it->path().filename().string(), it->path().string(), dir});
    it.increment(ec);
  }
  error = ec ? ec.message() : "";
  std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
    return a.directory != b.directory ? a.directory > b.directory : a.name < b.name;
  });
  return entries;
}
struct Settings
{
  bool request_jit = false;
  // Separate opt-in: legacy jit=1 used to mean an unavailable backend request.
  bool native_jit = false;
  bool cached_interpreter = false;
  bool vsync = true;
  bool shader_cache = true;
  bool audio = true;
  bool dual_core = true;
  // v0.1.10 trial: independent of emulated CPU dual-core. Ignored by OpenGL.
  bool vulkan_submit_thread = true;
  // v0.1.14 reduced WSI waits but did not improve the controlled hardware run.
  // Default to the v0.1.12 known-good policy; the A/B controls remain available.
  bool vulkan_present_overlap = false;
  bool vulkan_triple_buffering = false;
  // NXVK supports VK_KHR_push_descriptor. Use it for the high-churn GX
  // sampler set; Off preserves the upstream pooled descriptor-set path.
  bool horizon_vulkan_push_descriptors = true;
  // Required on Switch: acquire and present share mutable libnx NWindow state.
  bool vulkan_present_completion_guard = true;
  // The upstream generated AArch64 loader avoids the portable per-component
  // function pipeline. Software remains available for controlled fallback.
  bool native_vertex_loader = true;
  // Prevent Horizon's small CodeMemory-object limit from forcing per-format
  // vertex loaders back to software. Off reproduces the v0.1.15 allocator.
  bool jit_small_code_pooling = true;
  // Amortize graphics-thread FIFO bookkeeping across a small, bounded burst.
  bool fifo_batching = true;
  // v0.1.26 correctness policy: one disables batching, four remains diagnostic,
  // and eight is the hardware-proven ceiling. Saved 16-block configs migrate to 8.
  std::uint32_t fifo_batch_size = 8;
  // True fastmem is enabled only after Horizon syscall/alias capability probes
  // succeed. Off preserves the v0.1.16 checked page-table path for A/B tests.
  bool horizon_ppc_fastmem = true;
  // One-shot CPU/GPU requests for emulation only. Memory is always read only
  // and remains under Horizon/sys-clk/user control.
  bool phocoena_auto_clocks = true;
  // "performance" requests 1785/768. "legacy" preserves the v0.1.18-v0.1.24
  // 1581/614.4 policy for controlled software-only comparisons.
  std::string phocoena_clock_profile = "performance";
  // Use an isolated, disposable GCI folder for the local Twilight Princess
  // torture benchmark. The normal memory-card directory is untouched.
  bool tp_benchmark_save = false;
  // Sample developer-only hot scopes sparsely enough that instrumentation does
  // not become part of the graphics/FIFO workload it is measuring. Set 64 to
  // reproduce the v0.1.22 hardware trace for a controlled A/B comparison.
  std::uint32_t horizon_perf_sample_scale = 1024;

  // Tester builds use one fixed, hardware-validated baseline. Loading an older
  // switch.ini must never silently change the settings used for compatibility
  // or performance measurements. Keep this in sync with package-testers.py.
  void ApplyFrozenTesterBaseline()
  {
    request_jit = false;
    native_jit = true;
    cached_interpreter = false;
    vsync = true;
    shader_cache = true;
    audio = true;
    dual_core = true;
    vulkan_submit_thread = true;
    vulkan_present_overlap = false;
    vulkan_triple_buffering = false;
    horizon_vulkan_push_descriptors = true;
    vulkan_present_completion_guard = true;
    native_vertex_loader = true;
    jit_small_code_pooling = true;
    fifo_batching = true;
    fifo_batch_size = 8;
    horizon_ppc_fastmem = true;
    phocoena_auto_clocks = true;
    phocoena_clock_profile = "performance";
    tp_benchmark_save = false;
    horizon_perf_sample_scale = 1024;
  }

  std::uint32_t EffectiveFifoBatchSize() const
  {
    if (!fifo_batching)
      return 1;
    if (fifo_batch_size >= 8)
      return 8;
    if (fifo_batch_size >= 4)
      return 4;
    return 1;
  }
  const char* FifoBatchName() const
  {
    switch (EffectiveFifoBatchSize())
    {
    case 4: return "4";
    case 8: return "8 (hardware-safe)";
    default: return "1 (Off)";
    }
  }
  void CycleFifoBatchSize()
  {
    switch (EffectiveFifoBatchSize())
    {
    case 1: fifo_batching = true; fifo_batch_size = 4; break;
    case 4: fifo_batch_size = 8; break;
    case 8: fifo_batching = false; fifo_batch_size = 1; break;
    default: fifo_batching = false; fifo_batch_size = 1; break;
    }
  }
  const char* CPUName() const
  {
    return native_jit ? "AArch64 JIT (experimental)" :
        request_jit ? "Legacy JIT request (Interpreter)" :
        cached_interpreter ? "Cached Interpreter (experimental)" : "Interpreter";
  }
  void CycleCPU()
  {
    if (request_jit || native_jit) { request_jit = false; native_jit = false; cached_interpreter = false; }
    else if (cached_interpreter) { cached_interpreter = false; native_jit = true; }
    else cached_interpreter = true;
  }
  bool Load(const std::string& path)
  {
    std::ifstream stream(path);
    if (!stream) return false;
    std::string line;
    while (std::getline(stream, line))
    {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      const auto eq = line.find('=');
      if (eq == std::string::npos) continue;
      const auto key = line.substr(0, eq), value = line.substr(eq + 1);
      if (key == "fifo_batch_size")
      {
        if (value == "1" || value == "4" || value == "8" || value == "16")
        {
          fifo_batch_size = static_cast<std::uint32_t>(std::stoul(value));
          if (fifo_batch_size > 8)
            fifo_batch_size = 8;
          fifo_batching = fifo_batch_size > 1;
        }
        continue;
      }
      if (key == "horizon_perf_sample_scale")
      {
        if (value == "64" || value == "1024")
          horizon_perf_sample_scale = static_cast<std::uint32_t>(std::stoul(value));
        continue;
      }
      if (key == "phocoena_clock_profile")
      {
        if (value == "performance" || value == "legacy")
          phocoena_clock_profile = value;
        continue;
      }
      if (value != "0" && value != "1") continue;
      const bool on = value == "1";
      if (key == "jit") request_jit = on;
      else if (key == "native_jit") native_jit = on;
      else if (key == "cached_interpreter") cached_interpreter = on;
      else if (key == "vsync") vsync = on;
      else if (key == "shader_cache") shader_cache = on;
      else if (key == "audio") audio = on;
      else if (key == "dual_core") dual_core = on;
      else if (key == "vulkan_submit_thread") vulkan_submit_thread = on;
      else if (key == "vulkan_present_overlap") vulkan_present_overlap = on;
      else if (key == "vulkan_triple_buffering") vulkan_triple_buffering = on;
      else if (key == "horizon_vulkan_push_descriptors") horizon_vulkan_push_descriptors = on;
      else if (key == "vulkan_present_completion_guard") vulkan_present_completion_guard = on;
      else if (key == "native_vertex_loader") native_vertex_loader = on;
      else if (key == "jit_small_code_pooling") jit_small_code_pooling = on;
      else if (key == "fifo_batching") fifo_batching = on;
      else if (key == "horizon_ppc_fastmem") horizon_ppc_fastmem = on;
      else if (key == "phocoena_auto_clocks") phocoena_auto_clocks = on;
      else if (key == "tp_benchmark_save") tp_benchmark_save = on;
    }
    return true;
  }
  bool Save(const std::string& path) const
  {
    // Write a replacement file first; never truncate the last valid settings.
    const auto temp = path + ".tmp";
    std::ofstream stream(temp, std::ios::trunc);
    stream << "# Phocoena v0.1\njit=" << request_jit << "\nvsync=" << vsync
           << "\nnative_jit=" << native_jit
           << "\ncached_interpreter=" << cached_interpreter
           << "\nshader_cache=" << shader_cache << "\naudio=" << audio
           << "\ndual_core=" << dual_core
           << "\nvulkan_submit_thread=" << vulkan_submit_thread
           << "\nvulkan_present_overlap=" << vulkan_present_overlap
           << "\nvulkan_triple_buffering=" << vulkan_triple_buffering
           << "\nhorizon_vulkan_push_descriptors=" << horizon_vulkan_push_descriptors
           << "\nvulkan_present_completion_guard=" << vulkan_present_completion_guard
           << "\nnative_vertex_loader=" << native_vertex_loader << '\n';
    stream << "jit_small_code_pooling=" << jit_small_code_pooling << '\n';
    const auto effective_fifo_batch_size = EffectiveFifoBatchSize();
    stream << "fifo_batching=" << (effective_fifo_batch_size > 1) << '\n';
    stream << "fifo_batch_size=" << effective_fifo_batch_size << '\n';
    stream << "horizon_ppc_fastmem=" << horizon_ppc_fastmem << '\n';
    stream << "phocoena_auto_clocks=" << phocoena_auto_clocks << '\n';
    stream << "phocoena_clock_profile=" << phocoena_clock_profile << '\n';
    stream << "tp_benchmark_save=" << tp_benchmark_save << '\n';
    stream << "horizon_perf_sample_scale=" << horizon_perf_sample_scale << '\n';
    stream.flush();
    if (!stream) return false;
    stream.close();
    if (!stream) return false;
    std::error_code error;
    ReplaceFile(temp, path, error);
    return !error;
  }
};
}

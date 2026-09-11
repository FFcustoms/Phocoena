// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace Core
{
class System;
}

namespace Horizon
{
// These addresses and layouts are specific to the NTSC-U GZ2E01 executable.
// They are derived from the matching zeldaret/tp decomp and guarded by game ID
// plus a live-stage sanity check before any guest-memory write is made.
constexpr std::uint32_t TP_GAME_INFO_ADDRESS = 0x804061c0;
constexpr std::uint32_t TP_CURRENT_STAGE_ADDRESS = TP_GAME_INFO_ADDRESS + 0x0f38 + 0x03ec8;
constexpr std::uint32_t TP_NEXT_STAGE_ADDRESS = TP_GAME_INFO_ADDRESS + 0x0f38 + 0x03ed6;
constexpr std::uint32_t TP_RESTART_LAST_MODE_ADDRESS = TP_GAME_INFO_ADDRESS + 0x0dd0;

struct TPWarpPoint
{
  const char* label;
  std::array<char, 8> stage;
  std::int8_t room;
  std::int16_t spawn;
  std::int8_t layer;
  const char* route;
};

constexpr std::array<char, 8> TPStage(const char (&name)[8])
{
  return {name[0], name[1], name[2], name[3], name[4], name[5], name[6], '\0'};
}

inline constexpr std::array<TPWarpPoint, 4> TP_WARP_POINTS{{
    {"Hyrule Field - Torture 1", TPStage("F_SP121"), 10, 0, -1,
     "Lanayru Main Field: stand 5 s, turn camera left, ride north 20 s, rotate camera 360 degrees"},
    {"Hyrule Field - Torture 2", TPStage("F_SP121"), 0, 0, -1,
     "Eldin Main Field: stand 5 s, turn camera left, ride forward 20 s, rotate camera 360 degrees"},
    {"Faron Woods", TPStage("F_SP108"), 5, 0, -1,
     "Mist Area: stand 5 s, walk forward 20 s, rotate camera 360 degrees"},
    {"Gerudo Desert", TPStage("F_SP124"), 0, 0, -1,
     "Main Desert: stand 5 s, walk forward 20 s, rotate camera 360 degrees"},
}};

struct TPMenuInput
{
  bool toggle = false;
  bool up = false;
  bool down = false;
  bool activate = false;
  bool close = false;
  int stick_y = 0;
};

struct TPMenuResult
{
  bool changed = false;
  bool closed = false;
  std::optional<std::size_t> warp;
};

class TPMenuState
{
public:
  TPMenuResult Update(const TPMenuInput& input)
  {
    TPMenuResult result;
    if (input.toggle)
    {
      m_open = !m_open;
      result.changed = true;
      result.closed = !m_open;
      m_stick_latched = false;
      return result;
    }
    if (!m_open)
      return result;
    if (input.close)
    {
      m_open = false;
      result.changed = true;
      result.closed = true;
      m_stick_latched = false;
      return result;
    }
    const bool stick_up = input.stick_y > 18000;
    const bool stick_down = input.stick_y < -18000;
    const bool move_up = input.up || (stick_up && !m_stick_latched);
    const bool move_down = input.down || (stick_down && !m_stick_latched);
    if (!stick_up && !stick_down)
      m_stick_latched = false;
    else
      m_stick_latched = true;
    if (move_up)
    {
      m_selected = (m_selected + TP_WARP_POINTS.size() - 1) % TP_WARP_POINTS.size();
      result.changed = true;
    }
    if (move_down)
    {
      m_selected = (m_selected + 1) % TP_WARP_POINTS.size();
      result.changed = true;
    }
    if (input.activate)
    {
      result.warp = m_selected;
      result.changed = true;
      result.closed = true;
      m_open = false;
      m_stick_latched = false;
    }
    return result;
  }

  bool IsOpen() const { return m_open; }
  std::size_t Selected() const { return m_selected; }

private:
  bool m_open = false;
  bool m_stick_latched = false;
  std::size_t m_selected = 0;
};

// Rebuild an isolated, disposable benchmark file without relying on
// copy_options::overwrite_existing. Horizon's filesystem implementation can
// return EEXIST for that operation even when overwrite was requested. The
// protected template is never renamed or removed, and the active copy is not
// removed until a complete staging copy has been verified.
inline bool CopyBenchmarkFileBytes(const std::filesystem::path& source,
                                   const std::filesystem::path& destination, std::string& error)
{
  const std::string source_string = source.string();
  const std::string destination_string = destination.string();
  errno = 0;
  std::FILE* input = std::fopen(source_string.c_str(), "rb");
  if (!input)
  {
    error = "cannot open benchmark template: " + std::string(std::strerror(errno));
    return false;
  }

  errno = 0;
  std::FILE* output = std::fopen(destination_string.c_str(), "wb");
  if (!output)
  {
    const int saved_errno = errno;
    std::fclose(input);
    error = "cannot create benchmark staging file: " +
            std::string(std::strerror(saved_errno));
    return false;
  }

  const auto fail = [&](const char* operation) {
    const int saved_errno = errno;
    if (output)
      std::fclose(output);
    if (input)
      std::fclose(input);
    error = operation;
    if (saved_errno != 0)
      error += ": " + std::string(std::strerror(saved_errno));
    return false;
  };

  std::array<unsigned char, 16 * 1024> buffer{};
  while (true)
  {
    errno = 0;
    const std::size_t bytes_read = std::fread(buffer.data(), 1, buffer.size(), input);
    if (bytes_read != 0)
    {
      errno = 0;
      if (std::fwrite(buffer.data(), 1, bytes_read, output) != bytes_read)
        return fail("cannot write benchmark staging file");
    }
    if (bytes_read != buffer.size())
    {
      if (std::ferror(input))
        return fail("cannot read benchmark template");
      break;
    }
  }

  errno = 0;
  if (std::fflush(output) != 0)
    return fail("cannot flush benchmark staging file");

  errno = 0;
  const int output_close_result = std::fclose(output);
  output = nullptr;
  if (output_close_result != 0)
    return fail("cannot close benchmark staging file");

  errno = 0;
  const int input_close_result = std::fclose(input);
  input = nullptr;
  if (input_close_result != 0)
    return fail("cannot close benchmark template");

  error.clear();
  return true;
}

inline bool ResetDisposableBenchmarkFile(const std::filesystem::path& source,
                                         const std::filesystem::path& destination,
                                         std::uintmax_t expected_size, std::string& error)
{
  namespace fs = std::filesystem;
  if (source == destination)
  {
    error = "benchmark source and runtime paths are identical";
    return false;
  }

  const fs::path temporary = destination.string() + ".tmp";
  std::error_code ec;
  fs::remove(temporary, ec);
  if (ec)
  {
    error = "cannot clear benchmark staging file: " + ec.message();
    return false;
  }

  if (!CopyBenchmarkFileBytes(source, temporary, error))
    return false;

  ec.clear();
  const std::uintmax_t staged_size = fs::file_size(temporary, ec);
  if (ec || staged_size != expected_size)
  {
    error = ec ? "cannot verify staged benchmark GCI: " + ec.message() :
                 "staged benchmark GCI has unexpected size";
    return false;
  }

  // This destination is the hard-coded disposable runtime card. A power loss
  // here can only remove that disposable copy; the template remains intact and
  // the next boot reconstructs the runtime card.
  ec.clear();
  fs::remove(destination, ec);
  if (ec)
  {
    error = "cannot clear benchmark runtime GCI: " + ec.message();
    return false;
  }

  ec.clear();
  fs::rename(temporary, destination, ec);
  if (ec)
  {
    error = "cannot activate benchmark GCI: " + ec.message();
    return false;
  }

  ec.clear();
  const std::uintmax_t active_size = fs::file_size(destination, ec);
  if (ec || active_size != expected_size)
  {
    error = ec ? "cannot verify active benchmark GCI: " + ec.message() :
                 "active benchmark GCI has unexpected size";
    return false;
  }

  error.clear();
  return true;
}

bool PrepareTPBenchmarkSaveEnvironment(std::string_view root, std::string& error);
std::string TPBenchmarkRuntimeGCIFolder(std::string_view root);
void ShowTPDeveloperMenu(const TPMenuState& menu);
void HideTPDeveloperMenu();
bool RequestTPWarp(Core::System& system, const TPWarpPoint& point);
}  // namespace Horizon

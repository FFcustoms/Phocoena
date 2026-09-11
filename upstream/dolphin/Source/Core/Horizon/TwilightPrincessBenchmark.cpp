// SPDX-License-Identifier: GPL-2.0-or-later
#include "Horizon/TwilightPrincessBenchmark.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <limits>
#include <system_error>

#include <fmt/format.h>

#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/PowerPC/MMU.h"
#include "Horizon/Log.h"
#include "VideoCommon/OnScreenDisplay.h"

namespace Horizon
{
namespace
{
constexpr std::string_view TEMPLATE_RELATIVE =
    "benchmark-saves/twilight-princess/template/01-GZ2E-gczelda2.gci";
constexpr std::string_view RUNTIME_RELATIVE =
    "benchmark-saves/twilight-princess/runtime/Card A/01-GZ2E-gczelda2.gci";

bool IsSaneStageName(const std::string& stage)
{
  return !stage.empty() && stage.size() <= 7 &&
         std::all_of(stage.begin(), stage.end(), [](unsigned char c) {
           return std::isalnum(c) || c == '_';
         });
}
}  // namespace

std::string TPBenchmarkRuntimeGCIFolder(std::string_view root)
{
  return std::string(root) + "benchmark-saves/twilight-princess/runtime/Card A/";
}

bool PrepareTPBenchmarkSaveEnvironment(std::string_view root, std::string& error)
{
  const std::filesystem::path source = std::string(root) + std::string(TEMPLATE_RELATIVE);
  const std::filesystem::path destination = std::string(root) + std::string(RUNTIME_RELATIVE);
  std::error_code ec;
  if (!std::filesystem::is_regular_file(source, ec) || ec)
  {
    error = fmt::format("benchmark GCI missing: {}", source.string());
    return false;
  }
  if (std::filesystem::file_size(source, ec) != 32832 || ec)
  {
    error = fmt::format("benchmark GCI has unexpected size: {}", source.string());
    return false;
  }
  std::filesystem::create_directories(destination.parent_path(), ec);
  if (ec)
  {
    error = fmt::format("cannot create benchmark runtime card: {}", ec.message());
    return false;
  }
  return ResetDisposableBenchmarkFile(source, destination, 32832, error);
}

void ShowTPDeveloperMenu(const TPMenuState& menu)
{
  std::string text = "Phocoena Developer\nTeleport\n";
  for (std::size_t i = 0; i < TP_WARP_POINTS.size(); ++i)
    text += fmt::format("{} {}\n", i == menu.Selected() ? ">" : " ", TP_WARP_POINTS[i].label);
  text += "\nD-pad / stick: select   A: teleport   B / -: close";
  OSD::AddTypedMessage(OSD::MessageType::HorizonDeveloperMenu, std::move(text),
                       std::numeric_limits<std::uint32_t>::max(), 0xff39ff14);
}

void HideTPDeveloperMenu()
{
  OSD::AddTypedMessage(OSD::MessageType::HorizonDeveloperMenu, "", 0, 0xff39ff14);
}

bool RequestTPWarp(Core::System& system, const TPWarpPoint& point)
{
  if (SConfig::GetInstance().GetGameID() != "GZ2E01" || Core::GetState(system) != Core::State::Running)
  {
    Error("TP benchmark", "warp refused: GZ2E01 must be running");
    return false;
  }

  Core::RunOnCPUThread(system, [&system, point] {
    const Core::CPUThreadGuard guard(system);
    if (SConfig::GetInstance().GetGameID() != "GZ2E01")
    {
      Error("TP benchmark", "warp callback refused after game ID changed");
      return;
    }
    const std::string current =
        PowerPC::MMU::HostGetString(guard, TP_CURRENT_STAGE_ADDRESS, 8);
    if (!IsSaneStageName(current))
    {
      Error("TP benchmark", "warp refused: live stage signature invalid at 0x%08x",
            TP_CURRENT_STAGE_ADDRESS);
      return;
    }

    // Match Twilight Princess/Twilight Princess GZ's normal next-stage request.
    // Fields are written first and enabled last so the guest cannot observe a
    // partially initialized request.
    for (std::size_t i = 0; i < point.stage.size(); ++i)
      PowerPC::MMU::HostWrite<std::uint8_t>(guard, static_cast<std::uint8_t>(point.stage[i]),
                                            TP_NEXT_STAGE_ADDRESS + i);
    PowerPC::MMU::HostWrite<std::uint16_t>(guard, static_cast<std::uint16_t>(point.spawn),
                                           TP_NEXT_STAGE_ADDRESS + 0x08);
    PowerPC::MMU::HostWrite<std::uint8_t>(guard, static_cast<std::uint8_t>(point.room),
                                          TP_NEXT_STAGE_ADDRESS + 0x0a);
    PowerPC::MMU::HostWrite<std::uint8_t>(guard, static_cast<std::uint8_t>(point.layer),
                                          TP_NEXT_STAGE_ADDRESS + 0x0b);
    PowerPC::MMU::HostWrite<std::uint8_t>(guard, 13, TP_NEXT_STAGE_ADDRESS + 0x0f);
    PowerPC::MMU::HostWrite<std::uint32_t>(guard, 0, TP_RESTART_LAST_MODE_ADDRESS);
    PowerPC::MMU::HostWrite<std::uint8_t>(guard, 1, TP_NEXT_STAGE_ADDRESS + 0x0e);
    Log("TP benchmark", "warp requested current=%s target=%s room=%d spawn=%d layer=%d route=%s",
        current.c_str(), point.stage.data(), point.room, point.spawn, point.layer, point.route);
  });
  return true;
}
}  // namespace Horizon

// SPDX-License-Identifier: GPL-2.0-or-later
#include "Core/Host.h"
#include "Horizon/Log.h"
#include <atomic>
namespace Horizon { std::atomic<bool> menu_active{true}; }
std::vector<std::string> Host_GetPreferredLocales() { return {"en"}; }
bool Host_UIBlocksControllerState() { return Horizon::menu_active.load(); }
bool Host_RendererHasFocus() { return !Horizon::menu_active.load(); }
bool Host_RendererHasFullFocus() { return Host_RendererHasFocus(); }
bool Host_RendererIsFullscreen() { return true; }
bool Host_TASInputHasFocus() { return false; }
void Host_Message(HostMessageID id)
{
  if (id == HostMessageID::WMUserStop) Horizon::stop_requested = true;
}
void Host_PPCSymbolsChanged() {}
void Host_PPCBreakpointsChanged() {}
void Host_RequestRenderWindowSize(int, int) {}
void Host_UpdateDisasmDialog() {}
void Host_JitCacheInvalidation() {}
void Host_JitProfileDataWiped() {}
void Host_UpdateTitle(const std::string& title)
{
  std::string branded = title;
  if (branded.starts_with("Dolphin"))
    branded.replace(0, 7, "Phocoena");
  Horizon::Log("Title", "%s", branded.c_str());
}
void Host_YieldToUI() {}
void Host_TitleChanged() {}
void Host_UpdateDiscordClientID(const std::string&) {}
bool Host_UpdateDiscordPresenceRaw(const std::string&, const std::string&, const std::string&,
    const std::string&, const std::string&, const std::string&, int64_t, int64_t, int, int) { return false; }
std::unique_ptr<GBAHostInterface> Host_CreateGBAHost(std::weak_ptr<HW::GBA::Core>) { return nullptr; }

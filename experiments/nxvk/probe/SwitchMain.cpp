// SPDX-License-Identifier: GPL-2.0-or-later
#include "Probe.h"
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <switch.h>
#include "Horizon/Console.h"
#include "Horizon/Log.h"
#include "BuildIdentity.h"

extern "C"
{
u32 __nx_applet_type = AppletType_Application;
size_t __nx_heap_size = 0;
bool HorizonOpenCrashLog(void);
void HorizonCloseCrashLog(void);
}

namespace
{
FILE* logfile = nullptr;
std::mutex log_mutex;
std::string last_error;
constexpr const char* LOG_PATH = "sdmc:/switch/dolphin/logs/vulkan-probe.log";
void Write(bool error, const char* subsystem, const char* format, va_list args)
{
  char message[4096];
  std::vsnprintf(message, sizeof(message), format, args);
  std::lock_guard lock(log_mutex);
  if (error) last_error = std::string(subsystem) + ": " + message;
  if (logfile)
  {
    std::fprintf(logfile, "[%llu ms] %s: %s: %s\n",
        static_cast<unsigned long long>(armTicksToNs(armGetSystemTick()) / 1000000),
        error ? "ERROR" : "INFO", subsystem, message);
    std::fflush(logfile);
  }
}
bool OpenLog()
{
  mkdir("sdmc:/switch", 0777); mkdir("sdmc:/switch/dolphin", 0777);
  mkdir("sdmc:/switch/dolphin/logs", 0777);
  struct stat status{};
  if (stat(LOG_PATH, &status) == 0)
  {
    const char* previous = "sdmc:/switch/dolphin/logs/vulkan-probe.previous.log";
    std::remove(previous);
    if (std::rename(LOG_PATH, previous) != 0) return false;
  }
  logfile = std::fopen(LOG_PATH, "w");
  return logfile != nullptr;
}
void StartupFailure(const char* message)
{
  // No driver is started if logs cannot be opened. Still show the reason.
  try
  {
    Horizon::InitializeConsole();
    std::printf("Vulkan Probe startup failed\n\n%s\n\nPLUS: exit\n", message);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad{}; padInitializeDefault(&pad);
    while (appletMainLoop())
    {
      consoleUpdate(nullptr); padUpdate(&pad);
      if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
    }
    Horizon::ShutdownConsole();
  }
  catch (...) { /* No graphics/SD recovery is available at this point. */ }
}
}
namespace Horizon
{
std::atomic<bool> stop_requested{false};
void Log(const char* subsystem, const char* format, ...)
{
  va_list args; va_start(args, format); Write(false, subsystem, format, args); va_end(args);
}
void Error(const char* subsystem, const char* format, ...)
{
  va_list args; va_start(args, format); Write(true, subsystem, format, args); va_end(args);
}
}

int main()
{
  int result = 0;
  bool suspended = false;
  if (!OpenLog())
  {
    StartupFailure("Cannot open /switch/dolphin/logs/vulkan-probe.log.\nCheck SD free space and write access.");
    return 1;
  }
  if (!HorizonOpenCrashLog())
  {
    Horizon::Error("Startup", "cannot open separate Vulkan crash log");
    StartupFailure("Cannot open the Vulkan crash log. See vulkan-probe.log.");
    std::fclose(logfile); return 1;
  }
  Horizon::Log("Build", "Dolphin Vulkan Probe %s; NXVK %s; built %s", PROBE_VERSION, NXVK_COMMIT, PROBE_DATE);
  Horizon::Log("Build", "recipe sha256=%s; GCC %s; libnx 4.12.0; Rust nightly-2026-07-02; SDK %s",
      PROBE_RECIPE_SHA256, __VERSION__, SDK_DIGEST);
  const u32 version = hosversionGet();
  Horizon::Log("Environment", "Horizon=%u.%u.%u Atmosphere=%s applet_type=%d; no clock changes",
      HOSVER_MAJOR(version), HOSVER_MINOR(version), HOSVER_MICRO(version),
      hosversionIsAtmosphere() ? "yes" : "not detected", static_cast<int>(appletGetAppletType()));
  u64 total = 0, used = 0;
  const Result total_result = svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
  const Result used_result = svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
  Horizon::Log("Memory", "total=%llu reserved/used=%llu query=0x%x/0x%x",
      static_cast<unsigned long long>(total), static_cast<unsigned long long>(used), total_result, used_result);
  const Thread* main_thread = threadGetSelf();
  Horizon::Log("Thread", "loader-provided main stack=%zu bytes; default pthread workers request 2 MiB",
      main_thread ? main_thread->stack_sz : 0);
  // This is NXVK's documented opt-in for its experimental Maxwell driver.
  setenv("NVK_I_WANT_A_BROKEN_VULKAN_DRIVER", "1", 1);
  setenv("MESA_SHADER_CACHE_DIR", "sdmc:/switch/dolphin/cache/vulkan-probe", 1);
  setenv("MESA_SHADER_CACHE_MAX_SIZE", "32M", 1);
  try
  {
    Horizon::InitializeConsole();
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad{}; padInitializeDefault(&pad);
    std::string status = "NOT RUN";
    bool redraw = true;
    while (appletMainLoop())
    {
      if (redraw)
      {
        consoleClear();
        std::printf("Dolphin Vulkan Probe %s\n\n", PROBE_VERSION);
        std::puts("Separate driver diagnostic. Dolphin 0.1.8 is unchanged.\n");
        std::puts("A: run pixel/readback + visible triangle test");
        std::puts("PLUS: exit\n\nUse full title takeover, not Album.");
        std::printf("\nResult: %s\n", status.c_str());
        if (!last_error.empty()) std::printf("\nERROR: %s\n", last_error.c_str());
        std::puts("\nLog: /switch/dolphin/logs/vulkan-probe.log");
        redraw = false;
      }
      consoleUpdate(nullptr);
      padUpdate(&pad);
      const u64 pressed = padGetButtonsDown(&pad);
      if (pressed & HidNpadButton_Plus) break;
      if (!(pressed & HidNpadButton_A)) continue;
      last_error.clear();
      Horizon::Log("Probe", "BEGIN user-requested diagnostic cycle");
      Horizon::SuspendConsole(); suspended = true;
      try
      {
        RunVulkanProbe(true);
        status = last_error.empty() ? "PASS pixel/readback + presentation; confirm triangle looked correct" :
            "FAIL - platform/driver reported an error; see log";
      }
      catch (const std::exception& e)
      {
        Horizon::Error("Vulkan probe", "%s", e.what());
        status = "FAIL - see error and log";
      }
      Horizon::ResumeConsole(); suspended = false;
      if (!last_error.empty()) status = "FAIL - platform/driver reported an error; see log";
      Horizon::Log("Probe", "END cycle; console restored; %s", status.c_str());
      redraw = true;
    }
    Horizon::ShutdownConsole();
  }
  catch (const std::exception& e)
  {
    Horizon::Error("Frontend", "%s; console suspended=%d", e.what(), suspended);
    result = 1;
  }
  Horizon::Log("Exit", "result=%d", result);
  HorizonCloseCrashLog();
  std::fclose(logfile); logfile = nullptr;
  return result;
}

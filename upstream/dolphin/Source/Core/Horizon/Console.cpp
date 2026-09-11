// SPDX-License-Identifier: GPL-2.0-or-later
#include "Horizon/Console.h"

#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <sys/iosupport.h>
#include <switch.h>

#include "Horizon/Log.h"
#include "Horizon/Display.h"

namespace Horizon
{
namespace
{
bool console_active = false;
const devoptab_t* console_output = nullptr;

ssize_t CaptureOutput(struct _reent*, void*, const char* data, size_t length)
{
  // Mesa/newlib must never write through the console renderer while its
  // framebuffer is closed. The log uses its own SD FILE, not stdout/stderr.
  for (size_t offset = 0; offset < length;)
  {
    const size_t count = std::min(length - offset, size_t{1024});
    Log("Driver/stdio", "%.*s", static_cast<int>(count), data + offset);
    offset += count;
  }
  return static_cast<ssize_t>(length);
}

const devoptab_t captured_output = [] {
  devoptab_t device{};
  device.name = "dolphin-log";
  device.write_r = CaptureOutput;
  return device;
}();

void PrepareWindow(const char* owner)
{
  if (!PrepareDisplayWindow(owner))
    throw std::runtime_error("Display handoff failed; see preceding log");
}

void StartConsole()
{
  BeginDisplayTrace();
  Log("Console", "BEGIN consoleInit");
  PrintConsole* console = consoleInit(nullptr);
  // libnx returns a non-null pointer even when framebuffer initialization fails.
  if (!console || !console->consoleInitialised)
  {
    devoptab_list[STD_OUT] = &captured_output;
    Error("Console", "consoleInit failed; refusing to draw through a closed framebuffer");
    throw std::runtime_error("Console framebuffer initialization failed; see log");
  }
  if (!console_output)
    console_output = devoptab_list[STD_OUT];
  devoptab_list[STD_OUT] = console_output;
  console_active = true;
  Log("Console", "consoleInit READY");
}
}

void InitializeConsole()
{
  devoptab_list[STD_ERR] = &captured_output;
  setvbuf(stderr, nullptr, _IONBF, 0);
  StartConsole();
}

void SuspendConsole()
{
  if (!console_active)
    throw std::runtime_error("Console: attempted a second graphics handoff");
  Log("Console", "BEGIN consoleExit for graphics");
  fflush(stdout);
  devoptab_list[STD_OUT] = &captured_output;
  consoleExit(nullptr);
  console_active = false;
  Log("Console", "consoleExit complete");
#ifdef HORIZON_VULKAN
  PrepareWindow("Vulkan");
#else
  PrepareWindow("OpenGL");
#endif
  BeginDisplayTrace();
}

void ResumeConsole()
{
  if (console_active)
    throw std::runtime_error("Console: attempted to resume an active framebuffer");
  PrepareWindow("console");
  StartConsole();
  // A single successful present did not prove the handoff in v0.1.2: the next
  // dequeue aborted. Cycle the restored queue before reporting it ready.
  Log("Console", "BEGIN restored buffer cycle check (6 swaps)");
  for (unsigned frame = 0; frame < 6; ++frame)
    consoleUpdate(nullptr);
  Log("Console", "restored buffer cycle check COMPLETE (6 swaps)");
}

void ShutdownConsole()
{
  fflush(stdout);
  devoptab_list[STD_OUT] = &captured_output;
  if (console_active)
  {
    Log("Console", "BEGIN final consoleExit");
    consoleExit(nullptr);
    console_active = false;
    Log("Console", "final consoleExit complete");
  }
}
}

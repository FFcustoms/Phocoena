// SPDX-License-Identifier: GPL-2.0-or-later
#include "Probe.h"
#include <cstdarg>
#include <cstdio>
#include <dlfcn.h>
#include <exception>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include "Horizon/Log.h"

namespace
{
PFN_vkGetInstanceProcAddr get_instance_proc = nullptr;
void Write(const char* level, const char* subsystem, const char* format, va_list args)
{
  std::printf("%s: %s: ", level, subsystem);
  std::vprintf(format, args);
  std::puts("");
}
}
extern "C" PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance, const char* name)
{
  return get_instance_proc(instance, name);
}
namespace Horizon
{
void Log(const char* subsystem, const char* format, ...)
{
  va_list args; va_start(args, format); Write("INFO", subsystem, format, args); va_end(args);
}
void Error(const char* subsystem, const char* format, ...)
{
  va_list args; va_start(args, format); Write("ERROR", subsystem, format, args); va_end(args);
}
}
int main()
{
  void* library = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!library) { std::fprintf(stderr, "%s\n", dlerror()); return 1; }
  get_instance_proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(library, "vkGetInstanceProcAddr"));
  if (!get_instance_proc) { dlclose(library); return 1; }
  int result = 0;
  try
  {
    // Repeat all creation, shader execution, readback and destruction.
    // This validates the application commands, not NXVK or Horizon WSI.
    for (unsigned run = 0; run < 3; ++run) RunVulkanProbe(false);
    std::puts("PASS: three host Vulkan triangle/readback/lifecycle cycles; Horizon untested");
  }
  catch (const std::exception& e) { std::fprintf(stderr, "ERROR: %s\n", e.what()); result = 1; }
  dlclose(library);
  return result;
}

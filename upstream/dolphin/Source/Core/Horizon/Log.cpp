// SPDX-License-Identifier: GPL-2.0-or-later
#include "Horizon/Log.h"
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <malloc.h>
#include <unistd.h>
#include <sys/stat.h>
#include <switch.h>
#include "Common/Logging/LogManager.h"
#include "Horizon/BuildInfo.h"
#include "Horizon/Performance.h"
#include "Horizon/LogReport.h"
extern "C" char* fake_heap_end;

namespace Horizon
{
std::atomic<bool> stop_requested{false};
namespace
{
std::mutex log_mutex;
FILE* logfile = nullptr;
std::string last_error;
void Write(bool error, const char* subsystem, const char* format, va_list args)
{
  const ScopedPerfSample perf(PerfMetric::LogWrite);
  char message[4096];
  vsnprintf(message, sizeof(message), format, args);
  std::lock_guard lock(log_mutex);
  if (error)
    last_error = std::string(subsystem) + ": " + message;
  if (logfile)
  {
    const auto ms = armTicksToNs(armGetSystemTick()) / 1000000;
    fprintf(logfile, "[%llu ms] %s: %s: %s\n", static_cast<unsigned long long>(ms),
            error ? "ERROR" : "INFO", subsystem, message);
    // Errors are immediately durable. Routine INFO is deliberately buffered
    // and flushed by the 5-second report path so SD I/O cannot become a frame
    // pacing workload of its own.
    if (error)
      fflush(logfile);
  }
}
class CoreLog final : public Common::Log::LogListener
{
  void Log(Common::Log::LogLevel level, const char* msg) override
  {
    if (level == Common::Log::LogLevel::LERROR)
      Error("Upstream", "%s", msg);
    else
      Horizon::Log("Upstream", "%s", msg);
  }
};
}
bool OpenLog()
{
  mkdir("sdmc:/switch", 0777);
  mkdir(ROOT, 0777);
  for (const char* sub : {"games", "config", "cache", "shaders", "logs"})
    mkdir((std::string(ROOT) + sub).c_str(), 0777);
  // Keep the previous session, useful if startup fails before core logging.
  // Only rotate the previous backup if there is a current session to preserve.
  struct stat status{};
  if (stat((std::string(ROOT) + "logs/phocoena.log").c_str(), &status) == 0)
    std::remove((std::string(ROOT) + "logs/phocoena.previous.log").c_str());
  std::rename((std::string(ROOT) + "logs/phocoena.log").c_str(),
              (std::string(ROOT) + "logs/phocoena.previous.log").c_str());
  logfile = fopen((std::string(ROOT) + "logs/phocoena.log").c_str(), "w");
  if (logfile)
    setvbuf(logfile, nullptr, _IOFBF, 64 * 1024);
  return logfile != nullptr;
}
void CloseLog()
{
  std::lock_guard lock(log_mutex);
  if (logfile)
    fclose(logfile);
  logfile = nullptr;
}
void FlushLog()
{
  std::lock_guard lock(log_mutex);
  if (logfile)
    fflush(logfile);
}
void Log(const char* subsystem, const char* format, ...)
{
  va_list args;
  va_start(args, format);
  Write(false, subsystem, format, args);
  va_end(args);
}
void LogReport(std::string_view report)
{
  const ScopedPerfSample perf(PerfMetric::LogWrite);
  std::lock_guard lock(log_mutex);
  if (logfile)
    WriteInfoReport(logfile, armTicksToNs(armGetSystemTick()) / 1000000, report);
}
void Error(const char* subsystem, const char* format, ...)
{
  va_list args;
  va_start(args, format);
  Write(true, subsystem, format, args);
  va_end(args);
}
std::string LastError()
{
  std::lock_guard lock(log_mutex);
  return last_error;
}
void ClearLastError()
{
  std::lock_guard lock(log_mutex);
  last_error.clear();
}
void AttachCoreLogger()
{
  using namespace Common::Log;
  auto* manager = LogManager::GetInstance();
  manager->RegisterListener(LogListener::LOG_WINDOW_LISTENER, std::make_unique<CoreLog>());
  manager->EnableListener(LogListener::LOG_WINDOW_LISTENER, true);
  manager->EnableListener(LogListener::CONSOLE_LISTENER, false);
  manager->EnableListener(LogListener::FILE_LISTENER, false);
  manager->SetConfigLogLevel(LogLevel::LWARNING);
  for (int i = 0; i <= static_cast<int>(LAST_LOG_TYPE); ++i)
    manager->SetEnable(static_cast<LogType>(i), true);
}
void LogEnvironment()
{
  Log("Build", "Phocoena v%s; upstream core %s; built %s", DOLPHIN_PORT_VERSION, DOLPHIN_UPSTREAM_COMMIT, DOLPHIN_BUILD_DATE);
  Log("Build", "Phocoena source commit=%s state=%s", PHOCOENA_SOURCE_COMMIT, PHOCOENA_SOURCE_STATE);
  Log("Build", "%s", DOLPHIN_TOOLCHAIN_INFO);
  Log("Build", "source.lock.json SHA-256 %s", DOLPHIN_PATCHSET_SHA256);
  Log("Build", "compiler %s; toolchain image %s", __VERSION__, DOLPHIN_TOOLCHAIN_DIGEST);
  Log("Logging", "low-overhead mode: Horizon INFO buffered; upstream core level=WARNING; "
      "periodic reports flush every ~5 s; critical runtime tails flush immediately");
  const u32 version = hosversionGet();
  Log("Horizon", "%u.%u.%u; Atmosphere=%s; applet type=%d", HOSVER_MAJOR(version),
      HOSVER_MINOR(version), HOSVER_MICRO(version), hosversionIsAtmosphere() ? "yes" : "not detected",
      static_cast<int>(appletGetAppletType()));
  u64 cores = 0;
  const Result core_result = svcGetInfo(&cores, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0);
  Log("Thread", "process permitted_core_mask=0x%llx query=0x%08x; libnx pthreads use permitted cores; no clock/affinity override",
      static_cast<unsigned long long>(cores), core_result);
  Log("Thread", "loader-provided main stack=%zu bytes", threadGetSelf()->stack_sz);
  u64 total = 0, used = 0;
  const Result a = svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
  const Result b = svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
  if (R_SUCCEEDED(a) && R_SUCCEEDED(b))
    Log("Memory", "application total=%llu kernel-used (includes heap reservation)=%llu unreserved=%llu bytes",
        (unsigned long long)total, (unsigned long long)used,
        (unsigned long long)(total > used ? total - used : 0));
  else
    Error("Memory", "svcGetInfo failed: total=0x%08x used=0x%08x", a, b);
  const auto heap = mallinfo();
  const auto current_break = reinterpret_cast<uintptr_t>(sbrk(0));
  const auto end = reinterpret_cast<uintptr_t>(fake_heap_end);
  const size_t unused = end > current_break ? end - current_break : 0;
  Log("Memory", "newlib allocated=%zu free-chunks=%zu unused-heap=%zu approximate-available=%zu bytes",
      heap.uordblks, heap.fordblks, unused, heap.fordblks + unused);
}
}

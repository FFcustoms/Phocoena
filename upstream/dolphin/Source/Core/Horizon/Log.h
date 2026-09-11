// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <string>
#include <string_view>
namespace Horizon
{
inline constexpr const char* ROOT = "sdmc:/switch/Phocoena/";
extern std::atomic<bool> stop_requested;
bool OpenLog();
void CloseLog();
void FlushLog();
// Lines contain "subsystem: message"; timestamped and flushed once as a group.
void LogReport(std::string_view report);
[[gnu::format(printf, 2, 3)]] void Log(const char* subsystem, const char* format, ...);
[[gnu::format(printf, 2, 3)]] void Error(const char* subsystem, const char* format, ...);
std::string LastError();
void ClearLastError();
void AttachCoreLogger();
void LogEnvironment();
}

// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdio>
#include <cstdint>
#include <string_view>

namespace Horizon
{
// Caller owns the log lock. Only periodic INFO diagnostics are batched; error
// messages still go through the immediate, individually flushed logger.
// Returns the flush result so tests can use a real FILE without libnx mocks.
inline bool WriteInfoReport(FILE* file, std::uint64_t ms, std::string_view report)
{
  while (!report.empty())
  {
    const auto end = report.find('\n');
    const auto line = report.substr(0, end);
    if (std::fprintf(file, "[%llu ms] INFO: ", static_cast<unsigned long long>(ms)) < 0 ||
        std::fwrite(line.data(), 1, line.size(), file) != line.size() || std::fputc('\n', file) == EOF)
      return false;
    if (end == std::string_view::npos)
      break;
    report.remove_prefix(end + 1);
  }
  return std::fflush(file) == 0;
}
}

// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <chrono>
#include <ctime>

// Newlib has no timegm. Do not temporarily change TZ: core/audio threads may
// concurrently call localtime. Calendar arithmetic converts UTC directly.
inline std::time_t HorizonTimegm(std::tm* value)
{
  using namespace std::chrono;
  const auto months_from_zero = static_cast<long long>(value->tm_year) * 12 + value->tm_mon;
  const auto whole_years = (months_from_zero - (months_from_zero < 0 ? 11 : 0)) / 12;
  const auto actual_year = whole_years + 1900;
  if (actual_year < -32767 || actual_year > 32767) return -1;
  const auto actual_month = static_cast<unsigned>(months_from_zero - whole_years * 12 + 1);
  const auto first = sys_days{year{static_cast<int>(actual_year)} / month{actual_month} / day{1}};
  const auto instant = first + days{static_cast<long long>(value->tm_mday) - 1} +
                      hours{value->tm_hour} + minutes{value->tm_min} + seconds{value->tm_sec};
  const auto result = static_cast<std::time_t>(duration_cast<seconds>(instant.time_since_epoch()).count());
  std::tm normalized{};
  if (gmtime_r(&result, &normalized)) *value = normalized;
  return result;
}

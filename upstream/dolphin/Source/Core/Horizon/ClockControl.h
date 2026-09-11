// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "Horizon/ClockPolicy.h"

namespace Horizon
{
class AutoClockSession final
{
public:
  AutoClockSession() = default;
  ~AutoClockSession();
  AutoClockSession(const AutoClockSession&) = delete;
  AutoClockSession& operator=(const AutoClockSession&) = delete;

  const PhocoenaClock::StartReport& Start(bool enabled, PhocoenaClock::Profile profile);
  void Stop();

private:
  PhocoenaClock::Session m_session;
};
}

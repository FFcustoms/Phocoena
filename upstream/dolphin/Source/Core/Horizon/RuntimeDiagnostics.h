// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string_view>

#include "Common/CommonTypes.h"

namespace Core
{
class System;
}

namespace Horizon
{
// Keep a short, fixed-size FIFO/CP history in RAM. Steady-state sampling never
// writes to the SD card; a failure event dumps the recent history at once.
struct RuntimeFailureStats
{
  u64 gpu_wait_timeouts = 0;
  u64 pi_fifo_resets = 0;
  u64 pi_reset_codes = 0;
  u32 last_pi_reset_code = 0;
};

void ResetRuntimeDiagnostics();
void CaptureRuntimeSample(Core::System& system);
RuntimeFailureStats ReadRuntimeFailureStats();

// These hooks are intentionally narrow: they preserve evidence around the
// guest's own GPU watchdog/reset path without turning the general Dolphin log
// back into high-frequency SD traffic.
void ReportGuestGpuWaitTimeout(Core::System& system, std::string_view message);
void ReportPiFifoReset(Core::System& system, u32 value);
void ReportPiResetCode(Core::System& system, u32 value);
}  // namespace Horizon

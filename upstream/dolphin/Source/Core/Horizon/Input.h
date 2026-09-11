// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <switch.h>
namespace Horizon
{
void InstallController();
void UpdateController(const PadState& pad, u64 suppressed_buttons = 0,
                      bool suppress_sticks = false);
bool WriteDefaultMapping();
}

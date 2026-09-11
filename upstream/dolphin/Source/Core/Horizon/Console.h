// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace Horizon
{
// Main-thread only. A failed handoff throws before any further console drawing.
void InitializeConsole();
void SuspendConsole();
void ResumeConsole();
void ShutdownConsole();
}

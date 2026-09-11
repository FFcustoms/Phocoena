// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>

namespace Horizon
{
// Log a bounded number of buffer operations after each renderer change.
void BeginDisplayTrace();
// Previous renderer must be stopped and all its buffers released. Also called
// between Dolphin's temporary GL capability probe and the emulation GL context.
bool PrepareDisplayWindow(const char* owner);
void RecordGLSwap();
std::uint64_t GetGLSwapCount();
}

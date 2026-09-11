// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
namespace Horizon
{
// Actual upstream emitter on the production CodeMemory allocator. This tests
// generated ARM64 functions, not the complete PowerPC recompiler or game speed.
bool RunJitEmitterSelfTest();
}

// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

class CPUCoreBase;
namespace Core { class System; }
namespace PowerPC { enum class CPUCore; }

namespace Horizon
{
// Called after the ordinary interpreter has been initialized. Never falls back
// to DefaultCPUCore(). Native JIT requires an explicit request and emitter test;
// failures go through Cached Interpreter, then the ordinary interpreter.
CPUCoreBase* InitializeCPUCore(Core::System& system, PowerPC::CPUCore requested);
}

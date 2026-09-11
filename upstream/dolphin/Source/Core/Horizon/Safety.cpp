// SPDX-License-Identifier: GPL-2.0-or-later
#include "Horizon/Safety.h"
#include "Horizon/Log.h"
#include "Horizon/Renderer.h"
#include "Horizon/Fastmem.h"
#include "Common/Config/Config.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/PowerPC/PowerPC.h"
#include "VideoCommon/VideoConfig.h"
namespace Horizon
{
void EnforceSafetyConfig()
{
  // Apply AFTER all game INIs, BEFORE System::Initialize snapshots the settings.
  // Use only the frontend's base choice, not a per-game CPU override. Native
  // JIT is allowed through the explicit opt-in and runtime emitter self-test.
  const auto requested = Config::GetBase(Config::MAIN_CPU_CORE);
  const bool cached = requested == PowerPC::CPUCore::CachedInterpreter;
  const bool native = requested == PowerPC::CPUCore::JITARM64;
  Config::SetCurrent(Config::MAIN_CPU_CORE,
                     native ? PowerPC::CPUCore::JITARM64 :
                     cached ? PowerPC::CPUCore::CachedInterpreter : PowerPC::CPUCore::Interpreter);
  // The tested single-core mode remains the default. Dual-core is an explicit
  // frontend choice; a per-game INI must not silently enable the trial.
  Config::SetCurrent(Config::MAIN_CPU_THREAD, Config::GetBase(Config::MAIN_CPU_THREAD));
  const bool fastmem = native && IsPpcFastmemRequested();
  Config::SetCurrent(Config::MAIN_FASTMEM, fastmem);
  Config::SetCurrent(Config::MAIN_PAGE_TABLE_FASTMEM, fastmem);
  Config::SetCurrent(Config::MAIN_FASTMEM_ARENA, fastmem);
  Config::SetCurrent(Config::MAIN_LARGE_ENTRY_POINTS_MAP, false);
  Config::SetCurrent(Config::MAIN_DSP_HLE, true);
  Config::SetCurrent(Config::MAIN_DSP_JIT, false);
  Config::SetCurrent(Config::MAIN_SKIP_IPL, true);
  const auto vertex_loader = Config::GetBase(Config::GFX_VERTEX_LOADER_TYPE);
  Config::SetCurrent(Config::GFX_VERTEX_LOADER_TYPE,
                     vertex_loader == VertexLoaderType::Native ? VertexLoaderType::Native :
                                                                 VertexLoaderType::Software);
  Config::SetCurrent(Config::GFX_SHADER_COMPILER_THREADS, 0);
  Config::SetCurrent(Config::GFX_SHADER_PRECOMPILER_THREADS, 0);
  Config::SetCurrent(Config::GFX_SHADER_COMPILATION_MODE, ShaderCompilationMode::Synchronous);
  Config::SetCurrent(Config::GFX_EFB_SCALE, 1);
  Config::SetCurrent(Config::MAIN_GFX_BACKEND, std::string(RENDERER_ID));
#ifdef HORIZON_VULKAN
  // BindBackbuffer drains the submission worker BEFORE acquire/resize, so the
  // Switch NWindow still has one owner at a time. Keep the frontend's choice
  // after game INIs; CPU dual-core and shader compilation are separate settings.
  Config::SetCurrent(Config::GFX_BACKEND_MULTITHREADING,
                     Config::GetBase(Config::GFX_BACKEND_MULTITHREADING));
  Config::SetCurrent(Config::GFX_ENHANCE_HDR_OUTPUT, false);
  // AudioOut is independent of the Vulkan/NWindow path. Preserve the frontend
  // choice after per-game INIs instead of silently forcing NullSound.
  Config::SetCurrent(Config::MAIN_AUDIO_BACKEND, Config::GetBase(Config::MAIN_AUDIO_BACKEND));
#endif
  Log("Boot safety", "configured CPU=%s vertex loader=%s horizon_ppc_fastmem=%s "
      "large entry map=off shared shader contexts=off; dual_core=%d (explicit frontend choice) "
      "audio_backend=%s",
      native ? "AArch64 JIT (self-test required)" : cached ? "Cached Interpreter" : "Interpreter",
      vertex_loader == VertexLoaderType::Native ? "AArch64 JIT" : "Software",
      fastmem ? "requested (runtime-probed)" : "off",
      Config::Get(Config::MAIN_CPU_THREAD), Config::Get(Config::MAIN_AUDIO_BACKEND).c_str());
}
}

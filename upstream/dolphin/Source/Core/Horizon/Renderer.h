// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace Horizon
{
#ifdef HORIZON_VULKAN
inline constexpr const char* RENDERER_ID = "Vulkan";
inline constexpr const char* RENDERER_NAME = "Vulkan / NXVK (experimental)";
inline constexpr const char* RENDERER_SETTINGS = "Settings | Vulkan / NXVK, native 1x";
#else
inline constexpr const char* RENDERER_ID = "OGL";
inline constexpr const char* RENDERER_NAME = "OpenGL / Mesa Nouveau";
inline constexpr const char* RENDERER_SETTINGS = "Settings | OpenGL, native 1x";
#endif
inline constexpr bool AUDIO_AVAILABLE = true;
}

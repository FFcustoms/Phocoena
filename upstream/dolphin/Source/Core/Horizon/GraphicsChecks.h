// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstdint>

namespace Horizon
{
struct SurfaceSize
{
  int width = 0;
  int height = 0;
  bool Valid() const { return width > 0 && height > 0; }
};

inline SurfaceSize ResolveSurfaceSize(bool egl_query_ok, SurfaceSize egl, SurfaceSize native)
{
  // The pinned Switch Mesa driver leaves _EGLSurface.Width/Height at zero,
  // although it allocates buffers using nwindowGetDimensions. Reject failed
  // queries and partial/zero/negative extents; fall back to that native size.
  if (egl_query_ok && egl.Valid())
    return egl;
  return native.Valid() ? native : SurfaceSize{};
}

inline bool TrianglePixelsMatch(const std::array<std::uint8_t, 4>& center,
                                const std::array<std::uint8_t, 4>& corner)
{
  // Tolerate normal UNORM rounding, but not a clear-only, black or swizzled image.
  return center[0] >= 20 && center[0] <= 32 && center[1] >= 198 && center[1] <= 210 &&
         center[2] >= 121 && center[2] <= 134 && center[3] >= 250 &&
         corner[0] <= 14 && corner[1] >= 9 && corner[1] <= 22 &&
         corner[2] >= 19 && corner[2] <= 33 && corner[3] >= 250;
}
}

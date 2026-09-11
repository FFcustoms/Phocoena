// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <EGL/egl.h>
#include "Common/GL/GLContext.h"
namespace Horizon
{
class GLContextSwitch final : public GLContext
{
public:
  ~GLContextSwitch() override;
  bool IsHeadless() const override { return false; }
  bool MakeCurrent() override;
  bool ClearCurrent() override;
  void Swap() override;
  void SwapInterval(int interval) override;
  void* GetFuncAddress(const std::string& name) override;
  std::unique_ptr<GLContext> CreateSharedContext() override;
protected:
  bool Initialize(const WindowSystemInfo&, bool stereo, bool core) override;
private:
  EGLDisplay m_display = EGL_NO_DISPLAY;
  EGLSurface m_surface = EGL_NO_SURFACE;
  EGLContext m_context = EGL_NO_CONTEXT;
  bool m_initialized = false;
  std::uint64_t m_swaps = 0;
  std::uint64_t m_owner_thread = 0;
};
}

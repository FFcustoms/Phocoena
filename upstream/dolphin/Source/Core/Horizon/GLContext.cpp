// SPDX-License-Identifier: GPL-2.0-or-later
#include "Horizon/GLContext.h"
#include <EGL/eglext.h>
#include <switch.h>
#include "Horizon/Display.h"
#include "Horizon/GraphicsChecks.h"
#include "Horizon/Log.h"
#include "Horizon/Performance.h"
namespace Horizon
{
bool GLContextSwitch::Initialize(const WindowSystemInfo&, bool stereo, bool)
{
  if (stereo)
  {
    Error("OpenGL", "stereo window surfaces are unsupported");
    return false;
  }
  // InitBackendInfo destroys a temporary GL context before the real backend
  // starts. No console handoff runs between those two contexts.
  if (!PrepareDisplayWindow("OpenGL context"))
    return false;
  // The VI compositor scales this presentation surface in docked mode too.
  // This is independent of Dolphin's 1x internal GameCube resolution.
  const Result dimensions = nwindowSetDimensions(nwindowGetDefault(), 1280, 720);
  if (R_FAILED(dimensions))
  {
    Error("OpenGL", "native window dimensions failed: 0x%08x", dimensions);
    return false;
  }
  m_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  EGLint major = 0, minor = 0;
  if (m_display == EGL_NO_DISPLAY || !eglInitialize(m_display, &major, &minor))
  {
    Error("OpenGL", "EGL display initialization failed: 0x%x", eglGetError());
    return false;
  }
  m_initialized = true;
  if (!eglBindAPI(EGL_OPENGL_API))
  {
    Error("OpenGL", "eglBindAPI failed: 0x%x", eglGetError());
    return false;
  }
  Log("OpenGL", "EGL %d.%d vendor=%s", major, minor, eglQueryString(m_display, EGL_VENDOR));
  const EGLint attrs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                         EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
                         EGL_ALPHA_SIZE, 8, EGL_NONE};
  EGLConfig config{};
  EGLint count = 0;
  if (!eglChooseConfig(m_display, attrs, &config, 1, &count) || !count)
  {
    Error("OpenGL", "no window config: 0x%x", eglGetError());
    return false;
  }
  m_surface = eglCreateWindowSurface(m_display, config, nwindowGetDefault(), nullptr);
  if (m_surface == EGL_NO_SURFACE)
  {
    Error("OpenGL", "eglCreateWindowSurface failed: 0x%x", eglGetError());
    return false;
  }
  for (const auto& [maj, min] : s_desktop_opengl_versions)
  {
    const EGLint context_attrs[] = {EGL_CONTEXT_MAJOR_VERSION_KHR, maj,
        EGL_CONTEXT_MINOR_VERSION_KHR, min, EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR,
        EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR, EGL_NONE};
    m_context = eglCreateContext(m_display, config, EGL_NO_CONTEXT, context_attrs);
    if (m_context != EGL_NO_CONTEXT)
    {
      Log("OpenGL", "created core profile %d.%d (Mesa/Nouveau)", maj, min);
      break;
    }
    Log("OpenGL", "core profile %d.%d unavailable: EGL 0x%x", maj, min, eglGetError());
  }
  if (m_context == EGL_NO_CONTEXT || !MakeCurrent())
  {
    Error("OpenGL", "context creation/current failed: 0x%x", eglGetError());
    return false;
  }
  EGLint width = 0, height = 0;
  const bool width_ok = eglQuerySurface(m_display, m_surface, EGL_WIDTH, &width);
  const EGLint width_error = width_ok ? EGL_SUCCESS : eglGetError();
  const bool height_ok = eglQuerySurface(m_display, m_surface, EGL_HEIGHT, &height);
  const EGLint height_error = height_ok ? EGL_SUCCESS : eglGetError();
  u32 native_width = 0, native_height = 0;
  const Result native_result = nwindowGetDimensions(nwindowGetDefault(), &native_width, &native_height);
  const auto size = ResolveSurfaceSize(width_ok && height_ok, {width, height},
      R_SUCCEEDED(native_result) ? SurfaceSize{static_cast<int>(native_width), static_cast<int>(native_height)} : SurfaceSize{});
  Log("OpenGL", "EGL queried size=%dx%d errors=0x%x/0x%x; native=%ux%u result=0x%08x",
      width, height, width_error, height_error, native_width, native_height, native_result);
  if (!size.Valid())
  {
    Error("OpenGL", "no valid surface dimensions; refusing a zero-size viewport");
    return false;
  }
  m_backbuffer_width = size.width;
  m_backbuffer_height = size.height;
  m_opengl_mode = Mode::OpenGL;
  Log("OpenGL", "surface %dx%d using %s dimensions; shared shader contexts disabled",
      size.width, size.height, width_ok && height_ok && SurfaceSize{width, height}.Valid() ? "EGL" : "native window");
  return true;
}
GLContextSwitch::~GLContextSwitch()
{
  if (m_initialized)
  {
    Log("OpenGL cleanup", "BEGIN clear current; successful swaps=%llu",
        static_cast<unsigned long long>(m_swaps));
    if (!ClearCurrent())
    {
      Error("OpenGL cleanup", "clear current failed: 0x%x", eglGetError());
      stop_requested = true;
    }
    if (m_context != EGL_NO_CONTEXT)
    {
      Log("OpenGL cleanup", "BEGIN destroy context");
      if (!eglDestroyContext(m_display, m_context))
      {
        Error("OpenGL cleanup", "destroy context failed: 0x%x", eglGetError());
        stop_requested = true;
      }
    }
    if (m_surface != EGL_NO_SURFACE)
    {
      Log("OpenGL cleanup", "BEGIN destroy surface / release native buffers");
      if (!eglDestroySurface(m_display, m_surface))
      {
        Error("OpenGL cleanup", "destroy surface failed: 0x%x", eglGetError());
        stop_requested = true;
      }
    }
    Log("OpenGL cleanup", "BEGIN terminate EGL");
    if (!eglTerminate(m_display))
    {
      Error("OpenGL cleanup", "terminate failed: 0x%x", eglGetError());
      stop_requested = true;
    }
    Log("OpenGL cleanup", "COMPLETE");
  }
}
bool GLContextSwitch::MakeCurrent()
{
  if (eglMakeCurrent(m_display, m_surface, m_surface, m_context) != EGL_TRUE)
    return false;
  u64 id = 0;
  svcGetThreadId(&id, CUR_THREAD_HANDLE);
  m_owner_thread = id;
  Log("OpenGL ownership", "context current on thread=%llu", static_cast<unsigned long long>(id));
  return true;
}
bool GLContextSwitch::ClearCurrent()
{
  u64 id = 0;
  svcGetThreadId(&id, CUR_THREAD_HANDLE);
  if (m_owner_thread && id != m_owner_thread)
  {
    Error("OpenGL ownership", "clear from thread=%llu but owner=%llu",
        static_cast<unsigned long long>(id), static_cast<unsigned long long>(m_owner_thread));
    return false;
  }
  if (eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) != EGL_TRUE)
    return false;
  Log("OpenGL ownership", "context released on thread=%llu", static_cast<unsigned long long>(id));
  m_owner_thread = 0;
  return true;
}
void GLContextSwitch::Swap()
{
  const ScopedPerfSample perf(PerfMetric::EGLSwap);
  if (eglGetCurrentContext() != m_context)
  {
    Error("OpenGL ownership", "swap without this context current; stopping emulation");
    stop_requested = true;
    return;
  }
  if (!eglSwapBuffers(m_display, m_surface))
  {
    Error("OpenGL", "eglSwapBuffers failed: 0x%x", eglGetError());
    stop_requested = true;
  }
  else
  {
    ++m_swaps;
    RecordGLSwap();
    if (m_swaps <= 3)
      Log("OpenGL present", "eglSwapBuffers OK; context swaps=%llu native format=0x%x size=%ux%u",
          static_cast<unsigned long long>(m_swaps), nwindowGetDefault()->format,
          m_backbuffer_width, m_backbuffer_height);
  }
}
void GLContextSwitch::SwapInterval(int interval)
{
  if (!eglSwapInterval(m_display, interval))
    Error("OpenGL", "eglSwapInterval(%d) failed: 0x%x", interval, eglGetError());
}
void* GLContextSwitch::GetFuncAddress(const std::string& name)
{
  return reinterpret_cast<void*>(eglGetProcAddress(name.c_str()));
}
std::unique_ptr<GLContext> GLContextSwitch::CreateSharedContext() { return nullptr; }
}

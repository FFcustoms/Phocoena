// SPDX-License-Identifier: GPL-2.0-or-later
#include <switch.h>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <mutex>
#include <thread>
#include "Common/MemoryUtil.h"
#ifndef HORIZON_VULKAN
#include "Common/GL/GLContext.h"
#include "Common/GL/GLExtensions/GLExtensions.h"
#endif
#include "Horizon/Audio.h"
#include "Horizon/BuildInfo.h"
#include "Horizon/Console.h"
#include "Horizon/FrontendModel.h"
#include "Horizon/GraphicsChecks.h"
#include "Horizon/Input.h"
#include "Horizon/Log.h"
#include "Horizon/JitTest.h"
#include "Horizon/SupportDiagnostics.h"
namespace Horizon
{
namespace
{
bool SDTest()
{
  const std::string path = std::string(ROOT) + "logs/diagnostic-rw.tmp";
  if (std::filesystem::exists(path))
  {
    Error("Diagnostics SD", "refusing to overwrite existing %s; remove it then retry", path.c_str());
    return false;
  }
  const std::string expected = "Phocoena SD read/write test\n";
  std::ofstream out(path, std::ios::binary);
  out << expected;
  out.close();
  std::ifstream in(path, std::ios::binary);
  const std::string actual{std::istreambuf_iterator<char>(in), {}};
  in.close();
  std::error_code ec;
  std::filesystem::remove(path, ec);
  return out.good() && actual == expected && !ec;
}
bool MemoryTest()
{
  constexpr size_t size = 64 * 1024 * 1024;
  auto* ptr = static_cast<u8*>(Common::AllocateMemoryPages(size));
  if (!ptr) return false;
  for (size_t i = 0; i < size; i += 4096) ptr[i] = static_cast<u8>(i / 4096);
  bool good = true;
  for (size_t i = 0; i < size; i += 4096) good &= ptr[i] == static_cast<u8>(i / 4096);
  Common::FreeMemoryPages(ptr, size);
  return good;
}
bool JitTest()
{
  Jit jit{};
  Result rc = jitCreate(&jit, 4096);
  if (R_FAILED(rc)) { Error("Diagnostics JIT", "jitCreate: 0x%08x", rc); return false; }
  bool pass = true;
  for (u32 expected : {42u, 43u})
  {
    rc = jitTransitionToWritable(&jit);
    if (R_FAILED(rc)) { pass = false; break; }
    const std::array<u32, 2> code = {0x52800000u | (expected << 5), 0xd65f03c0u}; // mov w0,#n; ret
    std::memcpy(jitGetRwAddr(&jit), code.data(), sizeof(code));
    rc = jitTransitionToExecutable(&jit); // libnx cleans D-cache and invalidates I-cache
    if (R_FAILED(rc)) { pass = false; break; }
    const auto function = reinterpret_cast<u32 (*)()>(jitGetRxAddr(&jit));
    if (function() != expected) { pass = false; break; }
  }
  Log("Diagnostics JIT", "libnx type=%d rw=%p rx=%p result=0x%08x; this is not full PPC JIT validation",
      jit.type, jitGetRwAddr(&jit), jitGetRxAddr(&jit), rc);
  const Result closed = jitClose(&jit);
  return pass && R_SUCCEEDED(closed);
}
bool ThreadTest()
{
  std::mutex mutex;
  std::condition_variable cv;
  bool done = false;
  std::thread worker([&] {
    std::lock_guard lock(mutex);
    done = true;
    cv.notify_one();
  });
  std::unique_lock lock(mutex);
  const bool pass = cv.wait_for(lock, std::chrono::seconds(2), [&] { return done; });
  lock.unlock();
  worker.join();
  return pass;
}
#ifndef HORIZON_VULKAN
bool GLTest(PadState& pad)
{
  // The console and EGL cannot own the native window at the same time.
  SuspendConsole();
  stop_requested = false;
  bool success = false;
  {
    auto context = GLContext::Create(WindowSystemInfo{WindowSystemType::Horizon, nullptr, nwindowGetDefault(), nwindowGetDefault()});
    if (context && GLExtensions::Init(context.get()))
    {
      Log("Diagnostics GL", "vendor=%s renderer=%s version=%s", glGetString(GL_VENDOR),
          glGetString(GL_RENDERER), glGetString(GL_VERSION));
      const char* vs = "#version 330 core\nvoid main(){vec2 p[3]=vec2[3](vec2(0,.7),vec2(-.7,-.7),vec2(.7,-.7));gl_Position=vec4(p[gl_VertexID],0,1);}";
      const char* fs = "#version 330 core\nout vec4 c;void main(){c=vec4(.1,.8,.5,1);}";
      GLuint vertex = glCreateShader(GL_VERTEX_SHADER), fragment = glCreateShader(GL_FRAGMENT_SHADER);
      glShaderSource(vertex, 1, &vs, nullptr); glCompileShader(vertex);
      glShaderSource(fragment, 1, &fs, nullptr); glCompileShader(fragment);
      GLint a = 0, b = 0;
      glGetShaderiv(vertex, GL_COMPILE_STATUS, &a); glGetShaderiv(fragment, GL_COMPILE_STATUS, &b);
      GLuint program = glCreateProgram();
      glAttachShader(program, vertex); glAttachShader(program, fragment); glLinkProgram(program);
      GLint linked = 0; glGetProgramiv(program, GL_LINK_STATUS, &linked);
      success = a && b && linked;
      if (!success)
      {
        char buffer[2048]{};
        glGetShaderInfoLog(vertex, sizeof(buffer), nullptr, buffer); Error("GL vertex shader", "%s", buffer);
        glGetShaderInfoLog(fragment, sizeof(buffer), nullptr, buffer); Error("GL fragment shader", "%s", buffer);
        glGetProgramInfoLog(program, sizeof(buffer), nullptr, buffer); Error("GL link", "%s", buffer);
      }
      GLuint vao = 0; glGenVertexArrays(1, &vao); glBindVertexArray(vao);
      context->SwapInterval(1);
      const int width = static_cast<int>(context->GetBackBufferWidth());
      const int height = static_cast<int>(context->GetBackBufferHeight());
      success &= width > 0 && height > 0;
      glViewport(0, 0, width, height);
      const GLenum framebuffer = glCheckFramebufferStatus(GL_FRAMEBUFFER);
      success &= framebuffer == GL_FRAMEBUFFER_COMPLETE;
      Log("Diagnostics GL", "viewport=%dx%d framebuffer=0x%x shaders=%d/%d link=%d",
          width, height, framebuffer, a, b, linked);
      int frames = 0;
      for (; frames < 120 && success && !stop_requested && appletMainLoop(); ++frames)
      {
        if (frames == 0) Log("Diagnostics GL", "BEGIN first draw and GPU readback");
        glClearColor(.03f, .06f, .1f, 1.f); glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(program); glDrawArrays(GL_TRIANGLES, 0, 3);
        if (frames == 0 || frames == 119)
        {
          std::array<std::uint8_t, 4> center{}, corner{};
          glReadBuffer(GL_BACK);
          glReadPixels(width / 2, height / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, center.data());
          glReadPixels(1, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, corner.data());
          success &= TrianglePixelsMatch(center, corner);
          Log("Diagnostics GL", "frame=%d center=%u,%u,%u,%u corner=%u,%u,%u,%u readback=%s",
              frames, center[0], center[1], center[2], center[3],
              corner[0], corner[1], corner[2], corner[3], success ? "PASS" : "FAIL");
          if (!success) Error("Diagnostics GL", "triangle pixels do not match; refusing a blank-render PASS");
        }
        if (frames == 0) Log("Diagnostics GL", "BEGIN first swap");
        context->Swap();
        if (frames == 0) Log("Diagnostics GL", "first swap returned");
        const GLenum draw_error = glGetError();
        if (draw_error)
        {
          Error("Diagnostics GL", "frame=%d glGetError=0x%x", frames, draw_error);
          success = false;
        }
      }
      Log("Diagnostics GL", "frames=%d/120; BEGIN GPU finish and object cleanup", frames);
      glFinish();
      const GLenum error = glGetError();
      success &= error == GL_NO_ERROR;
      success &= frames == 120 && !stop_requested.load();
      if (error) Error("Diagnostics GL", "glGetError=0x%x", error);
      glDeleteVertexArrays(1, &vao); glDeleteProgram(program);
      glDeleteShader(vertex); glDeleteShader(fragment);
      Log("Diagnostics GL", "objects deleted; BEGIN EGL teardown");
    }
    else
    {
      Error("Diagnostics GL", "context creation or GL function loading failed");
    }
  }
  success &= !stop_requested.load();
  Log("Diagnostics GL", "EGL teardown returned; BEGIN console restore");
  ResumeConsole();
  stop_requested = false;
  if (!success) return false;

  // Pixel readback validates rendering, not what the VI compositor displayed.
  // Require the tester to confirm visibility before claiming a full PASS.
  Log("Diagnostics GL", "BEGIN visual confirmation menu draw");
  printf("\x1b[2J\x1b[H\n  Graphics test returned to the menu.\n\n"
         "  GPU readback passed. Did you see a GREEN TRIANGLE?\n\n"
         "  A: Yes, visible triangle    B: No / black screen\n");
  consoleUpdate(nullptr);
  Log("Diagnostics GL", "console draw/swap returned; waiting for visual confirmation");
  while (appletMainLoop())
  {
    padUpdate(&pad); UpdateController(pad);
    const u64 down = padGetButtonsDown(&pad);
    if (down & HidNpadButton_B)
    {
      Error("Diagnostics GL", "tester reports missing triangle despite GPU readback; presentation failed");
      return false;
    }
    if (down & HidNpadButton_A)
    {
      Log("Diagnostics GL", "tester confirmed visible triangle; console restored");
      return true;
    }
    consoleUpdate(nullptr);
  }
  Error("Diagnostics GL", "exited before visual confirmation");
  return false;
}
#endif
}
void RunDiagnostics(PadState& pad)
{
  std::array<std::string, 6> results;
  results.fill("Not run");
  const std::array<const char*, 6> labels = {"SD read/write", "64 MiB allocation", "libnx JIT + cache", "std::thread / condition variable", "libnx audout initialization",
#ifdef HORIZON_VULKAN
    "Vulkan / shader / triangle"};
  results[4] = "Unavailable in this Vulkan build";
  results[5] = "Use separate Vulkan Probe app";
#else
    "OpenGL / shader / triangle"};
#endif
  std::string bundle_status = "Not generated";
  auto report = [&](size_t i, bool pass) {
    results[i] = pass ? "PASS" : "FAIL (see log)";
    if (pass) Log("Diagnostics", "%s PASS", labels[i]);
    else Error("Diagnostics", "%s FAIL", labels[i]);
  };
  auto run = [&](size_t i, auto test) {
    Log("Diagnostics", "BEGIN %s", labels[i]);
    report(i, test());
  };
  while (appletMainLoop())
  {
    padUpdate(&pad); UpdateController(pad);
    const u64 down = padGetButtonsDown(&pad);
    if (down & HidNpadButton_B) return;
    if (down & HidNpadButton_A)
    {
      run(0, SDTest); run(1, MemoryTest); run(3, ThreadTest);
      run(4, [] { Audio audio; return audio.Init(); });
    }
    if (down & HidNpadButton_Y) run(2, [] { return JitTest() && RunJitEmitterSelfTest(); });
#ifdef HORIZON_VULKAN
    if (down & HidNpadButton_X)
    {
      std::string bundle_path, bundle_error;
      if (GenerateSupportBundle(&bundle_path, &bundle_error))
        bundle_status = "Created: " + std::filesystem::path(bundle_path).filename().string();
      else if (bundle_error == "no new game sessions to bundle")
        bundle_status = "No new sessions to bundle";
      else
        bundle_status = "Bundle failed: " + bundle_error;
    }
#else
    if (down & HidNpadButton_X) run(5, [&] { return GLTest(pad); });
#endif
    printf("\x1b[2J\x1b[H\n  Phocoena v%s - About / Diagnostics\n\n  Phocoena source: %.12s (%s)\n  Upstream: %.40s\n  Built: %s\n\n",
           DOLPHIN_PORT_VERSION, PHOCOENA_SOURCE_COMMIT, PHOCOENA_SOURCE_STATE,
           DOLPHIN_UPSTREAM_COMMIT, DOLPHIN_BUILD_DATE);
    for (size_t i = 0; i < results.size(); ++i) printf("  %-34s %s\n", labels[i], results[i].c_str());
    printf("\n  New test sessions: %zu\n  Support bundle: %s\n", PendingSupportSessionCount(), bundle_status.c_str());
    const auto left = padGetStickPos(&pad, 0), right = padGetStickPos(&pad, 1);
    printf("\n  Controller connected: %d  buttons: %016llx\n  Left: %6d %6d  Right: %6d %6d\n", padIsConnected(&pad), (unsigned long long)padGetButtons(&pad), left.x, left.y, right.x, right.y);
#ifdef HORIZON_VULKAN
    printf("\n  A: SD / memory / threads / audio  X: Generate support bundle  Y: JIT  B: Back\n");
#else
    printf("\n  A: SD / memory / threads / audio  X: GL  Y: JIT  B: Back\n  GL: green triangle for 2 seconds, then A=visible / B=black.\n");
#endif
    printf("  JIT: libnx plus upstream emitter; not full PPC validation.\n"
           "  Support bundles: /switch/Phocoena/diagnostics/bundles/\n");
    consoleUpdate(nullptr);
  }
}
}

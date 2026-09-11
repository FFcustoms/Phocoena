// SPDX-License-Identifier: GPL-2.0-or-later
#include "Horizon/Console.h"
#include "Horizon/Display.h"
#include "Horizon/Log.h"
#include <switch.h>
#include <sys/iosupport.h>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

extern "C"
{
Result __wrap_framebufferCreate(Framebuffer*, NWindow*, u32, u32, u32, u32);
Result __wrap_nwindowDequeueBuffer(NWindow*, s32*, NvMultiFence*);
Result __wrap_nwindowQueueBuffer(NWindow*, s32, const NvMultiFence*);
Result __wrap_nwindowReleaseBuffers(NWindow*);
Result __wrap_bqDequeueBuffer(Binder*, bool, u32, u32, s32, u32, s32*, NvMultiFence*);
Result __wrap_bqRequestBuffer(Binder*, s32, BqGraphicBuffer*);
}

namespace
{
NWindow window;
PrintConsole console;
Service relay;
std::vector<std::string> events;
size_t captured_bytes = 0;
bool fail_init = false, fail_interval = false, first_console_init = true;
bool binder_alive = true;
Framebuffer framebuffer;
unsigned created_buffers = 0, next_slot = 0, swapped_frames = 0, trace_lines = 0;
Result dequeue_error = 0, queue_error = 0, binder_error = 0;
bool return_unregistered_slot = false;
std::vector<std::string> errors;

ssize_t ConsoleWrite(_reent*, void*, const char*, size_t length)
{
  assert(console.consoleInitialised); // Never draw through a closed framebuffer.
  return static_cast<ssize_t>(length);
}
const devoptab_t console_device{"console", ConsoleWrite};

void ReleaseBuffers()
{
  window.slots_configured = 0;
  window.slots_requested = 0;
  window.cur_slot = -1;
  window.is_connected = false;
  window.width = window.height = window.format = 0; // libnx 4.12.0 behavior
}
template <typename F> void ExpectFailure(F function)
{
  bool caught = false;
  try { function(); }
  catch (const std::runtime_error&) { caught = true; }
  assert(caught);
}
}

const devoptab_t* devoptab_list[3]{};
NWindow* nwindowGetDefault() { return &window; }
bool nwindowIsValid(NWindow* win) { return win->valid; }
Service* viGetSession_IHOSBinderDriverRelay() { return &relay; }
void nwindowClose(NWindow* win)
{
  assert(!win->slots_configured && win->cur_slot < 0);
  events.emplace_back("window-close");
  win->valid = false;
  binder_alive = false;
}
Result nwindowCreate(NWindow* win, Service* session, s32 id, bool controlled)
{
  assert(!win->valid && session == &relay && id == 0 && controlled);
  events.emplace_back("window-create");
  // Hardware regression: after close, recreating this producer returned BadInput.
  // Model a server that does not accept reuse of that released Binder handle.
  if (!binder_alive) return 0x1759;
  *win = {};
  win->valid = win->is_connected = true;
  win->bq.id = id;
  win->producer_controlled_by_app = controlled;
  return 0;
}
void mutexLock(Mutex* mutex) { assert(!mutex->locked); mutex->locked = true; }
void mutexUnlock(Mutex* mutex) { assert(mutex->locked); mutex->locked = false; }
Result nwindowSetSwapInterval(NWindow* win, u32 interval)
{
  assert(win->valid && !win->mutex.locked);
  if (fail_interval) return 0x1759;
  win->swap_interval = interval;
  return 0;
}
Result nwindowCancelBuffer(NWindow* win, s32 slot, const NvMultiFence*)
{
  assert(win->cur_slot == slot);
  win->cur_slot = -1;
  return 0;
}

extern "C"
{
Result __real_framebufferCreate(Framebuffer*, NWindow* win, u32 width, u32 height,
                                u32 format, u32 count)
{
  created_buffers = count;
  if (fail_init) return 0x1759;
  win->is_connected = true;
  win->width = width; win->height = height; win->format = format;
  win->slots_configured = (1ULL << count) - 1;
  return 0;
}
Result __real_nwindowDequeueBuffer(NWindow* win, s32* slot, NvMultiFence*)
{
  assert(win->cur_slot == -1);
  if (dequeue_error) return dequeue_error;
  // Deliberately exercise every slot, not only the first successful frame.
  // This models a three-slot producer; it is not a reproduction of HOS internals.
  *slot = return_unregistered_slot ? 3 : static_cast<s32>(next_slot++ % 3);
  win->cur_slot = *slot;
  win->slots_requested |= 1ULL << *slot;
  return 0;
}
Result __real_nwindowQueueBuffer(NWindow* win, s32 slot, const NvMultiFence*)
{
  assert(win->cur_slot == slot && (win->slots_configured & (1ULL << slot)));
  if (queue_error) return queue_error;
  win->cur_slot = -1;
  ++swapped_frames;
  return 0;
}
Result __real_nwindowReleaseBuffers(NWindow* win)
{
  assert(win == &window);
  ReleaseBuffers();
  return 0;
}
Result __real_bqDequeueBuffer(Binder*, bool, u32, u32, s32, u32, s32*, NvMultiFence*)
{ return binder_error; }
Result __real_bqRequestBuffer(Binder*, s32, BqGraphicBuffer*) { return binder_error; }
}

void consoleUpdate(PrintConsole*)
{
  assert(console.consoleInitialised);
  s32 slot = -1;
  assert(!R_FAILED(__wrap_nwindowDequeueBuffer(&window, &slot, nullptr)));
  assert(!R_FAILED(__wrap_nwindowQueueBuffer(&window, slot, nullptr)));
}
PrintConsole* consoleInit(PrintConsole*)
{
  assert(window.valid && window.slots_configured == 0 && window.format == ~0u);
  events.emplace_back("console-init");
  if (first_console_init)
  {
    devoptab_list[STD_OUT] = &console_device;
    first_console_init = false;
  }
  console.consoleInitialised = !R_FAILED(__wrap_framebufferCreate(
      &framebuffer, &window, 1280, 720, PIXEL_FORMAT_RGB_565, 2));
  if (console.consoleInitialised) assert(created_buffers == 3);
  return &console; // The real libnx also returns non-null when init fails.
}
void consoleExit(PrintConsole*)
{
  assert(console.consoleInitialised);
  assert(devoptab_list[STD_OUT] != &console_device);
  events.emplace_back("console-exit");
  console.consoleInitialised = false;
  assert(!R_FAILED(__wrap_nwindowReleaseBuffers(&window)));
}
namespace Horizon
{
void Log(const char* subsystem, const char* format, ...)
{
  if (std::string(subsystem) == "Display buffers" &&
      (std::string(format).find("%s result=") == 0)) ++trace_lines;
  if (std::string(subsystem) != "Driver/stdio") return;
  char data[2048];
  va_list args; va_start(args, format);
  captured_bytes += static_cast<size_t>(vsnprintf(data, sizeof(data), format, args));
  va_end(args);
}
void Error(const char*, const char* format, ...)
{
  char data[2048];
  va_list args; va_start(args, format);
  vsnprintf(data, sizeof(data), format, args);
  va_end(args);
  errors.emplace_back(data);
}
}

int main()
{
  using namespace Horizon;
  window.valid = window.is_connected = window.producer_controlled_by_app = true;
  window.bq.id = 0; // Hardware's valid Binder id is zero.
  InitializeConsole();
  assert(devoptab_list[STD_OUT] == &console_device);
  for (int cycle = 0; cycle < 3; ++cycle)
  {
    events.clear();
    SuspendConsole();
    assert((events == std::vector<std::string>{"console-exit"}));
    assert(window.format == ~0u && window.valid && binder_alive && window.bq.id == 0);
    const std::string driver_message(2050, 'x');
    const size_t before = captured_bytes;
    assert(devoptab_list[STD_OUT]->write_r(nullptr, nullptr, driver_message.data(), driver_message.size()) == 2050);
    assert(captured_bytes - before == driver_message.size());
    ExpectFailure(SuspendConsole);
    // Simulate an EGL owner. It must not be stolen by the console.
    window.slots_configured = 7; window.format = 1;
    window.swap_interval = 0;
    events.clear();
    ExpectFailure(ResumeConsole);
    assert(events.empty() && window.slots_configured == 7 && window.format == 1 && !window.mutex.locked);
    ReleaseBuffers();
    // Real core boot creates a temporary capability-probe context, releases
    // it (format becomes zero), then creates emulation GL without a console.
    // Model libnx's rule: ConfigureBuffer adopts format only from the sentinel.
    for (unsigned context = 0; context < 2; ++context)
    {
      assert(window.format == 0);
      assert(PrepareDisplayWindow("OpenGL context"));
      assert(window.format == ~0U && binder_alive && !window.mutex.locked);
      if (window.format == ~0U) window.format = 1; // RGBA8 configured by Mesa
      window.slots_configured = 7;
      assert(!PrepareDisplayWindow("must not steal active GL"));
      assert(window.format == 1 && window.slots_configured == 7 && !window.mutex.locked);
      ReleaseBuffers();
    }
    const unsigned before_swaps = swapped_frames;
    ResumeConsole();
    assert((events == std::vector<std::string>{"console-init"}));
    assert(window.valid && binder_alive && window.bq.id == 0 && window.swap_interval == 1);
    assert(swapped_frames - before_swaps == 6 && window.slots_requested == 7);
    assert(console.consoleInitialised && devoptab_list[STD_OUT] == &console_device);
    ExpectFailure(ResumeConsole);
  }
  // Trace only the first eight dequeue/queue pairs, but always keep errors.
  const auto swaps_before = GetGLSwapCount();
  RecordGLSwap(); RecordGLSwap();
  assert(GetGLSwapCount() == swaps_before + 2);
  BeginDisplayTrace();
  trace_lines = 0;
  for (unsigned frame = 0; frame < 24; ++frame) consoleUpdate(nullptr);
  assert(trace_lines == 16);
  errors.clear();
  s32 slot = -10;
  dequeue_error = 0x895d;
  assert(__wrap_nwindowDequeueBuffer(&window, &slot, nullptr) == dequeue_error);
  assert(slot == -10 && errors.back().find("0x0000895d") != std::string::npos);
  assert(trace_lines == 17); // error still logged after trace budget is exhausted
  dequeue_error = 0;
  return_unregistered_slot = true;
  assert(__wrap_nwindowDequeueBuffer(&window, &slot, nullptr) == 0x1759);
  assert(slot == -10 && window.cur_slot == -1); // cancelled, no invalid buffer address
  return_unregistered_slot = false;
  assert(__wrap_nwindowDequeueBuffer(&window, &slot, nullptr) == 0);
  queue_error = 0xa15d;
  assert(__wrap_nwindowQueueBuffer(&window, slot, nullptr) == queue_error);
  assert(window.cur_slot == slot && errors.back().find("0x0000a15d") != std::string::npos);
  queue_error = 0;
  assert(__wrap_nwindowQueueBuffer(&window, slot, nullptr) == 0);
  errors.clear();
  binder_error = MAKERESULT(Module_LibnxBinder, LibnxBinderError_WouldBlock);
  assert(__wrap_bqDequeueBuffer(&window.bq, true, 1280, 720, 4, 0, &slot, nullptr) == binder_error);
  assert(errors.empty()); // ordinary producer backpressure must not be an ERROR
  binder_error = 0x1759;
  assert(__wrap_bqDequeueBuffer(&window.bq, true, 1280, 720, 4, 0, &slot, nullptr) == binder_error);
  assert(errors.back().find("bqDequeueBuffer") != std::string::npos);
  assert(__wrap_bqRequestBuffer(&window.bq, 2, nullptr) == binder_error);
  assert(errors.back().find("bqRequestBuffer") != std::string::npos);
  // Other windows/formats/counts retain the original framebuffer allocation.
  NWindow other;
  assert(__wrap_framebufferCreate(&framebuffer, &other, 16, 16, 4, 2) == 0 && created_buffers == 2);
  assert(__wrap_framebufferCreate(&framebuffer, &other, 16, 16, 1, 1) == 0 && created_buffers == 1);
  SuspendConsole();
  fail_init = true;
  ExpectFailure(ResumeConsole);
  assert(!console.consoleInitialised && devoptab_list[STD_OUT] != &console_device);
  fail_init = false;
  ResumeConsole();
  SuspendConsole();
  fail_interval = true;
  ExpectFailure(ResumeConsole);
  assert(window.valid && binder_alive && devoptab_list[STD_OUT] != &console_device);
  fail_interval = false;
  window.cur_slot = 2;
  ExpectFailure(ResumeConsole); // a dequeued buffer must not be stolen either
  assert(window.cur_slot == 2 && !window.mutex.locked);
  window.cur_slot = -1;
  window.valid = false;
  ExpectFailure(ResumeConsole);
  ShutdownConsole();
  std::puts("PASS: console and GL-probe -> GL handoff, three-buffer cycling, bounded traces, raw errors and failure guards (fake libnx, not hardware)");
}

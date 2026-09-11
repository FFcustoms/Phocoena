// SPDX-License-Identifier: GPL-2.0-or-later
#include "Horizon/Display.h"

#include <atomic>
#include <switch.h>

#include "Horizon/Log.h"

namespace
{
std::atomic<unsigned> dequeues{0};
std::atomic<unsigned> queues{0};
std::atomic<std::uint64_t> gl_swaps{0};

void WindowState(const char* operation, NWindow* window, int slot, Result result)
{
  if (!window)
    return;
  Horizon::Log("Display buffers",
      "%s result=0x%08x slot=%d connected=%d configured=0x%llx requested=0x%llx "
      "current=%d size=%ux%u format=0x%x usage=0x%x interval=%u",
      operation, result, slot, window->is_connected,
      static_cast<unsigned long long>(window->slots_configured),
      static_cast<unsigned long long>(window->slots_requested), window->cur_slot,
      window->width, window->height, window->format, window->usage, window->swap_interval);
}

void BufferError(const char* operation, Result result)
{
  Horizon::Error("Display buffers", "%s failed: raw Result=0x%08x module=%u description=%u",
      operation, result, R_MODULE(result), R_DESCRIPTION(result));
}
}

namespace Horizon
{
bool PrepareDisplayWindow(const char* owner)
{
  NWindow* window = nwindowGetDefault();
  if (!nwindowIsValid(window))
  {
    Error("Display handoff", "invalid native window for %s", owner);
    return false;
  }

  mutexLock(&window->mutex);
  Log("Display handoff", "to=%s connected=%d slots=0x%llx current=%d size=%ux%u format=0x%x",
      owner, window->is_connected, static_cast<unsigned long long>(window->slots_configured),
      window->cur_slot, window->width, window->height, window->format);
  if (window->slots_configured || window->cur_slot >= 0)
  {
    mutexUnlock(&window->mutex);
    Error("Display handoff", "previous renderer still owns native buffers; refusing %s", owner);
    return false;
  }

  // libnx 4.12.0 release resets format to 0; ConfigureBuffer only adopts the
  // next buffer's format when it is ~0U. This applies to GL -> GL as well as
  // console -> GL. Preserve Binder/event: close/recreate failed on hardware.
  // No public format-reset API exists in this pinned libnx version.
  window->format = ~0U;
  mutexUnlock(&window->mutex);
  const Result result = nwindowSetSwapInterval(window, 1);
  if (R_FAILED(result))
  {
    Error("Display handoff", "reset swap interval for %s failed: 0x%08x", owner, result);
    return false;
  }
  Log("Display handoff", "reused native window for %s; Binder=%d preserved; format=0x%x interval=1",
      owner, window->bq.id, window->format);
  BeginDisplayTrace();
  return true;
}

void RecordGLSwap()
{
  gl_swaps.fetch_add(1, std::memory_order_relaxed);
}

std::uint64_t GetGLSwapCount()
{
  return gl_swaps.load(std::memory_order_relaxed);
}

void BeginDisplayTrace()
{
  dequeues.store(0, std::memory_order_relaxed);
  queues.store(0, std::memory_order_relaxed);
}
}

// Link adapters keep the pinned SDK unchanged. Verify their real callers in the
// final ELF; merely defining __wrap_* does not prove that a library call uses it.
extern "C"
{
Result __real_framebufferCreate(Framebuffer*, NWindow*, u32, u32, u32, u32);
Result __real_nwindowDequeueBuffer(NWindow*, s32*, NvMultiFence*);
Result __real_nwindowQueueBuffer(NWindow*, s32, const NvMultiFence*);
Result __real_nwindowReleaseBuffers(NWindow*);
Result __real_bqDequeueBuffer(Binder*, bool, u32, u32, s32, u32, s32*, NvMultiFence*);
Result __real_bqRequestBuffer(Binder*, s32, BqGraphicBuffer*);
Result __wrap_framebufferCreate(Framebuffer*, NWindow*, u32, u32, u32, u32);
Result __wrap_nwindowDequeueBuffer(NWindow*, s32*, NvMultiFence*);
Result __wrap_nwindowQueueBuffer(NWindow*, s32, const NvMultiFence*);
Result __wrap_nwindowReleaseBuffers(NWindow*);
Result __wrap_bqDequeueBuffer(Binder*, bool, u32, u32, s32, u32, s32*, NvMultiFence*);
Result __wrap_bqRequestBuffer(Binder*, s32, BqGraphicBuffer*);

Result __wrap_framebufferCreate(Framebuffer* fb, NWindow* window, u32 width, u32 height,
                               u32 format, u32 count)
{
  // libnx's console requests two RGB565 buffers, while the pinned Switch Mesa
  // driver registers three. Keep all three producer slots populated when the
  // text console returns. This is a candidate mitigation for the v0.1.2 dequeue
  // failure, not proof that a stale third slot caused that hardware failure.
  const bool console = format == PIXEL_FORMAT_RGB_565 && count == 2 &&
                       window == nwindowGetDefault();
  const u32 actual_count = console ? 3 : count;
  const Result result = __real_framebufferCreate(fb, window, width, height, format, actual_count);
  Horizon::Log("Display buffers", "framebufferCreate %ux%u format=0x%x requested=%u actual=%u result=0x%08x",
      width, height, format, count, actual_count, result);
  if (R_FAILED(result))
    BufferError("framebufferCreate", result);
  return result;
}

Result __wrap_nwindowDequeueBuffer(NWindow* window, s32* output, NvMultiFence* fence)
{
  s32 slot = -1;
  Result result = __real_nwindowDequeueBuffer(window, &slot, fence);
  // Neither libnx framebufferBegin nor the Mesa array lookup validates the slot.
  // Refuse an unregistered slot before either caller computes a buffer address.
  if (!R_FAILED(result) && (slot < 0 || slot >= 64 ||
      !(window->slots_configured & (1ULL << slot))))
  {
    Horizon::Error("Display buffers", "dequeue returned unregistered slot=%d configured=0x%llx",
        slot, static_cast<unsigned long long>(window->slots_configured));
    if (slot >= 0 && slot < 64)
    {
      const Result cancel = nwindowCancelBuffer(window, slot, fence);
      if (R_FAILED(cancel))
        BufferError("cancel unregistered slot", cancel);
    }
    result = MAKERESULT(Module_Libnx, LibnxError_BadInput);
  }
  if (R_FAILED(result))
    BufferError("nwindowDequeueBuffer", result);
  if (dequeues.fetch_add(1, std::memory_order_relaxed) < 8 || R_FAILED(result))
    WindowState("dequeue", window, slot, result);
  if (!R_FAILED(result) && output)
    *output = slot;
  return result;
}

Result __wrap_nwindowQueueBuffer(NWindow* window, s32 slot, const NvMultiFence* fence)
{
  const Result result = __real_nwindowQueueBuffer(window, slot, fence);
  if (R_FAILED(result))
    BufferError("nwindowQueueBuffer", result);
  if (queues.fetch_add(1, std::memory_order_relaxed) < 8 || R_FAILED(result))
    WindowState("queue", window, slot, result);
  return result;
}

Result __wrap_nwindowReleaseBuffers(NWindow* window)
{
  WindowState("release BEGIN", window, -1, 0);
  const Result result = __real_nwindowReleaseBuffers(window);
  if (R_FAILED(result))
    BufferError("nwindowReleaseBuffers", result);
  WindowState("release END", window, -1, result);
  return result;
}

Result __wrap_bqDequeueBuffer(Binder* binder, bool async, u32 width, u32 height, s32 format,
                            u32 usage, s32* slot, NvMultiFence* fence)
{
  const Result result = __real_bqDequeueBuffer(binder, async, width, height, format, usage, slot, fence);
  // WouldBlock is a normal part of libnx's event/wait loop, not a graphics error.
  if (R_FAILED(result) && R_VALUE(result) != MAKERESULT(Module_LibnxBinder, LibnxBinderError_WouldBlock))
    BufferError("bqDequeueBuffer", result);
  return result;
}

Result __wrap_bqRequestBuffer(Binder* binder, s32 slot, BqGraphicBuffer* buffer)
{
  const Result result = __real_bqRequestBuffer(binder, slot, buffer);
  if (R_FAILED(result))
  {
    BufferError("bqRequestBuffer", result);
    Horizon::Log("Display buffers", "request slot=%d Binder=%d", slot, binder->id);
  }
  return result;
}
}

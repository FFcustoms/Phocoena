// SPDX-License-Identifier: GPL-2.0-or-later
#include "Horizon/Audio.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <malloc.h>

#include "AudioCommon/Mixer.h"
#include "Horizon/Log.h"

namespace Horizon
{
namespace
{
constexpr u64 AUDIO_WAIT_NS = 25'000'000ULL;
constexpr u64 IDLE_SLEEP_NS = 2'000'000ULL;

std::atomic<std::uint64_t> s_release_events{0};
std::atomic<std::uint64_t> s_requeues{0};
std::atomic<std::uint64_t> s_wait_failures{0};
std::atomic<std::uint64_t> s_append_failures{0};
}

AudioDiagnosticsStats ReadAudioDiagnosticsStats()
{
  return {
      .release_events = s_release_events.load(std::memory_order_relaxed),
      .requeues = s_requeues.load(std::memory_order_relaxed),
      .wait_failures = s_wait_failures.load(std::memory_order_relaxed),
      .append_failures = s_append_failures.load(std::memory_order_relaxed),
  };
}

void ResetAudioDiagnosticsStats()
{
  s_release_events.store(0, std::memory_order_relaxed);
  s_requeues.store(0, std::memory_order_relaxed);
  s_wait_failures.store(0, std::memory_order_relaxed);
  s_append_failures.store(0, std::memory_order_relaxed);
}


Audio::~Audio()
{
  m_running.store(false, std::memory_order_release);
  m_stop.store(true, std::memory_order_release);
  if (m_thread.joinable())
    m_thread.join();

  StopDevice();

  for (void*& storage : m_storage)
  {
    std::free(storage);
    storage = nullptr;
  }

  if (m_initialized)
  {
    audoutExit();
    m_initialized = false;
  }

  Log("Audio", "shutdown summary: releases=%llu requeues=%llu wait_failures=%llu append_failures=%llu",
      static_cast<unsigned long long>(m_release_events.load(std::memory_order_relaxed)),
      static_cast<unsigned long long>(m_requeues.load(std::memory_order_relaxed)),
      static_cast<unsigned long long>(m_wait_failures.load(std::memory_order_relaxed)),
      static_cast<unsigned long long>(m_append_failures.load(std::memory_order_relaxed)));
}

bool Audio::Init()
{
  Log("Audio", "Horizon AudioOut Init BEGIN");
  ResetAudioDiagnosticsStats();
  const Result rc = audoutInitialize();
  if (R_FAILED(rc))
  {
    Error("Audio", "audoutInitialize failed: 0x%08x", static_cast<unsigned>(rc));
    return false;
  }
  m_initialized = true;

  const u32 sample_rate = audoutGetSampleRate();
  const u32 channels = audoutGetChannelCount();
  const PcmFormat format = audoutGetPcmFormat();
  Log("Audio", "audoutInitialize OK: rate=%u channels=%u format=%u state=%u", sample_rate,
      channels, static_cast<unsigned>(format), static_cast<unsigned>(audoutGetDeviceState()));
  if (sample_rate != 48000 || channels != 2 || format != PcmFormat_Int16)
  {
    Error("Audio", "unexpected audout format: rate=%u channels=%u format=%u", sample_rate,
          channels, static_cast<unsigned>(format));
    return false;
  }

  static_assert(BUFFER_BYTES % 0x1000 == 0);
  for (std::size_t i = 0; i < BUFFER_COUNT; ++i)
  {
    void* const storage = memalign(0x1000, BUFFER_BYTES);
    if (!storage)
    {
      Error("Audio", "failed allocating audio buffer %zu", i);
      return false;
    }
    std::memset(storage, 0, BUFFER_BYTES);
    m_storage[i] = storage;
    m_buffers[i] = {};
    m_buffers[i].buffer = storage;
    m_buffers[i].buffer_size = BUFFER_BYTES;
    m_buffers[i].data_size = BUFFER_BYTES;
    m_buffers[i].data_offset = 0;
  }

  Log("Audio", "buffers ready: stereo s16 %u Hz, %zu x %zu frames (%zu bytes each)",
      sample_rate, BUFFER_COUNT, FRAMES_PER_BUFFER, BUFFER_BYTES);
  m_thread = std::thread(&Audio::ThreadMain, this);
  Log("Audio", "Horizon AudioOut Init READY; worker created");
  return true;
}

bool Audio::SetRunning(bool running)
{
  if (!m_initialized)
  {
    Error("Audio", "SetRunning(%d) before audout initialization", running ? 1 : 0);
    return false;
  }

  const bool previous = m_running.exchange(running, std::memory_order_acq_rel);
  if (previous != running)
    Log("Audio", "SetRunning %d -> %d", previous ? 1 : 0, running ? 1 : 0);
  return true;
}

void Audio::SetVolume(int volume)
{
  if (!m_initialized)
    return;

  const float normalized = std::clamp(volume, 0, 100) / 100.0f;
  const Result rc = audoutSetAudioOutVolume(normalized);
  if (R_FAILED(rc))
    Error("Audio", "audoutSetAudioOutVolume failed: 0x%08x", static_cast<unsigned>(rc));
  else
    Log("Audio", "volume=%d%%", std::clamp(volume, 0, 100));
}

void Audio::FillBuffer(AudioOutBuffer* buffer)
{
  if (!buffer || !buffer->buffer)
    return;

  if (m_running.load(std::memory_order_acquire) && m_mixer)
    m_mixer->Mix(static_cast<s16*>(buffer->buffer), FRAMES_PER_BUFFER);
  else
    std::memset(buffer->buffer, 0, BUFFER_BYTES);

  buffer->data_size = BUFFER_BYTES;
  buffer->data_offset = 0;
  armDCacheFlush(buffer->buffer, BUFFER_BYTES);
}

bool Audio::RequeueBuffer(AudioOutBuffer* buffer)
{
  FillBuffer(buffer);
  const Result rc = audoutAppendAudioOutBuffer(buffer);
  if (R_FAILED(rc))
  {
    m_append_failures.fetch_add(1, std::memory_order_relaxed);
    s_append_failures.fetch_add(1, std::memory_order_relaxed);
    Error("Audio", "audoutAppendAudioOutBuffer failed: 0x%08x", static_cast<unsigned>(rc));
    return false;
  }
  m_requeues.fetch_add(1, std::memory_order_relaxed);
  s_requeues.fetch_add(1, std::memory_order_relaxed);
  return true;
}

bool Audio::QueueInitialBuffers()
{
  Log("Audio", "queueing %zu initial buffers", BUFFER_COUNT);
  for (AudioOutBuffer& buffer : m_buffers)
  {
    if (!RequeueBuffer(&buffer))
      return false;
  }

  const Result rc = audoutStartAudioOut();
  if (R_FAILED(rc))
  {
    Error("Audio", "audoutStartAudioOut failed: 0x%08x", static_cast<unsigned>(rc));
    return false;
  }
  m_device_started = true;
  Log("Audio", "audoutStartAudioOut OK");
  FlushLog();
  return true;
}

void Audio::StopDevice()
{
  if (!m_initialized)
    return;

  if (m_device_started)
  {
    const Result rc = audoutStopAudioOut();
    if (R_FAILED(rc))
      Error("Audio", "audoutStopAudioOut failed: 0x%08x", static_cast<unsigned>(rc));
    else
      Log("Audio", "audoutStopAudioOut OK");
    m_device_started = false;
  }

  bool flushed = false;
  const Result flush_rc = audoutFlushAudioOutBuffers(&flushed);
  if (R_FAILED(flush_rc))
    Error("Audio", "audoutFlushAudioOutBuffers failed: 0x%08x", static_cast<unsigned>(flush_rc));
}

void Audio::ThreadMain()
{
  Log("Audio", "worker thread started");
  while (!m_stop.load(std::memory_order_acquire))
  {
    if (!m_running.load(std::memory_order_acquire))
    {
      if (m_device_started)
        StopDevice();
      svcSleepThread(IDLE_SLEEP_NS);
      continue;
    }

    if (!m_device_started)
    {
      if (!QueueInitialBuffers())
      {
        m_running.store(false, std::memory_order_release);
        StopDevice();
        continue;
      }
    }

    AudioOutBuffer* released = nullptr;
    u32 released_count = 0;
    const Result rc = audoutWaitPlayFinish(&released, &released_count, AUDIO_WAIT_NS);
    if (R_FAILED(rc))
    {
      const auto failures = m_wait_failures.fetch_add(1, std::memory_order_relaxed) + 1;
      s_wait_failures.fetch_add(1, std::memory_order_relaxed);
      if (failures == 1 || (failures % 128) == 0)
        Log("Audio", "audoutWaitPlayFinish result=0x%08x failures=%llu",
            static_cast<unsigned>(rc), static_cast<unsigned long long>(failures));
      continue;
    }

    if (released_count != 0 && released)
    {
      const auto events = m_release_events.fetch_add(released_count, std::memory_order_relaxed) +
                          released_count;
      s_release_events.fetch_add(released_count, std::memory_order_relaxed);
      if (events == released_count)
      {
        Log("Audio", "first released buffer: count=%u; hardware playback loop active",
            released_count);
        FlushLog();
      }
      if (!RequeueBuffer(released))
      {
        m_running.store(false, std::memory_order_release);
        StopDevice();
      }
    }
  }
  Log("Audio", "worker thread stopping");
}
}  // namespace Horizon

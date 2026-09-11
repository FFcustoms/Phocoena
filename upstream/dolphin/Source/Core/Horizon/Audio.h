// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>

#include <switch.h>

#include "AudioCommon/SoundStream.h"

namespace Horizon
{
struct AudioDiagnosticsStats
{
  std::uint64_t release_events = 0;
  std::uint64_t requeues = 0;
  std::uint64_t wait_failures = 0;
  std::uint64_t append_failures = 0;
};

// Lightweight counters used by the ~5 s support reporter. Reading/resetting these
// does not touch the SD card and does not add per-frame work.
AudioDiagnosticsStats ReadAudioDiagnosticsStats();
void ResetAudioDiagnosticsStats();

class Audio final : public SoundStream
{
public:
  Audio() = default;
  ~Audio() override;

  bool Init() override;
  bool SetRunning(bool running) override;
  void SetVolume(int volume) override;

private:
  static constexpr std::size_t BUFFER_COUNT = 3;
  static constexpr std::size_t FRAMES_PER_BUFFER = 1024;
  static constexpr std::size_t BYTES_PER_FRAME = sizeof(s16) * 2;
  static constexpr std::size_t BUFFER_BYTES = FRAMES_PER_BUFFER * BYTES_PER_FRAME;

  bool QueueInitialBuffers();
  bool RequeueBuffer(AudioOutBuffer* buffer);
  void FillBuffer(AudioOutBuffer* buffer);
  void ThreadMain();
  void StopDevice();

  std::array<AudioOutBuffer, BUFFER_COUNT> m_buffers{};
  std::array<void*, BUFFER_COUNT> m_storage{};
  std::thread m_thread;
  std::atomic<bool> m_running{false};
  std::atomic<bool> m_stop{false};
  std::atomic<std::uint64_t> m_requeues{0};
  std::atomic<std::uint64_t> m_wait_failures{0};
  std::atomic<std::uint64_t> m_release_events{0};
  std::atomic<std::uint64_t> m_append_failures{0};
  bool m_initialized = false;
  bool m_device_started = false;
};
}  // namespace Horizon

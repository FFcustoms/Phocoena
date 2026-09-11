// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace Horizon::PhocoenaClock
{
inline constexpr std::uint32_t PERFORMANCE_CPU_TARGET_HZ = 1'785'000'000;
inline constexpr std::uint32_t PERFORMANCE_GPU_TARGET_HZ = 768'000'000;
inline constexpr std::uint32_t LEGACY_CPU_TARGET_HZ = 1'581'000'000;
inline constexpr std::uint32_t LEGACY_GPU_TARGET_HZ = 614'400'000;

enum class Profile : std::uint8_t
{
  Performance,
  Legacy,
};

struct ClockTargets
{
  std::uint32_t cpu_hz;
  std::uint32_t gpu_hz;
};

constexpr ClockTargets TargetsForProfile(const Profile profile)
{
  return profile == Profile::Legacy ?
             ClockTargets{LEGACY_CPU_TARGET_HZ, LEGACY_GPU_TARGET_HZ} :
             ClockTargets{PERFORMANCE_CPU_TARGET_HZ, PERFORMANCE_GPU_TARGET_HZ};
}

constexpr std::string_view ProfileName(const Profile profile)
{
  return profile == Profile::Legacy ? "legacy" : "performance";
}

// Intentionally contains no memory domain. Memory is read only in ClockReadings
// and cannot reach Backend::RequestRate through this interface.
enum class Domain : std::uint8_t
{
  CPU,
  GPU,
};

struct ClockReadings
{
  std::uint32_t cpu_hz = 0;
  std::uint32_t gpu_hz = 0;
  std::uint32_t memory_hz = 0;
  bool cpu_valid = false;
  bool gpu_valid = false;
  bool memory_valid = false;
};

class Backend
{
public:
  virtual ~Backend() = default;

  virtual std::string_view Mechanism() const = 0;
  virtual std::string_view ExternalManager() const = 0;
  virtual bool Initialize(ClockReadings* before, std::string* error) = 0;
  virtual bool GetAvailableRates(Domain domain, std::vector<std::uint32_t>* rates,
                                 std::string* error) = 0;
  virtual bool RequestRate(Domain domain, std::uint32_t hz, std::string* error) = 0;
  virtual void WaitForApply() = 0;
  virtual bool ReadEffective(ClockReadings* effective, std::string* error) = 0;
  // Restores only CPU/GPU state still owned by this backend. There is no
  // memory-clock restoration or write API.
  virtual bool Release(std::string* detail) = 0;
};

struct BackendSelection
{
  std::unique_ptr<Backend> backend;
  std::string external_manager;
  std::string error;
};

using BackendFactory = std::function<BackendSelection()>;

enum class StartStatus : std::uint8_t
{
  Disabled,
  Active,
  Failed,
};

struct StartReport
{
  StartStatus status = StartStatus::Disabled;
  std::string mechanism = "disabled";
  std::string external_manager = "not checked";
  std::string detail;
  ClockReadings before;
  ClockReadings effective;
  Profile profile = Profile::Performance;
  std::uint32_t requested_cpu_hz = PERFORMANCE_CPU_TARGET_HZ;
  std::uint32_t requested_gpu_hz = PERFORMANCE_GPU_TARGET_HZ;
  std::uint32_t selected_cpu_hz = 0;
  std::uint32_t selected_gpu_hz = 0;
  bool cpu_requested = false;
  bool gpu_requested = false;
};

struct StopReport
{
  bool attempted = false;
  bool succeeded = true;
  std::string mechanism = "disabled";
  std::string detail;
};

std::uint32_t SelectNearestAtOrBelow(const std::vector<std::uint32_t>& rates,
                                     std::uint32_t target_hz);
bool ControlStillOwned(std::uint32_t current_hz, std::uint32_t requested_hz);

class Session final
{
public:
  Session() = default;
  ~Session();
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  const StartReport& Start(bool enabled, const BackendFactory& factory,
                           Profile profile = Profile::Performance);
  StopReport Stop();
  bool IsActive() const { return m_report.status == StartStatus::Active && m_backend != nullptr; }
  const StartReport& Report() const { return m_report; }

private:
  void FailAndRelease(std::string detail);

  std::unique_ptr<Backend> m_backend;
  StartReport m_report;
};
}

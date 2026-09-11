// SPDX-License-Identifier: GPL-2.0-or-later
#include "Horizon/ClockPolicy.h"

#include <algorithm>
#include <utility>

namespace Horizon::PhocoenaClock
{
std::uint32_t SelectNearestAtOrBelow(const std::vector<std::uint32_t>& rates,
                                     const std::uint32_t target_hz)
{
  std::uint32_t selected = 0;
  for (const std::uint32_t rate : rates)
  {
    if (rate != 0 && rate <= target_hz)
      selected = std::max(selected, rate);
  }
  return selected;
}

bool ControlStillOwned(const std::uint32_t current_hz, const std::uint32_t requested_hz)
{
  return current_hz == requested_hz;
}

Session::~Session()
{
  Stop();
}

const StartReport& Session::Start(const bool enabled, const BackendFactory& factory,
                                  const Profile profile)
{
  if (IsActive())
  {
    m_report.detail = "clock session already active; no additional requests made";
    return m_report;
  }

  m_report = {};
  m_report.profile = profile;
  const ClockTargets targets = TargetsForProfile(profile);
  m_report.requested_cpu_hz = targets.cpu_hz;
  m_report.requested_gpu_hz = targets.gpu_hz;
  if (!enabled)
  {
    m_report.status = StartStatus::Disabled;
    m_report.mechanism = "disabled by phocoena_auto_clocks=0";
    m_report.external_manager = "not checked";
    m_report.detail = "zero CPU/GPU/MEM clock writes";
    return m_report;
  }

  BackendSelection selection = factory();
  m_report.external_manager = selection.external_manager.empty() ? "none detected" :
                                                                    selection.external_manager;
  if (!selection.backend)
  {
    m_report.status = StartStatus::Failed;
    m_report.mechanism = "none";
    m_report.detail = selection.error.empty() ? "no compatible clock-control backend" :
                                               std::move(selection.error);
    return m_report;
  }

  m_backend = std::move(selection.backend);
  m_report.mechanism = std::string(m_backend->Mechanism());
  if (!m_backend->ExternalManager().empty())
    m_report.external_manager = std::string(m_backend->ExternalManager());

  std::string error;
  if (!m_backend->Initialize(&m_report.before, &error))
  {
    FailAndRelease(error.empty() ? "clock backend initialization failed" : std::move(error));
    return m_report;
  }

  std::vector<std::uint32_t> cpu_rates;
  std::vector<std::uint32_t> gpu_rates;
  if (!m_backend->GetAvailableRates(Domain::CPU, &cpu_rates, &error) ||
      !m_backend->GetAvailableRates(Domain::GPU, &gpu_rates, &error))
  {
    FailAndRelease(error.empty() ? "failed reading supported CPU/GPU rates" : std::move(error));
    return m_report;
  }

  m_report.selected_cpu_hz = SelectNearestAtOrBelow(cpu_rates, targets.cpu_hz);
  m_report.selected_gpu_hz = SelectNearestAtOrBelow(gpu_rates, targets.gpu_hz);
  if (m_report.selected_cpu_hz == 0 || m_report.selected_gpu_hz == 0)
  {
    FailAndRelease("no supported CPU/GPU rate exists at or below the requested targets");
    return m_report;
  }

  m_report.cpu_requested = true;
  if (!m_backend->RequestRate(Domain::CPU, m_report.selected_cpu_hz, &error))
  {
    FailAndRelease(error.empty() ? "CPU clock request failed" : std::move(error));
    return m_report;
  }

  m_report.gpu_requested = true;
  if (!m_backend->RequestRate(Domain::GPU, m_report.selected_gpu_hz, &error))
  {
    FailAndRelease(error.empty() ? "GPU clock request failed; CPU request rolled back" :
                                   std::move(error));
    return m_report;
  }

  m_backend->WaitForApply();
  if (!m_backend->ReadEffective(&m_report.effective, &error))
  {
    // The requests succeeded. Keep the session active so normal Stop() still
    // relinquishes ownership; effective clocks are simply reported unknown.
    m_report.detail = error.empty() ? "requests accepted; effective clock read unavailable" :
                                     std::move(error);
  }
  else
  {
    m_report.detail = "CPU/GPU requests accepted; memory remained read only";
  }
  m_report.status = StartStatus::Active;
  return m_report;
}

StopReport Session::Stop()
{
  StopReport report;
  if (!m_backend)
    return report;

  report.attempted = true;
  report.mechanism = std::string(m_backend->Mechanism());
  report.succeeded = m_backend->Release(&report.detail);
  m_backend.reset();
  if (m_report.status == StartStatus::Active)
    m_report.status = StartStatus::Disabled;
  return report;
}

void Session::FailAndRelease(std::string detail)
{
  std::string release_detail;
  const bool released = !m_backend || m_backend->Release(&release_detail);
  if (!released)
  {
    if (!detail.empty())
      detail += "; ";
    detail += release_detail.empty() ? "rollback failed" : release_detail;
  }
  m_backend.reset();
  m_report.status = StartStatus::Failed;
  m_report.detail = std::move(detail);
}
}

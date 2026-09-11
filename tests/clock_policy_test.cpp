// SPDX-License-Identifier: GPL-2.0-or-later
#include "Horizon/ClockPolicy.h"

#include <cassert>
#include <concepts>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace
{
using namespace Horizon::PhocoenaClock;

template <typename T>
concept HasMemoryWriteMethod = requires(T& value) { value.RequestMemoryRate(std::uint32_t{}); };
static_assert(!HasMemoryWriteMethod<Backend>);

struct MockState
{
  ClockReadings before{1'020'000'000, 460'800'000, 1'600'000'000, true, true, true};
  ClockReadings effective{1'785'000'000, 921'600'000, 1'600'000'000, true, true, true};
  std::vector<std::uint32_t> cpu_rates{612'000'000, 1'020'000'000, 1'581'000'000,
                                       1'785'000'000};
  std::vector<std::uint32_t> gpu_rates{307'200'000, 460'800'000, 614'400'000,
                                       921'600'000};
  bool initialize_ok = true;
  bool rates_ok = true;
  bool effective_ok = true;
  bool release_ok = true;
  bool fail_cpu_request = false;
  bool fail_gpu_request = false;
  unsigned initialize_calls = 0;
  unsigned rate_queries = 0;
  unsigned wait_calls = 0;
  unsigned effective_reads = 0;
  unsigned release_calls = 0;
  unsigned memory_write_calls = 0;
  std::vector<std::pair<Domain, std::uint32_t>> writes;
};

class MockBackend final : public Backend
{
public:
  explicit MockBackend(std::shared_ptr<MockState> state, std::string mechanism = "mock-clkrst",
                       std::string external = {})
      : m_state(std::move(state)), m_mechanism(std::move(mechanism)),
        m_external(std::move(external))
  {
  }

  std::string_view Mechanism() const override { return m_mechanism; }
  std::string_view ExternalManager() const override { return m_external; }
  bool Initialize(ClockReadings* before, std::string* error) override
  {
    ++m_state->initialize_calls;
    *before = m_state->before;
    if (!m_state->initialize_ok && error)
      *error = "synthetic initialize failure";
    return m_state->initialize_ok;
  }
  bool GetAvailableRates(Domain domain, std::vector<std::uint32_t>* rates,
                         std::string* error) override
  {
    ++m_state->rate_queries;
    *rates = domain == Domain::CPU ? m_state->cpu_rates : m_state->gpu_rates;
    if (!m_state->rates_ok && error)
      *error = "synthetic rate-list failure";
    return m_state->rates_ok;
  }
  bool RequestRate(Domain domain, std::uint32_t hz, std::string* error) override
  {
    m_state->writes.emplace_back(domain, hz);
    const bool fail = domain == Domain::CPU ? m_state->fail_cpu_request :
                                              m_state->fail_gpu_request;
    if (fail && error)
      *error = "synthetic request failure";
    return !fail;
  }
  void WaitForApply() override { ++m_state->wait_calls; }
  bool ReadEffective(ClockReadings* effective, std::string* error) override
  {
    ++m_state->effective_reads;
    *effective = m_state->effective;
    if (!m_state->effective_ok && error)
      *error = "synthetic effective read failure";
    return m_state->effective_ok;
  }
  bool Release(std::string* detail) override
  {
    ++m_state->release_calls;
    if (detail)
      *detail = m_state->release_ok ? "synthetic release complete; MEM writes=0" :
                                     "synthetic release failure; MEM writes=0";
    return m_state->release_ok;
  }

private:
  std::shared_ptr<MockState> m_state;
  std::string m_mechanism;
  std::string m_external;
};

BackendFactory Factory(const std::shared_ptr<MockState>& state,
                       std::string mechanism = "mock-clkrst", std::string external = {})
{
  return [state, mechanism = std::move(mechanism), external = std::move(external)]() mutable {
    return BackendSelection{std::make_unique<MockBackend>(state, std::move(mechanism),
                                                          std::move(external)),
                            {}, {}};
  };
}
}

int main()
{
  using namespace Horizon::PhocoenaClock;
  static_assert(PERFORMANCE_CPU_TARGET_HZ == 1'785'000'000);
  static_assert(PERFORMANCE_GPU_TARGET_HZ == 921'600'000);
  static_assert(LEGACY_CPU_TARGET_HZ == 1'581'000'000);
  static_assert(LEGACY_GPU_TARGET_HZ == 614'400'000);
  static_assert(TargetsForProfile(Profile::Performance).cpu_hz == PERFORMANCE_CPU_TARGET_HZ);
  static_assert(TargetsForProfile(Profile::Legacy).gpu_hz == LEGACY_GPU_TARGET_HZ);

  // Selection is order-independent, ignores zero, and never selects above the
  // target. No lower rate means no request rather than an unsafe round-up.
  assert(SelectNearestAtOrBelow({1'700, 0, 1'500, 1'600, 1'500}, 1'581) == 1'500);
  assert(SelectNearestAtOrBelow({1'582, 1'600}, 1'581) == 0);
  assert(ControlStillOwned(614'400'000, 614'400'000));
  assert(!ControlStillOwned(537'600'000, 614'400'000));

  // Default performance request, memory unchanged, and one bounded settle.
  auto exact = std::make_shared<MockState>();
  Session exact_session;
  const auto& exact_report = exact_session.Start(true, Factory(exact, "sys-clk", "sys-clk"));
  assert(exact_report.status == StartStatus::Active);
  assert(exact_report.profile == Profile::Performance);
  assert(exact_report.selected_cpu_hz == PERFORMANCE_CPU_TARGET_HZ);
  assert(exact_report.selected_gpu_hz == PERFORMANCE_GPU_TARGET_HZ);
  assert(exact->writes.size() == 2);
  assert(exact->writes[0] == std::pair(Domain::CPU, PERFORMANCE_CPU_TARGET_HZ));
  assert(exact->writes[1] == std::pair(Domain::GPU, PERFORMANCE_GPU_TARGET_HZ));
  assert(exact->wait_calls == 1 && exact->memory_write_calls == 0);
  const auto& repeated_report = exact_session.Start(true, Factory(exact));
  assert(repeated_report.status == StartStatus::Active && exact->writes.size() == 2);
  const StopReport exact_stop = exact_session.Stop();
  assert(exact_stop.attempted && exact_stop.succeeded && exact->release_calls == 1);
  assert(!exact_session.Stop().attempted); // idempotent shutdown

  // Legacy mode exactly reproduces the v0.1.18-v0.1.24 CPU/GPU policy.
  auto legacy = std::make_shared<MockState>();
  Session legacy_session;
  const auto& legacy_report = legacy_session.Start(true, Factory(legacy), Profile::Legacy);
  assert(legacy_report.profile == Profile::Legacy);
  assert(legacy_report.requested_cpu_hz == LEGACY_CPU_TARGET_HZ);
  assert(legacy_report.requested_gpu_hz == LEGACY_GPU_TARGET_HZ);
  assert(legacy->writes[0] == std::pair(Domain::CPU, LEGACY_CPU_TARGET_HZ));
  assert(legacy->writes[1] == std::pair(Domain::GPU, LEGACY_GPU_TARGET_HZ));
  legacy_session.Stop();

  // Nearest-lower policy is applied independently to CPU and GPU.
  auto lower = std::make_shared<MockState>();
  lower->cpu_rates = {1'020'000'000, 1'530'000'000, 1'683'000'000};
  lower->gpu_rates = {460'800'000, 537'600'000, 691'200'000};
  Session lower_session;
  const auto& lower_report = lower_session.Start(true, Factory(lower));
  assert(lower_report.selected_cpu_hz == 1'683'000'000);
  assert(lower_report.selected_gpu_hz == 691'200'000);
  lower_session.Stop();

  // Auto Clocks Off does not even construct/open a backend: zero reads and
  // zero CPU/GPU/MEM writes.
  bool factory_called = false;
  Session disabled_session;
  const auto& disabled = disabled_session.Start(false, [&] {
    factory_called = true;
    return BackendSelection{};
  });
  assert(disabled.status == StartStatus::Disabled && !factory_called);
  assert(!disabled_session.Stop().attempted);

  // Unsupported external manager remains untouched.
  Session unsupported_session;
  const auto& unsupported = unsupported_session.Start(true, [] {
    return BackendSelection{nullptr, "synthetic external manager",
                            "unsupported API; no clock writes attempted"};
  });
  assert(unsupported.status == StartStatus::Failed);
  assert(unsupported.external_manager == "synthetic external manager");

  // Initialization failure and partial CPU-success/GPU-failure both clean up.
  auto init_failure = std::make_shared<MockState>();
  init_failure->initialize_ok = false;
  Session init_failure_session;
  assert(init_failure_session.Start(true, Factory(init_failure)).status == StartStatus::Failed);
  assert(init_failure->writes.empty() && init_failure->release_calls == 1);

  auto partial = std::make_shared<MockState>();
  partial->fail_gpu_request = true;
  Session partial_session;
  assert(partial_session.Start(true, Factory(partial)).status == StartStatus::Failed);
  assert(partial->writes.size() == 2 && partial->release_calls == 1);
  assert(partial->memory_write_calls == 0);

  // Effective-read failure is diagnostic-only; Stop still restores control.
  auto read_failure = std::make_shared<MockState>();
  read_failure->effective_ok = false;
  Session read_failure_session;
  assert(read_failure_session.Start(true, Factory(read_failure)).status == StartStatus::Active);
  assert(read_failure_session.Stop().succeeded && read_failure->release_calls == 1);

  // Repeated independent start/stop cycles do not accumulate backend state.
  auto cycles = std::make_shared<MockState>();
  Session cycle_session;
  for (unsigned i = 0; i < 1000; ++i)
  {
    assert(cycle_session.Start(true, Factory(cycles)).status == StartStatus::Active);
    assert(cycle_session.Stop().succeeded);
  }
  assert(cycles->initialize_calls == 1000 && cycles->release_calls == 1000);
  assert(cycles->writes.size() == 2000 && cycles->memory_write_calls == 0);

  std::cout << "PASS: Phocoena CPU/GPU clock policy, nearest-lower selection, rollback, "
               "external-manager coexistence, repeated lifecycle, and zero MEM writes\n";
}

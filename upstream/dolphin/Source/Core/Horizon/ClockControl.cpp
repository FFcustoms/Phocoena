// SPDX-License-Identifier: GPL-2.0-or-later
#include "Horizon/ClockControl.h"

#include <switch.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "Horizon/Log.h"

namespace Horizon
{
namespace
{
using PhocoenaClock::Backend;
using PhocoenaClock::BackendSelection;
using PhocoenaClock::ClockReadings;
using PhocoenaClock::ControlStillOwned;
using PhocoenaClock::Domain;

constexpr std::size_t CPU_INDEX = 0;
constexpr std::size_t GPU_INDEX = 1;
constexpr std::size_t MEMORY_INDEX = 2;
constexpr std::uint32_t SYSCLK_OFFICIAL_API_VERSION = 4;
constexpr std::uint32_t SYSCLK_OC_API_VERSION = 2;
constexpr std::uint32_t SYSCLK_CMD_GET_API_VERSION = 0;
constexpr std::uint32_t SYSCLK_CMD_GET_CONTEXT = 2;
constexpr std::uint32_t SYSCLK_CMD_SET_OVERRIDE = 8;
constexpr std::uint32_t SYSCLK_CMD_GET_FREQ_LIST = 11;
constexpr std::uint32_t SYSCLK_OC_CMD_GET_FREQUENCY_TABLE = 12;
constexpr std::size_t SYSCLK_FREQ_LIST_MAX = 32;
constexpr std::size_t SYSCLK_OC_FREQ_LIST_MAX = 31;
constexpr std::int64_t SYSCLK_APPLY_WAIT_NS = 600'000'000;

std::size_t Index(const Domain domain)
{
  return domain == Domain::CPU ? CPU_INDEX : GPU_INDEX;
}

std::uint32_t ProtocolModule(const Domain domain)
{
  return static_cast<std::uint32_t>(Index(domain));
}

std::string ResultText(const char* operation, const Result result)
{
  char text[128];
  std::snprintf(text, sizeof(text), "%s failed: 0x%08x", operation, result);
  return text;
}

void SetError(std::string* error, std::string value)
{
  if (error)
    *error = std::move(value);
}

struct SysClkSetOverrideArgs
{
  std::uint32_t module;
  std::uint32_t hz;
};

struct SysClkGetFreqListArgs
{
  std::uint32_t module;
  std::uint32_t max_count;
};

struct SysClkOcGetFrequencyTableArgs
{
  std::uint32_t module;
  std::uint32_t profile;
};

// Exact API-4 wire layout from retronx-team/sys-clk. Explicit padding avoids
// relying on compiler treatment of the leading uint8_t.
struct SysClkOfficialContext
{
  std::uint8_t enabled;
  std::uint8_t padding[7];
  std::uint64_t application_id;
  std::uint32_t profile;
  std::uint32_t freqs[3];
  std::uint32_t real_freqs[3];
  std::uint32_t override_freqs[3];
  std::uint32_t temperatures[3];
  std::int32_t power[2];
  std::uint32_t ram_load[2];
};

// Exact API-2 wire layout used by the established Switch-OC-Suite sysclkOC
// client. New or incompatible versions are detected and left untouched.
struct SysClkOcContext
{
  std::uint8_t enabled;
  std::uint8_t padding[7];
  std::uint64_t application_id;
  std::uint32_t profile;
  std::uint32_t freqs[3];
  std::uint32_t override_freqs[3];
  std::uint32_t temperatures[3];
  std::uint32_t performance_configuration;
};

struct SysClkOcFrequencyTable
{
  std::uint32_t rates[SYSCLK_OC_FREQ_LIST_MAX];
};

static_assert(sizeof(SysClkOfficialContext) == 88);
static_assert(offsetof(SysClkOfficialContext, override_freqs) == 44);
static_assert(sizeof(SysClkOcContext) == 64);
static_assert(offsetof(SysClkOcContext, override_freqs) == 32);
static_assert(sizeof(SysClkSetOverrideArgs) == 8);

Result GetApiVersion(Service* service, std::uint32_t* version)
{
  return serviceDispatchOut(service, SYSCLK_CMD_GET_API_VERSION, *version);
}

Result SetOverride(Service* service, const Domain domain, const std::uint32_t hz)
{
  const SysClkSetOverrideArgs args{ProtocolModule(domain), hz};
  return serviceDispatchIn(service, SYSCLK_CMD_SET_OVERRIDE, args);
}

enum class SysClkFlavor
{
  Official,
  OcSuite,
};

class SysClkBackend final : public Backend
{
public:
  explicit SysClkBackend(const SysClkFlavor flavor) : m_flavor(flavor) {}
  ~SysClkBackend() override
  {
    std::string ignored;
    Release(&ignored);
    if (serviceIsActive(&m_service))
      serviceClose(&m_service);
  }

  bool Connect(const char* service_name, const std::uint32_t expected_api, bool* service_present,
               std::string* error)
  {
    Result result = smGetService(&m_service, service_name);
    if (R_FAILED(result))
    {
      if (service_present)
        *service_present = false;
      return false;
    }
    if (service_present)
      *service_present = true;

    result = GetApiVersion(&m_service, &m_api_version);
    if (R_FAILED(result))
    {
      SetError(error, ResultText("clock-manager API query", result));
      return false;
    }
    if (m_api_version != expected_api)
    {
      std::ostringstream out;
      out << service_name << " API " << m_api_version << " is incompatible with expected API "
          << expected_api << "; no direct clock writes attempted";
      SetError(error, out.str());
      return false;
    }

    m_service_name = service_name;
    m_mechanism = m_flavor == SysClkFlavor::Official ? "sys-clk temporary override IPC API 4" :
                                                       "sys-clk-OC temporary override IPC API 2";
    m_external_manager = m_flavor == SysClkFlavor::Official ? "sys-clk (sys:clk)" :
                                                              "sys-clk-OC (sysclkOC)";
    return true;
  }

  std::string_view Mechanism() const override { return m_mechanism; }
  std::string_view ExternalManager() const override { return m_external_manager; }

  bool Initialize(ClockReadings* before, std::string* error) override
  {
    std::array<std::uint32_t, 3> overrides{};
    if (!ReadContext(before, &overrides, error))
      return false;
    m_previous_overrides[CPU_INDEX] = overrides[CPU_INDEX];
    m_previous_overrides[GPU_INDEX] = overrides[GPU_INDEX];
    m_initialized = true;
    return true;
  }

  bool GetAvailableRates(const Domain domain, std::vector<std::uint32_t>* rates,
                         std::string* error) override
  {
    rates->clear();
    if (m_flavor == SysClkFlavor::Official)
    {
      std::array<std::uint32_t, SYSCLK_FREQ_LIST_MAX> list{};
      std::uint32_t count = 0;
      const SysClkGetFreqListArgs args{ProtocolModule(domain),
                                       static_cast<std::uint32_t>(list.size())};
      const Result result = serviceDispatchInOut(
          &m_service, SYSCLK_CMD_GET_FREQ_LIST, args, count,
          .buffer_attrs = {SfBufferAttr_HipcAutoSelect | SfBufferAttr_Out},
          .buffers = {{list.data(), list.size() * sizeof(list[0])}});
      if (R_FAILED(result))
      {
        SetError(error, ResultText("sys-clk supported-rate query", result));
        return false;
      }
      if (count > list.size())
      {
        SetError(error, "sys-clk returned an invalid supported-rate count");
        return false;
      }
      rates->assign(list.begin(), list.begin() + count);
      return true;
    }

    SysClkOcFrequencyTable table{};
    const SysClkOcGetFrequencyTableArgs args{ProtocolModule(domain), m_profile};
    const Result result = serviceDispatchInOut(
        &m_service, SYSCLK_OC_CMD_GET_FREQUENCY_TABLE, args, table);
    if (R_FAILED(result))
    {
      SetError(error, ResultText("sys-clk-OC supported-rate query", result));
      return false;
    }
    for (const std::uint32_t rate : table.rates)
    {
      if (rate != 0)
        rates->push_back(rate);
    }
    return true;
  }

  bool RequestRate(const Domain domain, const std::uint32_t hz, std::string* error) override
  {
    const std::size_t index = Index(domain);
    m_attempted[index] = true;
    m_requested[index] = hz;
    const Result result = SetOverride(&m_service, domain, hz);
    if (R_FAILED(result))
    {
      SetError(error, ResultText(domain == Domain::CPU ? "CPU override" : "GPU override", result));
      return false;
    }
    m_applied[index] = true;
    return true;
  }

  void WaitForApply() override
  {
    // sys-clk applies overrides on its manager tick (300-500 ms by default).
    // One bounded startup wait permits an effective-rate diagnostic without
    // polling or continuously fighting another controller.
    svcSleepThread(SYSCLK_APPLY_WAIT_NS);
  }

  bool ReadEffective(ClockReadings* effective, std::string* error) override
  {
    return ReadContext(effective, nullptr, error);
  }

  bool Release(std::string* detail) override
  {
    if (!m_initialized && !m_attempted[CPU_INDEX] && !m_attempted[GPU_INDEX])
    {
      if (detail)
        *detail = "no clock overrides to release; MEM writes=0";
      return true;
    }

    std::array<std::uint32_t, 3> current_overrides{};
    ClockReadings unused;
    std::string read_error;
    const bool current_valid = ReadContext(&unused, &current_overrides, &read_error);
    bool success = true;
    std::ostringstream out;
    for (const Domain domain : {Domain::CPU, Domain::GPU})
    {
      const std::size_t index = Index(domain);
      if (!m_attempted[index])
        continue;

      const char* name = domain == Domain::CPU ? "CPU" : "GPU";
      if (current_valid && !ControlStillOwned(current_overrides[index], m_requested[index]))
      {
        out << name << " changed externally; preserved current override; ";
      }
      else
      {
        const Result result = SetOverride(&m_service, domain, m_previous_overrides[index]);
        if (R_FAILED(result))
        {
          success = false;
          out << name << " restore failed 0x" << std::hex << result << std::dec << "; ";
        }
        else
        {
          out << name << " previous override=" << m_previous_overrides[index] << " restored; ";
        }
      }
      m_attempted[index] = false;
      m_applied[index] = false;
    }
    m_initialized = false;
    if (!current_valid)
      out << "ownership read unavailable (conservative restore): " << read_error << "; ";
    out << "MEM writes=0";
    if (detail)
      *detail = out.str();
    return success;
  }

private:
  bool ReadContext(ClockReadings* readings, std::array<std::uint32_t, 3>* overrides,
                   std::string* error)
  {
    if (m_flavor == SysClkFlavor::Official)
    {
      SysClkOfficialContext context{};
      const Result result = serviceDispatchOut(&m_service, SYSCLK_CMD_GET_CONTEXT, context);
      if (R_FAILED(result))
      {
        SetError(error, ResultText("sys-clk context query", result));
        return false;
      }
      m_profile = context.profile;
      readings->cpu_hz = context.real_freqs[CPU_INDEX] != 0 ? context.real_freqs[CPU_INDEX] :
                                                                  context.freqs[CPU_INDEX];
      readings->gpu_hz = context.real_freqs[GPU_INDEX] != 0 ? context.real_freqs[GPU_INDEX] :
                                                                  context.freqs[GPU_INDEX];
      readings->memory_hz = context.real_freqs[MEMORY_INDEX] != 0 ?
                                context.real_freqs[MEMORY_INDEX] : context.freqs[MEMORY_INDEX];
      if (overrides)
        std::copy(std::begin(context.override_freqs), std::end(context.override_freqs),
                  overrides->begin());
    }
    else
    {
      SysClkOcContext context{};
      const Result result = serviceDispatchOut(&m_service, SYSCLK_CMD_GET_CONTEXT, context);
      if (R_FAILED(result))
      {
        SetError(error, ResultText("sys-clk-OC context query", result));
        return false;
      }
      m_profile = context.profile;
      readings->cpu_hz = context.freqs[CPU_INDEX];
      readings->gpu_hz = context.freqs[GPU_INDEX];
      readings->memory_hz = context.freqs[MEMORY_INDEX];
      if (overrides)
        std::copy(std::begin(context.override_freqs), std::end(context.override_freqs),
                  overrides->begin());
    }
    readings->cpu_valid = readings->cpu_hz != 0;
    readings->gpu_valid = readings->gpu_hz != 0;
    readings->memory_valid = readings->memory_hz != 0;
    return true;
  }

  Service m_service{};
  SysClkFlavor m_flavor;
  std::string m_service_name;
  std::string m_mechanism;
  std::string m_external_manager;
  std::uint32_t m_api_version = 0;
  std::uint32_t m_profile = 0;
  std::array<std::uint32_t, 2> m_previous_overrides{};
  std::array<std::uint32_t, 2> m_requested{};
  std::array<bool, 2> m_attempted{};
  std::array<bool, 2> m_applied{};
  bool m_initialized = false;
};

class DirectClockBackend final : public Backend
{
public:
  DirectClockBackend() : m_modern(hosversionAtLeast(8, 0, 0)) {}
  ~DirectClockBackend() override
  {
    std::string ignored;
    Release(&ignored);
    Close();
  }

  std::string_view Mechanism() const override
  {
    return m_modern ? "libnx clkrst one-shot request" : "libnx pcv one-shot request";
  }
  std::string_view ExternalManager() const override { return {}; }

  bool Initialize(ClockReadings* before, std::string* error) override
  {
    if (!Open(error))
      return false;
    if (!ReadWritable(Domain::CPU, &before->cpu_hz, error) ||
        !ReadWritable(Domain::GPU, &before->gpu_hz, error))
      return false;
    before->cpu_valid = true;
    before->gpu_valid = true;
    before->memory_valid = ReadMemory(&before->memory_hz);
    m_previous[CPU_INDEX] = before->cpu_hz;
    m_previous[GPU_INDEX] = before->gpu_hz;
    m_initialized = true;
    return true;
  }

  bool GetAvailableRates(const Domain domain, std::vector<std::uint32_t>* rates,
                         std::string* error) override
  {
    std::array<std::uint32_t, 64> list{};
    PcvClockRatesListType type = PcvClockRatesListType_Invalid;
    s32 count = 0;
    Result result;
    if (m_modern)
    {
      result = clkrstGetPossibleClockRates(WritableSession(domain), list.data(),
                                           static_cast<s32>(list.size()), &type, &count);
    }
    else
    {
      result = pcvGetPossibleClockRates(WritableModule(domain), list.data(),
                                        static_cast<s32>(list.size()), &type, &count);
    }
    if (R_FAILED(result))
    {
      SetError(error, ResultText("supported-rate query", result));
      return false;
    }
    if (type == PcvClockRatesListType_Invalid || count < 0 ||
        count > static_cast<s32>(list.size()))
    {
      SetError(error, "clock service returned an invalid supported-rate list");
      return false;
    }
    rates->assign(list.begin(), list.begin() + count);
    return true;
  }

  bool RequestRate(const Domain domain, const std::uint32_t hz, std::string* error) override
  {
    const std::size_t index = Index(domain);
    m_attempted[index] = true;
    m_requested[index] = hz;
    const Result result = SetWritable(domain, hz);
    if (R_FAILED(result))
    {
      SetError(error, ResultText(domain == Domain::CPU ? "CPU clock request" :
                                                         "GPU clock request",
                                 result));
      return false;
    }
    return true;
  }

  void WaitForApply() override {}

  bool ReadEffective(ClockReadings* effective, std::string* error) override
  {
    if (!ReadWritable(Domain::CPU, &effective->cpu_hz, error) ||
        !ReadWritable(Domain::GPU, &effective->gpu_hz, error))
      return false;
    effective->cpu_valid = true;
    effective->gpu_valid = true;
    effective->memory_valid = ReadMemory(&effective->memory_hz);
    return true;
  }

  bool Release(std::string* detail) override
  {
    if (!m_initialized && !m_attempted[CPU_INDEX] && !m_attempted[GPU_INDEX])
    {
      if (detail)
        *detail = "no direct clock requests to release; MEM writes=0";
      return true;
    }

    bool success = true;
    std::ostringstream out;
    for (const Domain domain : {Domain::CPU, Domain::GPU})
    {
      const std::size_t index = Index(domain);
      if (!m_attempted[index])
        continue;
      const char* name = domain == Domain::CPU ? "CPU" : "GPU";
      std::uint32_t current = 0;
      std::string ignored;
      const bool current_valid = ReadWritable(domain, &current, &ignored);
      if (current_valid && !ControlStillOwned(current, m_requested[index]))
      {
        out << name << " changed externally; preserved current rate; ";
      }
      else
      {
        const Result result = SetWritable(domain, m_previous[index]);
        if (R_FAILED(result))
        {
          success = false;
          out << name << " restore failed 0x" << std::hex << result << std::dec << "; ";
        }
        else
        {
          out << name << " previous rate=" << m_previous[index] << " restored; ";
        }
      }
      m_attempted[index] = false;
    }
    m_initialized = false;
    out << "MEM writes=0";
    if (detail)
      *detail = out.str();
    return success;
  }

private:
  static PcvModule WritableModule(const Domain domain)
  {
    return domain == Domain::CPU ? PcvModule_CpuBus : PcvModule_GPU;
  }

  ClkrstSession* WritableSession(const Domain domain)
  {
    return domain == Domain::CPU ? &m_cpu_session : &m_gpu_session;
  }

  bool Open(std::string* error)
  {
    if (m_service_initialized)
      return true;
    Result result;
    if (m_modern)
    {
      result = clkrstInitialize();
      if (R_FAILED(result))
      {
        SetError(error, ResultText("clkrstInitialize", result));
        return false;
      }
      m_service_initialized = true;
      if (!OpenSession(PcvModule_CpuBus, &m_cpu_session, &m_cpu_open, error) ||
          !OpenSession(PcvModule_GPU, &m_gpu_session, &m_gpu_open, error))
        return false;
      // Memory is diagnostic-only. Failure to open it must not prevent safe
      // CPU/GPU control and cannot create a memory write path.
      std::string ignored;
      OpenSession(PcvModule_EMC, &m_memory_read_session, &m_memory_read_open, &ignored);
    }
    else
    {
      result = pcvInitialize();
      if (R_FAILED(result))
      {
        SetError(error, ResultText("pcvInitialize", result));
        return false;
      }
      m_service_initialized = true;
    }
    return true;
  }

  bool OpenSession(const PcvModule module, ClkrstSession* session, bool* opened,
                   std::string* error)
  {
    PcvModuleId module_id{};
    Result result = pcvGetModuleId(&module_id, module);
    if (R_SUCCEEDED(result))
      result = clkrstOpenSession(session, module_id, 3);
    if (R_FAILED(result))
    {
      SetError(error, ResultText("clkrstOpenSession", result));
      return false;
    }
    *opened = true;
    return true;
  }

  void Close()
  {
    if (!m_service_initialized)
      return;
    if (m_modern)
    {
      if (m_memory_read_open)
        clkrstCloseSession(&m_memory_read_session);
      if (m_gpu_open)
        clkrstCloseSession(&m_gpu_session);
      if (m_cpu_open)
        clkrstCloseSession(&m_cpu_session);
      clkrstExit();
      m_memory_read_open = false;
      m_gpu_open = false;
      m_cpu_open = false;
    }
    else
    {
      pcvExit();
    }
    m_service_initialized = false;
  }

  bool ReadWritable(const Domain domain, std::uint32_t* hz, std::string* error)
  {
    const Result result = m_modern ? clkrstGetClockRate(WritableSession(domain), hz) :
                                     pcvGetClockRate(WritableModule(domain), hz);
    if (R_FAILED(result))
    {
      SetError(error, ResultText(domain == Domain::CPU ? "CPU clock read" : "GPU clock read",
                                 result));
      return false;
    }
    return true;
  }

  bool ReadMemory(std::uint32_t* hz)
  {
    if (m_modern)
      return m_memory_read_open && R_SUCCEEDED(clkrstGetClockRate(&m_memory_read_session, hz));
    return R_SUCCEEDED(pcvGetClockRate(PcvModule_EMC, hz));
  }

  Result SetWritable(const Domain domain, const std::uint32_t hz)
  {
    return m_modern ? clkrstSetClockRate(WritableSession(domain), hz) :
                      pcvSetClockRate(WritableModule(domain), hz);
  }

  bool m_modern = false;
  bool m_service_initialized = false;
  bool m_cpu_open = false;
  bool m_gpu_open = false;
  bool m_memory_read_open = false;
  bool m_initialized = false;
  ClkrstSession m_cpu_session{};
  ClkrstSession m_gpu_session{};
  ClkrstSession m_memory_read_session{};
  std::array<std::uint32_t, 2> m_previous{};
  std::array<std::uint32_t, 2> m_requested{};
  std::array<bool, 2> m_attempted{};
};

bool ServiceExists(const char* name)
{
  Service service{};
  const Result result = smGetService(&service, name);
  if (R_FAILED(result))
    return false;
  serviceClose(&service);
  return true;
}

BackendSelection CreatePlatformBackend()
{
  for (const auto& [name, flavor, api] : {
           std::tuple{"sys:clk", SysClkFlavor::Official, SYSCLK_OFFICIAL_API_VERSION},
           std::tuple{"sysclkOC", SysClkFlavor::OcSuite, SYSCLK_OC_API_VERSION},
       })
  {
    auto backend = std::make_unique<SysClkBackend>(flavor);
    bool present = false;
    std::string error;
    if (backend->Connect(name, api, &present, &error))
      return {std::move(backend), flavor == SysClkFlavor::Official ? "sys-clk" : "sys-clk-OC", {}};
    if (present)
      return {nullptr, name, std::move(error)};
  }

  // Horizon-OC uses a separate protocol. Merely detect it; never send commands
  // from the sys-clk protocol and never start a direct write fight against it.
  if (ServiceExists("hoc:clk"))
    return {nullptr, "Horizon-OC (hoc:clk)",
            "external clock manager uses an unsupported protocol; no clock writes attempted"};

  return {std::make_unique<DirectClockBackend>(), "none detected", {}};
}

const char* StatusName(const PhocoenaClock::StartStatus status)
{
  switch (status)
  {
  case PhocoenaClock::StartStatus::Disabled:
    return "disabled";
  case PhocoenaClock::StartStatus::Active:
    return "active";
  case PhocoenaClock::StartStatus::Failed:
    return "unavailable";
  }
  return "unknown";
}

void LogStartReport(const PhocoenaClock::StartReport& report)
{
  Log("Phocoena clocks", "status=%s mechanism=%s external_manager=%s detail=%s",
      StatusName(report.status), report.mechanism.c_str(), report.external_manager.c_str(),
      report.detail.c_str());
  if (report.status == PhocoenaClock::StartStatus::Disabled)
  {
    Log("Phocoena clocks", "phocoena_auto_clocks=0; zero CPU/GPU/MEM clock writes");
    return;
  }
  if (report.status == PhocoenaClock::StartStatus::Failed)
  {
    Log("Phocoena clocks", "request unavailable; emulation continues under existing policy; MEM writes=0");
    return;
  }
  Log("Phocoena CPU", "before_hz=%u before_valid=%d requested_hz=%u selected_hz=%u effective_hz=%u effective_valid=%d",
      report.before.cpu_hz, report.before.cpu_valid, report.requested_cpu_hz,
      report.selected_cpu_hz, report.effective.cpu_hz, report.effective.cpu_valid);
  Log("Phocoena GPU", "before_hz=%u before_valid=%d requested_hz=%u selected_hz=%u effective_hz=%u effective_valid=%d",
      report.before.gpu_hz, report.before.gpu_valid, report.requested_gpu_hz,
      report.selected_gpu_hz, report.effective.gpu_hz, report.effective.gpu_valid);
  Log("Phocoena MEM", "current_hz=%u valid=%d READ_ONLY=1 writes=0",
      report.effective.memory_valid ? report.effective.memory_hz : report.before.memory_hz,
      report.effective.memory_valid || report.before.memory_valid);
}
}

AutoClockSession::~AutoClockSession()
{
  Stop();
}

const PhocoenaClock::StartReport& AutoClockSession::Start(const bool enabled, const PhocoenaClock::Profile profile)
{
  const auto& report = m_session.Start(enabled, CreatePlatformBackend, profile);
  LogStartReport(report);
  return report;
}

void AutoClockSession::Stop()
{
  const PhocoenaClock::StopReport report = m_session.Stop();
  if (report.attempted)
  {
    Log("Phocoena clocks exit", "mechanism=%s release=%s detail=%s",
        report.mechanism.c_str(), report.succeeded ? "success" : "failed", report.detail.c_str());
  }
}
}

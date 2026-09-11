// SPDX-License-Identifier: GPL-2.0-or-later
// Physical USB GameCube adapters are not the libnx controller device.
#include "InputCommon/GCAdapter.h"
#include "InputCommon/GCPadStatus.h"
#include "Horizon/Log.h"
namespace GCAdapter
{
void Init() { Horizon::Log("USB", "physical GameCube adapter backend disabled; use Switch HID mapping"); }
void ResetRumble() {}
void Shutdown() {}
void SetAdapterCallback(std::function<void(void)>) {}
GCPadStatus Input(int) { return {}; }
void Output(int, u8) {}
bool IsDetected(const char** error_message)
{
  if (error_message) *error_message = "USB GameCube adapter passthrough is unavailable on Horizon v0.1";
  return false;
}
bool DeviceConnected(int) { return false; }
void ResetDeviceType(int) {}
double GetCurrentPollRate() { return 0; }
}

// SPDX-License-Identifier: GPL-2.0-or-later
#include "Core/HW/EXI/EXI_DeviceEthernet.h"
#include "Horizon/Log.h"
namespace ExpansionInterface
{
bool CEXIETHERNET::TAPNetworkInterface::Activate()
{
  Horizon::Error("BBA", "host TAP devices are unavailable on Horizon");
  return false;
}
void CEXIETHERNET::TAPNetworkInterface::Deactivate() {}
bool CEXIETHERNET::TAPNetworkInterface::IsActivated() { return false; }
bool CEXIETHERNET::TAPNetworkInterface::SendFrame(const u8*, u32) { return false; }
bool CEXIETHERNET::TAPNetworkInterface::RecvInit() { return false; }
void CEXIETHERNET::TAPNetworkInterface::RecvStart() {}
void CEXIETHERNET::TAPNetworkInterface::RecvStop() {}
}

// SPDX-License-Identifier: GPL-2.0-or-later
#include "Horizon/Input.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <fstream>
#include <filesystem>
#include "Common/FileUtil.h"
#include "Horizon/Log.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/ControllerInterface/CoreDevice.h"
namespace Horizon
{
namespace
{
std::array<std::atomic<double>, 24> states{};
class ControlInput final : public ciface::Core::Device::Input
{
public:
  ControlInput(const char* name, size_t index) : m_name(name), m_index(index) {}
  std::string GetName() const override { return m_name; }
  ControlState GetState() const override { return states[m_index].load(std::memory_order_relaxed); }
private:
  std::string m_name;
  size_t m_index;
};
constexpr std::array<const char*, 24> names = {"A", "B", "X", "Y", "L", "R", "ZL", "ZR",
  "Plus", "Minus", "Up", "Down", "Left", "Right", "LStick", "RStick",
  "LX-", "LX+", "LY-", "LY+", "RX-", "RX+", "RY-", "RY+"};
constexpr std::array<u64, 16> masks = {HidNpadButton_A, HidNpadButton_B, HidNpadButton_X,
  HidNpadButton_Y, HidNpadButton_L, HidNpadButton_R, HidNpadButton_ZL, HidNpadButton_ZR,
  HidNpadButton_Plus, HidNpadButton_Minus, HidNpadButton_Up, HidNpadButton_Down,
  HidNpadButton_Left, HidNpadButton_Right, HidNpadButton_StickL, HidNpadButton_StickR};
class Controller final : public ciface::Core::Device
{
public:
  Controller()
  {
    for (size_t i = 0; i < names.size(); ++i) AddInput(new ControlInput(names[i], i));
  }
  std::string GetName() const override { return "Controller"; }
  std::string GetSource() const override { return "Switch"; }
  std::optional<int> GetPreferredId() const override { return 0; }
};
}
void InstallController()
{
  g_controller_interface.AddDevice(std::make_shared<Controller>());
  Log("Input", "registered Switch/0/Controller (handheld, paired Joy-Con, Pro Controller)");
}
void UpdateController(const PadState& pad, u64 suppressed_buttons, bool suppress_sticks)
{
  const u64 held = padGetButtons(&pad) & ~suppressed_buttons;
  static u32 last_style = ~0u;
  if (last_style != padGetStyleSet(&pad))
  {
    last_style = padGetStyleSet(&pad);
    Log("Input", "connected=%d style=0x%08x", padIsConnected(&pad), last_style);
  }
  for (size_t i = 0; i < masks.size(); ++i) states[i] = (held & masks[i]) ? 1.0 : 0.0;
  const auto l = suppress_sticks ? HidAnalogStickState{} : padGetStickPos(&pad, 0);
  const auto r = suppress_sticks ? HidAnalogStickState{} : padGetStickPos(&pad, 1);
  const std::array<int, 4> axes = {l.x, l.y, r.x, r.y};
  for (size_t i = 0; i < axes.size(); ++i)
  {
    const double value = std::clamp(axes[i] / 32767.0, -1.0, 1.0);
    states[16 + i * 2] = std::max(0.0, -value);
    states[17 + i * 2] = std::max(0.0, value);
  }
}
bool WriteDefaultMapping()
{
  const auto path = File::GetUserPath(F_GCPADCONFIG_IDX);
  if (File::Exists(path)) return true;
  std::ofstream file(path);
  file << "[GCPad1]\nDevice = Switch/0/Controller\n"
    "Buttons/A = A\nButtons/B = B\nButtons/X = X\nButtons/Y = Y\n"
    "Buttons/Z = R\nButtons/Start = `Plus` | `Minus`\n"
    "Main Stick/Up = `LY+`\nMain Stick/Down = `LY-`\nMain Stick/Left = `LX-`\nMain Stick/Right = `LX+`\n"
    "Main Stick/Dead Zone = 15.0\n"
    "C-Stick/Up = `RY+`\nC-Stick/Down = `RY-`\nC-Stick/Left = `RX-`\nC-Stick/Right = `RX+`\n"
    "C-Stick/Dead Zone = 15.0\n"
    "Triggers/L = `ZL` | `L`\nTriggers/R = ZR\nTriggers/L-Analog = ZL\nTriggers/R-Analog = ZR\n"
    "D-Pad/Up = Up\nD-Pad/Down = Down\nD-Pad/Left = Left\nD-Pad/Right = Right\n";
  file.flush();
  return bool(file);
}
}

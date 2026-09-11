// Minimal host fake for the actual Horizon/Console.cpp handoff code.
// Not included by or shipped inside the Switch executable.
#pragma once
#include <cstdint>
using u32 = std::uint32_t;
using s32 = std::int32_t;
using Result = u32;
constexpr bool R_FAILED(Result result) { return result != 0; }
constexpr u32 R_VALUE(Result result) { return result & 0x3fffff; }
constexpr u32 R_MODULE(Result result) { return result & 0x1ff; }
constexpr u32 R_DESCRIPTION(Result result) { return (result >> 9) & 0x1fff; }
constexpr u32 Module_Libnx = 345, Module_LibnxBinder = 349;
constexpr u32 LibnxError_BadInput = 11, LibnxBinderError_WouldBlock = 14;
constexpr Result MAKERESULT(u32 module, u32 description) { return module | (description << 9); }
constexpr u32 PIXEL_FORMAT_RGB_565 = 4;
struct NvMultiFence {};
struct BqGraphicBuffer {};
struct Framebuffer {};
struct Service {};
struct Binder { s32 id = 0; };
struct Mutex { bool locked = false; };
void mutexLock(Mutex*);
void mutexUnlock(Mutex*);
struct NWindow
{
  bool valid = false;
  bool is_connected = false;
  std::uint64_t slots_configured = 0;
  std::uint64_t slots_requested = 0;
  s32 cur_slot = -1;
  u32 width = 0, height = 0, format = ~0u, usage = 0;
  Binder bq;
  Mutex mutex;
  u32 swap_interval = 1;
  bool producer_controlled_by_app = false;
};
struct PrintConsole { bool consoleInitialised = false; };
NWindow* nwindowGetDefault();
bool nwindowIsValid(NWindow*);
void nwindowClose(NWindow*);
Result nwindowCreate(NWindow*, Service*, s32, bool);
Result nwindowSetSwapInterval(NWindow*, u32);
Result nwindowCancelBuffer(NWindow*, s32, const NvMultiFence*);
Service* viGetSession_IHOSBinderDriverRelay();
PrintConsole* consoleInit(PrintConsole*);
void consoleExit(PrintConsole*);
void consoleUpdate(PrintConsole*);

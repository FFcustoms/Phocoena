// SPDX-License-Identifier: GPL-2.0-or-later
#include "Common/Thread.h"
#include <switch.h>
#include "Horizon/Log.h"
#include "Horizon/Performance.h"

namespace Horizon
{
ThreadUsage ReadCurrentThreadUsage()
{
  ThreadUsage sample;
  u64 id = 0, ticks = 0;
  Result result = svcGetThreadId(&id, CUR_THREAD_HANDLE);
  if (R_SUCCEEDED(result))
  {
    const InfoType type = hosversionAtLeast(13, 0, 0) ? InfoType_ThreadTickCount :
                                                    InfoType_ThreadTickCountDeprecated;
    result = svcGetInfo(&ticks, type, CUR_THREAD_HANDLE, TickCountInfo_Total);
  }
  sample.id = id;
  sample.cpu_ns = R_SUCCEEDED(result) ? armTicksToNs(ticks) : 0;
  sample.wall_ns = armTicksToNs(armGetSystemTick());
  sample.core = svcGetCurrentProcessorNumber();
  sample.result = result;
  sample.valid = R_SUCCEEDED(result);
  return sample;
}
}
namespace Common
{
int CurrentThreadId()
{
  u64 id = 0;
  svcGetThreadId(&id, CUR_THREAD_HANDLE);
  return static_cast<int>(id);
}
void SetThreadAffinity(std::thread::native_handle_type, u32)
{
  // libnx's pthread handle is not a Horizon kernel handle. The scheduler owns
  // placement; never reinterpret_cast a pthread_t or move onto a reserved core.
}
void SetCurrentThreadAffinity(u32) {}
void SleepCurrentThread(int ms) { svcSleepThread(static_cast<s64>(ms) * 1000000); }
void SwitchCurrentThread() { svcSleepThread(1000000); }
void SetCurrentThreadName(const char* name)
{
  s32 preferred = -1;
  u64 mask = 0;
  const Result result = svcGetThreadCoreMask(&preferred, &mask, CUR_THREAD_HANDLE);
  // Pinned libnx newlib already assigns the process's permitted core mask to
  // pthreads. Observe it; do not guess a core ID or claim an affinity change.
  Horizon::Log("Thread", "id=%d name=%s core=%u preferred=%d allowed_mask=0x%llx query=0x%08x",
      CurrentThreadId(), name, svcGetCurrentProcessorNumber(), preferred,
      static_cast<unsigned long long>(mask), result);
}
std::tuple<void*, size_t> GetCurrentThreadStack()
{
  auto* thread = threadGetSelf();
  if (thread) return {thread->stack_mem, thread->stack_sz};
  // libnx does not give its initial thread a Thread object.
  MemoryInfo info{};
  u32 page_info = 0;
  if (R_SUCCEEDED(svcQueryMemory(&info, &page_info, reinterpret_cast<u64>(&info))))
    return {reinterpret_cast<void*>(info.addr), static_cast<size_t>(info.size)};
  return {nullptr, 0};
}
}

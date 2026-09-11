// SPDX-License-Identifier: GPL-2.0-or-later
// Actual Horizon registry and upstream emitter with a Linux dual-mmap libnx
// fixture. RX is not writable, RW is not executable. AArch64 runs also execute
// the production emitter self-test under QEMU, not the full Dolphin PPC core.
#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>
#include <sys/mman.h>
#include <unistd.h>
#include <switch.h>
#include "Common/Arm64Emitter.h"
#include "Common/MsgHandler.h"
#include "Horizon/JitMemory.h"
#include "Horizon/JitTest.h"

namespace
{
bool fail_create = false, fail_close = false, unsupported = false;
unsigned creates = 0, closes = 0;
unsigned live_objects = 0, peak_live_objects = 0;
unsigned max_live_objects = std::numeric_limits<unsigned>::max();
void* last_rw_flush = nullptr;
std::size_t last_size = 0;
unsigned flushes = 0;
}
Result jitCreate(Jit* jit, std::size_t size)
{
  if (fail_create || live_objects >= max_live_objects) return 0xce01;
  const int fd = memfd_create("dolphin-jit-alias-test", 0);
  assert(fd >= 0 && ftruncate(fd, size) == 0);
  jit->rw_addr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  jit->rx_addr = mmap(nullptr, size, PROT_READ | PROT_EXEC, MAP_SHARED, fd, 0);
  assert(jit->rw_addr != MAP_FAILED && jit->rx_addr != MAP_FAILED);
  jit->type = unsupported ? JitType_SetProcessMemoryPermission : JitType_CodeMemory;
  jit->size = size; jit->handle = fd; ++creates; ++live_objects;
  peak_live_objects = std::max(peak_live_objects, live_objects);
  return 0;
}
Result jitClose(Jit* jit)
{
  if (fail_close) return 456;
  assert(munmap(jit->rw_addr, jit->size) == 0);
  assert(munmap(jit->rx_addr, jit->size) == 0);
  close(jit->handle); ++closes; --live_objects;
  return 0;
}
void armDCacheFlush(void* address, std::size_t size)
{
  assert(!last_rw_flush);
  last_rw_flush = address; last_size = size;
  __builtin___clear_cache(static_cast<char*>(address), static_cast<char*>(address) + size);
}
void armICacheInvalidate(void* address, std::size_t size)
{
  assert(last_rw_flush && address != last_rw_flush && size == last_size);
  assert(std::memcmp(last_rw_flush, address, size) == 0);
  last_rw_flush = nullptr; ++flushes;
  __builtin___clear_cache(static_cast<char*>(address), static_cast<char*>(address) + size);
}
namespace Horizon
{
void Log(const char*, const char*, ...) {}
void Error(const char*, const char*, ...) {}
}
namespace Common
{
void* AllocateExecutableMemory(std::size_t size) { return Horizon::AllocateJitMemory(size); }
bool FreeMemoryPages(void* ptr, std::size_t size)
{
  return Horizon::FreeJitMemory(ptr, size) == Horizon::JitFreeResult::Released;
}
bool MsgAlertFmtImpl(bool, MsgType, Log::LogType, const char*, int,
                     fmt::string_view, const fmt::format_args&)
{
  throw std::runtime_error("Unexpected emitter assertion");
}
}
template<class Exception, class Function> void MustThrow(Function function)
{
  bool caught = false;
  try { function(); } catch (const Exception&) { caught = true; }
  assert(caught);
}
int main()
{
  using namespace Arm64Gen;
  using enum ARM64Reg;
  using Horizon::JitFreeResult;
  fail_create = true;
  MustThrow<std::bad_alloc>([] { Horizon::AllocateJitMemory(4096); });
  fail_create = false; unsupported = true;
  MustThrow<std::bad_alloc>([] { Horizon::AllocateJitMemory(4096); });
  assert(creates == closes);
  unsupported = false;
  MustThrow<std::bad_alloc>([] { Horizon::AllocateJitMemory(0); });
  MustThrow<std::bad_alloc>([] { Horizon::AllocateJitMemory(std::numeric_limits<std::size_t>::max()); });
  {
    ARM64CodeBlock child, code;
    code.AllocCodeSpace(8192); code.AddChildCodeSpace(&child, 4096);
    auto* rx = code.GetWritableCodePtr(); // mutable cursor, still RX
    auto* rw = static_cast<u8*>(Common::GetWritableJitAddress(rx, 4));
    assert(rx != rw);
    code.MOVI2R(W0, 42); code.RET();
    u32 instruction;
    std::memcpy(&instruction, rx, 4); assert(instruction == 0x52800540);
    auto branch = code.B(); code.NOP(); code.SetJumpTarget(branch);
    std::memcpy(&instruction, branch.ptr, 4); assert(instruction == 0x14000002);
    const auto* cross = code.GetCodePtr(); code.B(child.GetCodePtr());
    std::memcpy(&instruction, cross, 4);
    assert((instruction & 0x03ffffff) == static_cast<u32>((child.GetCodePtr() - cross) / 4));
    code.FlushIcache(); assert(flushes > 0);
    ARM64XEmitter patch(rx, rx + 4); patch.MOVI2R(W0, 43); patch.FlushIcache();
    std::memcpy(&instruction, rx, 4); assert(instruction == 0x52800560);
    auto* literal = code.GetWritableCodePtr(); code.ReserveCodeSpace(sizeof(void*));
    const auto* child_ptr = child.GetCodePtr();
    std::memcpy(Common::GetWritableJitAddress(literal, sizeof(void*)), &child_ptr, sizeof(void*));
    const u8* loaded; std::memcpy(&loaded, literal, sizeof(loaded)); assert(loaded == child_ptr);
    MustThrow<std::runtime_error>([&] { Common::GetWritableJitAddress(rx + 8190, 4); });
    MustThrow<std::runtime_error>([&] { Common::GetWritableJitAddress(rw, 4); });
    assert(Horizon::FreeJitMemory(rx + 4, 8192) == JitFreeResult::Failed);
    assert(Horizon::FreeJitMemory(rx, 4096) == JitFreeResult::Failed);
    code.ClearCodeSpace(); std::memcpy(&instruction, rx, 4); assert(instruction == 0xd4200000);
    code.SetCodePtr(rx, rx + 3); code.NOP(); assert(code.HasWriteFailed());
  }
  // Reproduce Horizon's CodeMemory-object limit. The old one-object-per-format
  // implementation failed around ten live vertex formats; 2,048 logical 4 KiB
  // programs now fit in two arenas even when the kernel fixture allows only two
  // simultaneous objects.
  max_live_objects = 2;
  const unsigned creates_before_pool = creates;
  std::vector<void*> slots(Horizon::MAX_JIT_ALLOCATIONS);
  for (auto& slot : slots)
    slot = Horizon::AllocateJitMemory(4096);
  MustThrow<std::bad_alloc>([] { Horizon::AllocateJitMemory(4096); });
  auto stats = Horizon::ReadJitMemoryStats();
  assert(stats.active == Horizon::MAX_JIT_ALLOCATIONS);
  assert(stats.peak_active == Horizon::MAX_JIT_ALLOCATIONS);
  assert(stats.pooled_pages_active == Horizon::MAX_JIT_ALLOCATIONS);
  assert(stats.kernel_regions_active == 2 && live_objects == 2);
  assert(creates - creates_before_pool == 2);

  // Free alternating pages and refill them to prove fragmentation does not
  // force another kernel object or alias overlapping live code.
  for (std::size_t i = 0; i < slots.size(); i += 2)
  {
    assert(Horizon::FreeJitMemory(slots[i], 4096) == JitFreeResult::Released);
    slots[i] = nullptr;
  }
  const unsigned creates_before_refill = creates;
  for (std::size_t i = 0; i < slots.size(); i += 2)
    slots[i] = Horizon::AllocateJitMemory(4096);
  assert(creates == creates_before_refill);

  // Wrong-size/interior frees are rejected without releasing a neighbor.
  assert(Horizon::FreeJitMemory(static_cast<unsigned char*>(slots[0]) + 4, 4096) ==
         JitFreeResult::Failed);
  assert(Horizon::FreeJitMemory(slots[0], 8192) == JitFreeResult::Failed);

  for (auto slot : slots)
    assert(Horizon::FreeJitMemory(slot, 4096) == JitFreeResult::Released);
  stats = Horizon::ReadJitMemoryStats();
  assert(stats.active == 0 && stats.kernel_regions_active == 0 && live_objects == 0);

  // The final allocation in an arena is retained if the kernel refuses close,
  // then releases cleanly when retrying.
  void* retained = Horizon::AllocateJitMemory(4096);
  fail_close = true;
  assert(Horizon::FreeJitMemory(retained, 4096) == JitFreeResult::Failed);
  assert(Horizon::ReadJitMemoryStats().active == 1 && live_objects == 1);
  fail_close = false;
  assert(Horizon::FreeJitMemory(retained, 4096) == JitFreeResult::Released);

  // The PPC cache remains a dedicated object and coexists with both small-code
  // pools under a realistic three-object kernel limit.
  max_live_objects = 3;
  void* ppc = Horizon::AllocateJitMemory(Horizon::SMALL_JIT_MAX_ALLOCATION + 4096);
  std::vector<void*> formats(1500);
  for (auto& format : formats)
    format = Horizon::AllocateJitMemory(4096);
  stats = Horizon::ReadJitMemoryStats();
  assert(stats.active == formats.size() + 1 && stats.kernel_regions_active == 3);
  for (auto format : formats)
    assert(Horizon::FreeJitMemory(format, 4096) == JitFreeResult::Released);
  assert(Horizon::FreeJitMemory(ppc, Horizon::SMALL_JIT_MAX_ALLOCATION + 4096) ==
         JitFreeResult::Released);

  // The development A/B restores the old dedicated policy only between boots.
  Horizon::SetSmallJitPoolingEnabled(false);
  assert(!Horizon::IsSmallJitPoolingEnabled());
  max_live_objects = 2;
  void* legacy_a = Horizon::AllocateJitMemory(4096);
  void* legacy_b = Horizon::AllocateJitMemory(4096);
  MustThrow<std::bad_alloc>([] { Horizon::AllocateJitMemory(4096); });
  Horizon::SetSmallJitPoolingEnabled(true); // rejected while allocations live
  assert(!Horizon::IsSmallJitPoolingEnabled());
  assert(Horizon::FreeJitMemory(legacy_a, 4096) == JitFreeResult::Released);
  assert(Horizon::FreeJitMemory(legacy_b, 4096) == JitFreeResult::Released);
  Horizon::SetSmallJitPoolingEnabled(true);
  assert(Horizon::IsSmallJitPoolingEnabled());
#if !defined(__aarch64__)
  struct BenchResult { std::uint64_t ns; unsigned kernel_creates; };
  const auto benchmark = [](bool pooled) {
    constexpr std::size_t format_count = 512;
    Horizon::SetSmallJitPoolingEnabled(pooled);
    const unsigned initial_creates = creates;
    const auto started = std::chrono::steady_clock::now();
    std::vector<void*> formats(format_count);
    for (auto& format : formats)
      format = Horizon::AllocateJitMemory(4096);
    for (auto format : formats)
      assert(Horizon::FreeJitMemory(format, 4096) == JitFreeResult::Released);
    return BenchResult{
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       std::chrono::steady_clock::now() - started)
                                       .count()),
        creates - initial_creates};
  };
  std::array<BenchResult, 5> legacy_bench{}, pooled_bench{};
  max_live_objects = std::numeric_limits<unsigned>::max();
  for (std::size_t i = 0; i < legacy_bench.size(); ++i)
  {
    legacy_bench[i] = benchmark(false);
    pooled_bench[i] = benchmark(true);
    assert(legacy_bench[i].kernel_creates == 512 && pooled_bench[i].kernel_creates == 1);
  }
  const auto median_ns = [](auto results) {
    std::sort(results.begin(), results.end(),
              [](const BenchResult& left, const BenchResult& right) { return left.ns < right.ns; });
    return results[results.size() / 2].ns;
  };
  const auto legacy_ns = median_ns(legacy_bench);
  const auto pooled_ns = median_ns(pooled_bench);
  std::cout << "JIT_POOL_BENCH formats=512 legacy_kernel_creates=512 pooled_kernel_creates=1 "
            << "legacy_ms=" << legacy_ns / 1e6 << " pooled_ms=" << pooled_ns / 1e6
            << " speedup=" << static_cast<double>(legacy_ns) / pooled_ns << '\n';
  Horizon::SetSmallJitPoolingEnabled(true);
#endif
#if defined(__aarch64__)
  assert(Horizon::RunJitEmitterSelfTest());
  std::cout << "PASS: executed actual Dolphin emitter self-test ARM64 functions (not full PPC core)\n";
#endif
  assert(creates == closes && live_objects == 0 && !last_rw_flush);
  std::cout << "JIT_POOL_RESULT logical_formats=" << Horizon::MAX_JIT_ALLOCATIONS
            << " kernel_objects=2 old_kernel_objects=" << Horizon::MAX_JIT_ALLOCATIONS
            << " object_reduction_pct=99.9023 peak_fixture_objects=" << peak_live_objects << '\n';
  std::cout << "PASS: RX/RW aliases, actual emitter writes/fixups, pointer table, patch, poison, bounds, pooled allocation/refill, kernel-object cap, retained close failure and dedicated-cache coexistence\n";
}

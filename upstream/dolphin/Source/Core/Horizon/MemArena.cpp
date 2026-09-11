// SPDX-License-Identifier: GPL-2.0-or-later
#include "Common/MemArena.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>

#include <malloc.h>
#include <switch.h>

#include "Common/MemoryUtil.h"
#include "Common/ScopeGuard.h"
#include "Horizon/Fastmem.h"
#include "Horizon/Log.h"

namespace Common
{
namespace
{
constexpr size_t HORIZON_PAGE_SIZE = 0x1000;
constexpr size_t HORIZON_ARENA_ALIGNMENT = 0x200000;
constexpr unsigned SVC_SET_MEMORY_PERMISSION = 0x02;
constexpr unsigned SVC_SET_PROCESS_MEMORY_PERMISSION = 0x73;
constexpr unsigned SVC_MAP_PROCESS_MEMORY = 0x74;
constexpr unsigned SVC_UNMAP_PROCESS_MEMORY = 0x75;
constexpr unsigned SVC_MAP_PROCESS_CODE_MEMORY = 0x77;
constexpr unsigned SVC_UNMAP_PROCESS_CODE_MEMORY = 0x78;

struct FastmemSupport
{
  bool arena = false;
  bool read_only_mappings = false;
};

size_t AlignUp(size_t value, size_t alignment)
{
  if (value > std::numeric_limits<size_t>::max() - (alignment - 1))
    throw std::bad_alloc();
  return (value + alignment - 1) & ~(alignment - 1);
}

FastmemSupport DetectFastmemSupport()
{
  FastmemSupport support;
  constexpr std::array arena_syscalls = {
      SVC_SET_PROCESS_MEMORY_PERMISSION, SVC_MAP_PROCESS_MEMORY,
      SVC_UNMAP_PROCESS_MEMORY,          SVC_MAP_PROCESS_CODE_MEMORY,
      SVC_UNMAP_PROCESS_CODE_MEMORY,
  };
  if (!std::ranges::all_of(arena_syscalls, envIsSyscallHinted))
    return support;

  const Handle self = envGetOwnProcessHandle();
  if (self == INVALID_HANDLE)
    return support;

  void* const backing = memalign(HORIZON_PAGE_SIZE, HORIZON_PAGE_SIZE);
  if (!backing)
    return support;
  ScopeGuard backing_guard([backing] { free(backing); });

  virtmemLock();
  void* const canonical = virtmemFindCodeMemory(HORIZON_PAGE_SIZE, HORIZON_PAGE_SIZE);
  VirtmemReservation* const canonical_reservation =
      canonical ? virtmemAddReservation(canonical, HORIZON_PAGE_SIZE) : nullptr;
  virtmemUnlock();
  if (!canonical_reservation)
    return support;
  ScopeGuard canonical_reservation_guard([canonical_reservation] {
    virtmemLock();
    virtmemRemoveReservation(canonical_reservation);
    virtmemUnlock();
  });

  Result result = svcMapProcessCodeMemory(self, reinterpret_cast<u64>(canonical),
                                          reinterpret_cast<u64>(backing), HORIZON_PAGE_SIZE);
  if (R_FAILED(result))
    return support;
  ScopeGuard canonical_mapping_guard([self, canonical, backing] {
    svcUnmapProcessCodeMemory(self, reinterpret_cast<u64>(canonical),
                              reinterpret_cast<u64>(backing), HORIZON_PAGE_SIZE);
  });

  result = svcSetProcessMemoryPermission(self, reinterpret_cast<u64>(canonical),
                                         HORIZON_PAGE_SIZE, Perm_Rw);
  if (R_FAILED(result))
    return support;

  virtmemLock();
  void* const alias = virtmemFindAslr(HORIZON_PAGE_SIZE, HORIZON_PAGE_SIZE);
  VirtmemReservation* const alias_reservation =
      alias ? virtmemAddReservation(alias, HORIZON_PAGE_SIZE) : nullptr;
  virtmemUnlock();
  if (!alias_reservation)
    return support;
  ScopeGuard alias_reservation_guard([alias_reservation] {
    virtmemLock();
    virtmemRemoveReservation(alias_reservation);
    virtmemUnlock();
  });

  result = svcMapProcessMemory(alias, self, reinterpret_cast<u64>(canonical), HORIZON_PAGE_SIZE);
  if (R_FAILED(result))
    return support;
  ScopeGuard alias_mapping_guard([alias, self, canonical] {
    svcUnmapProcessMemory(alias, self, reinterpret_cast<u64>(canonical), HORIZON_PAGE_SIZE);
  });

  constexpr u32 pattern = 0x464D454D;
  *static_cast<volatile u32*>(alias) = pattern;
  if (*static_cast<volatile u32*>(canonical) != pattern)
    return support;
  *static_cast<volatile u32*>(canonical) = 0;
  support.arena = true;

  if (envIsSyscallHinted(SVC_SET_MEMORY_PERMISSION) &&
      R_SUCCEEDED(svcSetMemoryPermission(alias, HORIZON_PAGE_SIZE, Perm_R)) &&
      R_SUCCEEDED(svcSetMemoryPermission(alias, HORIZON_PAGE_SIZE, Perm_Rw)))
  {
    support.read_only_mappings = true;
  }
  return support;
}

const FastmemSupport& GetFastmemSupport()
{
  static const FastmemSupport support = [] {
    const FastmemSupport detected = DetectFastmemSupport();
    Horizon::SetPpcFastmemCapabilities(detected.arena, detected.read_only_mappings);
    Horizon::Log("Fastmem probe", "arena=%s read_only_mappings=%s",
                 detected.arena ? "supported" : "unsupported",
                 detected.read_only_mappings ? "supported" : "unsupported");
    return detected;
  }();
  return support;
}
}  // namespace

MemArena::MemArena() = default;

MemArena::~MemArena()
{
  ReleaseSHMSegment();
}

void MemArena::GrabSHMSegment(size_t size, std::string_view name)
{
  ReleaseSHMSegment();
  if (m_shm_buffer)
  {
    Horizon::Error("Fastmem", "previous RAM mappings could not be released safely");
    throw std::bad_alloc();
  }

  const bool requested = Horizon::IsPpcFastmemRequested();
  const FastmemSupport support = requested ? GetFastmemSupport() : FastmemSupport{};
  if (requested && support.arena)
  {
    const size_t aligned_size = AlignUp(size, HORIZON_ARENA_ALIGNMENT);
    m_shm_buffer = AllocateAlignedMemory(aligned_size, HORIZON_ARENA_ALIGNMENT);
    if (m_shm_buffer)
    {
      std::memset(m_shm_buffer, 0, aligned_size);
      m_shm_size = aligned_size;
      m_fastmem_backing = true;

      virtmemLock();
      m_rw_mirror = virtmemFindCodeMemory(aligned_size, HORIZON_ARENA_ALIGNMENT);
      Result result = m_rw_mirror ?
                          svcMapProcessCodeMemory(envGetOwnProcessHandle(),
                                                  reinterpret_cast<u64>(m_rw_mirror),
                                                  reinterpret_cast<u64>(m_shm_buffer), aligned_size) :
                          MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
      if (R_SUCCEEDED(result))
      {
        result = svcSetProcessMemoryPermission(envGetOwnProcessHandle(),
                                               reinterpret_cast<u64>(m_rw_mirror), aligned_size,
                                               Perm_Rw);
      }
      if (R_FAILED(result) && m_rw_mirror)
      {
        svcUnmapProcessCodeMemory(envGetOwnProcessHandle(),
                                  reinterpret_cast<u64>(m_rw_mirror),
                                  reinterpret_cast<u64>(m_shm_buffer), aligned_size);
      }
      virtmemUnlock();

      if (R_SUCCEEDED(result))
      {
        Horizon::Log("RAM", "allocated %zu bytes for %.*s; fastmem canonical=%p backing=%p",
                     aligned_size, static_cast<int>(name.size()), name.data(), m_rw_mirror,
                     m_shm_buffer);
        return;
      }

      Horizon::Error("Fastmem", "canonical RAM alias failed: 0x%08x; using checked path", result);
      m_rw_mirror = nullptr;
    }
  }

  if (!m_shm_buffer)
  {
    m_shm_buffer = AllocateMemoryPages(size);
    m_shm_size = m_shm_buffer ? size : 0;
    m_fastmem_backing = false;
  }
  if (!m_shm_buffer)
  {
    Horizon::Error("RAM", "failed allocating %zu bytes for %.*s", size,
                   static_cast<int>(name.size()), name.data());
    throw std::bad_alloc();
  }
  Horizon::Log("RAM", "allocated %zu bytes; direct MEM1/MEM2 views; fastmem unavailable",
               m_shm_size);
}

void MemArena::ReleaseSHMSegment()
{
  ReleaseMemoryRegion();
  if (!m_fastmem_mappings.empty())
  {
    // Retain the canonical mapping and backing allocation rather than freeing
    // memory which the kernel still aliases. The process will reclaim this on
    // exit, and a subsequent arena creation fails closed above.
    Horizon::Error("Fastmem", "%zu RAM aliases remain live; retaining backing memory",
                   m_fastmem_mappings.size());
    return;
  }
  if (m_rw_mirror)
  {
    virtmemLock();
    const Result result = svcUnmapProcessCodeMemory(
        envGetOwnProcessHandle(), reinterpret_cast<u64>(m_rw_mirror),
        reinterpret_cast<u64>(m_shm_buffer), m_shm_size);
    virtmemUnlock();
    if (R_FAILED(result))
      Horizon::Error("Fastmem", "canonical RAM unmap failed: 0x%08x", result);
    m_rw_mirror = nullptr;
  }
  if (m_shm_buffer)
  {
    if (m_fastmem_backing)
      FreeAlignedMemory(m_shm_buffer);
    else
      FreeMemoryPages(m_shm_buffer, m_shm_size);
  }
  m_shm_buffer = nullptr;
  m_shm_size = 0;
  m_fastmem_backing = false;
}

void* MemArena::CreateView(s64 offset, size_t size)
{
  if (!m_shm_buffer || offset < 0 || static_cast<size_t>(offset) > m_shm_size ||
      size > m_shm_size - static_cast<size_t>(offset))
  {
    Horizon::Error("RAM", "invalid view offset=%lld size=%zu", static_cast<long long>(offset),
                   size);
    return nullptr;
  }
  void* const base = m_rw_mirror ? m_rw_mirror : m_shm_buffer;
  return static_cast<u8*>(base) + offset;
}

void MemArena::ReleaseView(void*, size_t)
{
}

u8* MemArena::ReserveMemoryRegion(size_t size)
{
  if (!m_rw_mirror || !Horizon::IsPpcFastmemArenaSupported())
    return nullptr;

  const size_t aligned_size = AlignUp(size, HORIZON_ARENA_ALIGNMENT);
  virtmemLock();
  m_reserved_region = virtmemFindAslr(aligned_size, HORIZON_ARENA_ALIGNMENT);
  m_reservation = m_reserved_region ? virtmemAddReservation(m_reserved_region, aligned_size) :
                                     nullptr;
  if (!m_reservation)
    m_reserved_region = nullptr;
  virtmemUnlock();

  if (!m_reserved_region)
  {
    Horizon::Error("Fastmem", "failed reserving %zu-byte virtual arena", aligned_size);
    return nullptr;
  }
  m_reserved_region_size = aligned_size;
  Horizon::Log("Fastmem", "reserved %zu-byte AArch64 arena at %p", aligned_size,
               m_reserved_region);
  return static_cast<u8*>(m_reserved_region);
}

void MemArena::ReleaseMemoryRegion()
{
  for (size_t index = m_fastmem_mappings.size(); index-- > 0;)
  {
    const auto& mapping = m_fastmem_mappings[index];
    const Result result = svcUnmapProcessMemory(
        std::get<0>(mapping), envGetOwnProcessHandle(),
        reinterpret_cast<u64>(m_rw_mirror) + static_cast<u64>(std::get<1>(mapping)),
        std::get<2>(mapping));
    Horizon::RecordPpcFastmemMapping(false, R_SUCCEEDED(result));
    if (R_SUCCEEDED(result))
      m_fastmem_mappings.erase(m_fastmem_mappings.begin() + index);
    else
      Horizon::Error("Fastmem", "RAM alias unmap failed: 0x%08x", result);
  }

  if (m_reservation && m_fastmem_mappings.empty())
  {
    virtmemLock();
    virtmemRemoveReservation(static_cast<VirtmemReservation*>(m_reservation));
    virtmemUnlock();
  }
  if (m_fastmem_mappings.empty())
  {
    m_reservation = nullptr;
    m_reserved_region = nullptr;
    m_reserved_region_size = 0;
  }
  Horizon::SetPpcFastmemArenaActive(false);
}

void* MemArena::MapInMemoryRegion(s64 offset, size_t size, void* base, bool writeable)
{
  if (!m_reserved_region || !m_rw_mirror || offset < 0 ||
      static_cast<size_t>(offset) > m_shm_size || size > m_shm_size - static_cast<size_t>(offset) ||
      reinterpret_cast<uintptr_t>(base) % HORIZON_PAGE_SIZE != 0 ||
      static_cast<size_t>(offset) % HORIZON_PAGE_SIZE != 0 || size % HORIZON_PAGE_SIZE != 0)
  {
    Horizon::RecordPpcFastmemMapping(true, false);
    return nullptr;
  }
  const auto arena_begin = reinterpret_cast<uintptr_t>(m_reserved_region);
  const auto mapping_begin = reinterpret_cast<uintptr_t>(base);
  if (mapping_begin < arena_begin || mapping_begin - arena_begin > m_reserved_region_size ||
      size > m_reserved_region_size - (mapping_begin - arena_begin) ||
      (!writeable && !Horizon::ArePpcFastmemReadOnlyMappingsSupported()))
  {
    Horizon::RecordPpcFastmemMapping(true, false);
    return nullptr;
  }

  Result result = svcMapProcessMemory(base, envGetOwnProcessHandle(),
                                      reinterpret_cast<u64>(m_rw_mirror) + offset, size);
  if (R_SUCCEEDED(result) && !writeable)
    result = svcSetMemoryPermission(base, size, Perm_R);
  if (R_FAILED(result))
  {
    svcUnmapProcessMemory(base, envGetOwnProcessHandle(),
                          reinterpret_cast<u64>(m_rw_mirror) + offset, size);
    Horizon::RecordPpcFastmemMapping(true, false);
    return nullptr;
  }

  try
  {
    m_fastmem_mappings.emplace_back(base, offset, size);
  }
  catch (...)
  {
    svcUnmapProcessMemory(base, envGetOwnProcessHandle(),
                          reinterpret_cast<u64>(m_rw_mirror) + offset, size);
    Horizon::RecordPpcFastmemMapping(true, false);
    return nullptr;
  }
  Horizon::RecordPpcFastmemMapping(true, true);
  return base;
}

bool MemArena::ChangeMappingProtection(void* view, size_t size, bool writeable)
{
  if (!view || size == 0)
    return true;
  if (!Horizon::ArePpcFastmemReadOnlyMappingsSupported())
    return writeable;
  return R_SUCCEEDED(svcSetMemoryPermission(view, size, writeable ? Perm_Rw : Perm_R));
}

void MemArena::UnmapFromMemoryRegion(void* view, size_t size)
{
  const auto it = std::find_if(m_fastmem_mappings.begin(), m_fastmem_mappings.end(),
                               [view](const auto& mapping) {
                                 return std::get<0>(mapping) == view;
                               });
  if (it == m_fastmem_mappings.end())
  {
    Horizon::RecordPpcFastmemMapping(false, false);
    return;
  }
  if (std::get<2>(*it) != size)
  {
    Horizon::RecordPpcFastmemMapping(false, false);
    return;
  }
  const Result result = svcUnmapProcessMemory(
      view, envGetOwnProcessHandle(),
      reinterpret_cast<u64>(m_rw_mirror) + static_cast<u64>(std::get<1>(*it)),
      std::get<2>(*it));
  Horizon::RecordPpcFastmemMapping(false, R_SUCCEEDED(result));
  if (R_SUCCEEDED(result))
    m_fastmem_mappings.erase(it);
}

size_t MemArena::GetPageSize() const
{
  return HORIZON_PAGE_SIZE;
}

LazyMemoryRegion::LazyMemoryRegion() = default;
LazyMemoryRegion::~LazyMemoryRegion()
{
  Release();
}

void* LazyMemoryRegion::Create(size_t size)
{
  if (size > 64 * 1024 * 1024)
    return nullptr;
  m_memory = AllocateMemoryPages(size);
  m_size = m_memory ? size : 0;
  return m_memory;
}

void LazyMemoryRegion::Clear()
{
  if (m_memory)
    std::memset(m_memory, 0, m_size);
}

void LazyMemoryRegion::Release()
{
  FreeMemoryPages(m_memory, m_size);
  m_memory = nullptr;
  m_size = 0;
}
}  // namespace Common

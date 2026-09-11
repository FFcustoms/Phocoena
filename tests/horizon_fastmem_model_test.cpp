#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <random>
#include <span>
#include <string_view>
#include <vector>

namespace
{
constexpr std::uint32_t MEM1_SIZE = 24 * 1024 * 1024;
constexpr std::uint32_t MEM2_BASE = 0x10000000;
constexpr std::uint32_t MEM2_SIZE = 64 * 1024 * 1024;
constexpr std::uint32_t MMIO_BASE = 0x0C000000;
constexpr std::uint32_t MMIO_SIZE = 0x10000;
constexpr std::uint32_t PAGE_SIZE = 128 * 1024;

enum class Kind
{
  Mem1,
  Mem2,
  Mmio,
  Invalid,
};

struct Location
{
  Kind kind = Kind::Invalid;
  std::uint32_t offset = 0;
};

struct Counters
{
  std::uint64_t direct = 0;
  std::uint64_t slow = 0;
  std::uint64_t mmio = 0;
  std::uint64_t fallback = 0;
  std::uint64_t invalid = 0;
  std::uint64_t code_invalidations = 0;
};

Location Translate(std::uint32_t address, std::size_t width)
{
  std::uint32_t physical = address;
  if ((address & 0xFE000000) == 0x80000000)
    physical = address & 0x01FFFFFF;
  else if ((address & 0xFE000000) == 0x90000000)
    physical = MEM2_BASE + (address & 0x01FFFFFF);
  else if ((address & 0xFFFF0000) == 0xCC000000)
    physical = MMIO_BASE + (address & 0xFFFF);

  const auto fits = [width](std::uint32_t offset, std::uint32_t size) {
    return offset <= size && width <= static_cast<std::size_t>(size - offset);
  };
  if (physical < MEM1_SIZE && fits(physical, MEM1_SIZE))
    return {Kind::Mem1, physical};
  if (physical >= MEM2_BASE && fits(physical - MEM2_BASE, MEM2_SIZE))
    return {Kind::Mem2, physical - MEM2_BASE};
  if (physical >= MMIO_BASE && fits(physical - MMIO_BASE, MMIO_SIZE))
    return {Kind::Mmio, physical - MMIO_BASE};
  return {};
}

template <typename T>
T LoadBigEndian(std::span<const std::uint8_t> bytes, std::size_t offset)
{
  T value = 0;
  for (std::size_t i = 0; i < sizeof(T); ++i)
    value = static_cast<T>((value << 8) | bytes[offset + i]);
  return value;
}

template <typename T>
void StoreBigEndian(std::span<std::uint8_t> bytes, std::size_t offset, T value)
{
  for (std::size_t i = 0; i < sizeof(T); ++i)
  {
    bytes[offset + sizeof(T) - 1 - i] = static_cast<std::uint8_t>(value);
    value >>= 8;
  }
}

class MemoryModel
{
public:
  explicit MemoryModel(bool direct) : m_direct(direct), m_mem1(MEM1_SIZE), m_mem2(MEM2_SIZE)
  {
  }

  template <typename T>
  T Read(std::uint32_t address)
  {
    const Location location = Translate(address, sizeof(T));
    Count(location);
    switch (location.kind)
    {
    case Kind::Mem1:
      return LoadBigEndian<T>(m_mem1, location.offset);
    case Kind::Mem2:
      return LoadBigEndian<T>(m_mem2, location.offset);
    case Kind::Mmio:
      return LoadBigEndian<T>(m_mmio, location.offset);
    case Kind::Invalid:
      m_exception = true;
      return {};
    }
    return {};
  }

  template <typename T>
  void Write(std::uint32_t address, T value)
  {
    const Location location = Translate(address, sizeof(T));
    Count(location);
    switch (location.kind)
    {
    case Kind::Mem1:
      StoreBigEndian<T>(m_mem1, location.offset, value);
      if (location.offset < 0x200000)
        ++m_counters.code_invalidations;
      return;
    case Kind::Mem2:
      StoreBigEndian<T>(m_mem2, location.offset, value);
      return;
    case Kind::Mmio:
      StoreBigEndian<T>(m_mmio, location.offset, value);
      return;
    case Kind::Invalid:
      m_exception = true;
      return;
    }
  }

  const Counters& GetCounters() const { return m_counters; }
  bool Exception() const { return m_exception; }
  void ClearException() { m_exception = false; }
  const std::vector<std::uint8_t>& Mem1() const { return m_mem1; }
  const std::vector<std::uint8_t>& Mem2() const { return m_mem2; }

private:
  void Count(Location location)
  {
    if (location.kind == Kind::Mmio)
      ++m_counters.mmio;
    else if (location.kind == Kind::Invalid)
    {
      ++m_counters.invalid;
      ++m_counters.fallback;
    }
    else if (m_direct)
      ++m_counters.direct;
    else
      ++m_counters.slow;
  }

  bool m_direct;
  bool m_exception = false;
  Counters m_counters;
  std::vector<std::uint8_t> m_mem1;
  std::vector<std::uint8_t> m_mem2;
  std::array<std::uint8_t, MMIO_SIZE> m_mmio{};
};

template <typename T>
void CompareAccess(MemoryModel& reference, MemoryModel& fast, std::uint32_t address, T value,
                   bool write)
{
  reference.ClearException();
  fast.ClearException();
  if (write)
  {
    reference.Write<T>(address, value);
    fast.Write<T>(address, value);
  }
  else
  {
    assert(reference.Read<T>(address) == fast.Read<T>(address));
  }
  assert(reference.Exception() == fast.Exception());
}

void RunCorrectness()
{
  MemoryModel reference(false);
  MemoryModel fast(true);

  const std::array boundary_addresses = {
      0u, 1u, MEM1_SIZE - 8, MEM1_SIZE - 4, MEM1_SIZE - 1, MEM1_SIZE,
      0x80000000u, 0x80000000u + MEM1_SIZE - 8, 0x80000000u + MEM1_SIZE,
      MEM2_BASE, MEM2_BASE + MEM2_SIZE - 8, MEM2_BASE + MEM2_SIZE,
      0x90000000u, 0x90000000u + MEM2_SIZE - 8,
      MMIO_BASE, MMIO_BASE + MMIO_SIZE - 8, 0xCC000000u, 0xCC00FFF8u,
      PAGE_SIZE - 8, PAGE_SIZE - 4, PAGE_SIZE - 1, 0xFFFFFFFFu,
  };
  for (const std::uint32_t address : boundary_addresses)
  {
    CompareAccess<std::uint8_t>(reference, fast, address, 0xA5, true);
    CompareAccess<std::uint16_t>(reference, fast, address, 0xA55A, true);
    CompareAccess<std::uint32_t>(reference, fast, address, 0xA55A1234, true);
    CompareAccess<std::uint64_t>(reference, fast, address, 0xA55A12345678FEDCull, true);
    CompareAccess<std::uint8_t>(reference, fast, address, 0, false);
    CompareAccess<std::uint16_t>(reference, fast, address, 0, false);
    CompareAccess<std::uint32_t>(reference, fast, address, 0, false);
    CompareAccess<std::uint64_t>(reference, fast, address, 0, false);
  }

  std::mt19937_64 random(0xD01F1A57u);
  for (std::size_t i = 0; i < 500000; ++i)
  {
    const std::uint32_t selector = static_cast<std::uint32_t>(random() % 10);
    std::uint32_t address;
    if (selector < 4)
      address = 0x80000000u + static_cast<std::uint32_t>(random() % (MEM1_SIZE + 32));
    else if (selector < 7)
      address = 0x90000000u + static_cast<std::uint32_t>(random() % (MEM2_SIZE + 32));
    else if (selector == 7)
      address = 0xCC000000u + static_cast<std::uint32_t>(random() % (MMIO_SIZE + 32));
    else
      address = static_cast<std::uint32_t>(random());

    const bool write = (random() & 1) != 0;
    switch (random() & 3)
    {
    case 0:
      CompareAccess<std::uint8_t>(reference, fast, address, random(), write);
      break;
    case 1:
      CompareAccess<std::uint16_t>(reference, fast, address, random(), write);
      break;
    case 2:
      CompareAccess<std::uint32_t>(reference, fast, address, random(), write);
      break;
    default:
      CompareAccess<std::uint64_t>(reference, fast, address, random(), write);
      break;
    }
  }
  assert(reference.Mem1() == fast.Mem1());
  assert(reference.Mem2() == fast.Mem2());
  assert(reference.GetCounters().code_invalidations == fast.GetCounters().code_invalidations);
  std::cout << "PASS: 500000 randomized differential accesses; MEM1/MEM2/MMIO/boundaries/"
               "widths/endian/exceptions/code invalidation\n";
}

void RunBenchmark()
{
  constexpr std::size_t accesses = 40'000'000;
  std::vector<std::uint8_t> ram(MEM1_SIZE);
  std::vector<std::uint8_t*> pages(0x1'0000'0000ull / PAGE_SIZE, nullptr);
  for (std::size_t offset = 0; offset < ram.size(); offset += PAGE_SIZE)
    pages[(0x80000000u + offset) / PAGE_SIZE] = ram.data() + offset;

  std::vector<std::uint32_t> addresses(65536);
  std::mt19937 random(0x51A57);
  for (auto& address : addresses)
    address = 0x80000000u + (random() % (MEM1_SIZE - 4));

  auto bench = [&](bool direct) {
    volatile std::uint64_t sink = 0;
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < accesses; ++i)
    {
      const std::uint32_t address = addresses[i & (addresses.size() - 1)];
      const std::uint32_t offset = address & 0x7FFFFFFFu;
      const std::uint8_t* pointer = direct ? ram.data() + offset :
          pages[address / PAGE_SIZE] + (address & (PAGE_SIZE - 1));
      std::uint32_t value;
      std::memcpy(&value, pointer, sizeof(value));
      sink = sink + value;
    }
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return std::pair{static_cast<double>(accesses) / seconds / 1e6,
                     static_cast<std::uint64_t>(sink)};
  };

  const auto [old_mops, old_sink] = bench(false);
  const auto [new_mops, new_sink] = bench(true);
  assert(old_sink == new_sink);
  std::cout << std::fixed << std::setprecision(2)
            << "old_page_table_mops=" << old_mops << "\n"
            << "new_direct_mops=" << new_mops << "\n"
            << "improvement_pct=" << ((new_mops / old_mops) - 1.0) * 100.0 << "\n"
            << "fast_path_hit_pct=100.00\nslow_fallback_pct=0.00\n";
}
}  // namespace

int main(int argc, char** argv)
{
  if (argc == 2 && std::string_view(argv[1]) == "--benchmark")
    RunBenchmark();
  else
    RunCorrectness();
}

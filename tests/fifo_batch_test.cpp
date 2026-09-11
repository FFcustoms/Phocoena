// SPDX-License-Identifier: GPL-2.0-or-later
#include "VideoCommon/FifoBatch.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string_view>
#include <vector>

namespace
{
using Fifo::PlanFifoBatch;
using Fifo::EffectiveFifoBatchBlocks;
using Fifo::NormalizeFifoBatchBlocks;

void TestPlans()
{
  constexpr u32 gather = 32;
  constexpr u32 base = 0x1000;
  constexpr u32 end = 0x1fe0;

  static_assert(Fifo::HORIZON_FIFO_LEGACY_BATCH_BLOCKS == 8);
  static_assert(Fifo::HORIZON_FIFO_DEFAULT_BATCH_BLOCKS == 8);
  static_assert(Fifo::HORIZON_FIFO_MAX_BATCH_BLOCKS == 8);
  static_assert(NormalizeFifoBatchBlocks(0) == 1);
  static_assert(NormalizeFifoBatchBlocks(3) == 1);
  static_assert(NormalizeFifoBatchBlocks(7) == 4);
  static_assert(NormalizeFifoBatchBlocks(15) == 8);
  static_assert(NormalizeFifoBatchBlocks(99) == 8);
  static_assert(EffectiveFifoBatchBlocks(16, false) == 8);
  static_assert(EffectiveFifoBatchBlocks(16, true) == 1);

  const auto full = PlanFifoBatch(base, base, end, 32 * gather, false, 0, gather, 8);
  assert(full.blocks == 8 && full.bytes == 8 * gather && full.next_read_ptr == base + 8 * gather);

  const auto full16 = PlanFifoBatch(base, base, end, 32 * gather, false, 0, gather, 16);
  assert(full16.blocks == 16 && full16.bytes == 16 * gather &&
         full16.next_read_ptr == base + 16 * gather);

  const auto short_distance = PlanFifoBatch(base, base, end, 3 * gather, false, 0, gather, 8);
  assert(short_distance.blocks == 3 && short_distance.next_read_ptr == base + 3 * gather);

  const auto before_wrap = PlanFifoBatch(end - 2 * gather, base, end, 8 * gather, false, 0,
                                         gather, 8);
  assert(before_wrap.blocks == 3 && before_wrap.next_read_ptr == base);

  const auto before_break = PlanFifoBatch(base, base, end, 8 * gather, true, base + 3 * gather,
                                          gather, 8);
  assert(before_break.blocks == 3 && before_break.next_read_ptr == base + 3 * gather);

  // A breakpoint behind the current pointer does not constrain this contiguous burst.
  const auto past_break = PlanFifoBatch(base + 8 * gather, base, end, 8 * gather, true,
                                        base + 2 * gather, gather, 8);
  assert(past_break.blocks == 8);

  // Invalid inputs conservatively retain the one-block behavior.
  for (const auto plan : {PlanFifoBatch(base, base, end, 0, false, 0, gather, 8),
                          PlanFifoBatch(end + gather, base, end, gather, false, 0, gather, 8),
                          PlanFifoBatch(base, base, end, gather, false, 0, gather, 0)})
  {
    assert(plan.blocks == 1 && plan.bytes == gather);
  }
  const auto zero_gather = PlanFifoBatch(base, base, end, gather, false, 0, 0, 8);
  assert(zero_gather.blocks == 1 && zero_gather.bytes == 0);

  // Exhaustively verify the invariants for every aligned read pointer, distance,
  // batch cap, and forward breakpoint in this ring segment.
  for (u32 read = base; read <= end; read += gather)
  {
    for (u32 distance_blocks = 1; distance_blocks <= 128; ++distance_blocks)
    {
      for (u32 cap = 1; cap <= 16; ++cap)
      {
        for (u32 breakpoint = read + gather; breakpoint <= end + gather;
             breakpoint += gather)
        {
          const bool breakpoint_enabled = breakpoint <= end;
          const auto plan = PlanFifoBatch(read, base, end, distance_blocks * gather,
                                          breakpoint_enabled, breakpoint, gather, cap);
          assert(plan.blocks >= 1 && plan.blocks <= cap && plan.blocks <= distance_blocks);
          assert(plan.bytes == plan.blocks * gather);
          assert(read + (plan.blocks - 1) * gather <= end);
          if (breakpoint_enabled)
            assert(read + plan.blocks * gather <= breakpoint);
          const u32 last = read + (plan.blocks - 1) * gather;
          assert(plan.next_read_ptr == (last == end ? base : last + gather));
        }
      }
    }
  }
}

struct ConsumeResult
{
  std::uint64_t hash = 0xcbf29ce484222325ULL;
  u32 consumed = 0;
  u32 read_ptr = 0;
};

ConsumeResult ConsumeModel(u32 ring_blocks, u32 read_block, u32 distance_blocks,
                           bool breakpoint_enabled, u32 breakpoint_block, u32 cap)
{
  constexpr u32 gather = 32;
  const u32 base = 0x4000;
  const u32 end = base + (ring_blocks - 1) * gather;
  u32 read = base + read_block * gather;
  const u32 breakpoint = base + breakpoint_block * gather;
  ConsumeResult result{};
  while (distance_blocks != 0 && (!breakpoint_enabled || read != breakpoint))
  {
    const auto plan = PlanFifoBatch(read, base, end, distance_blocks * gather,
                                    breakpoint_enabled, breakpoint, gather, cap);
    for (u32 block = 0; block < plan.blocks; ++block)
    {
      const u32 ring_index = (read - base) / gather + block;
      assert(ring_index < ring_blocks);
      result.hash ^= static_cast<std::uint64_t>(ring_index) | (result.consumed << 16);
      result.hash *= 0x100000001b3ULL;
      ++result.consumed;
    }
    assert(plan.blocks <= distance_blocks);
    distance_blocks -= plan.blocks;
    read = plan.next_read_ptr;
  }
  result.read_ptr = read;
  return result;
}

void TestRandomizedStreamEquivalence()
{
  // Deterministic xorshift: 100,000 ring/wrap/distance/breakpoint cases for
  // every supported cap, compared byte-order-for-byte-order to cap one.
  std::uint32_t random = 0x7a11c0de;
  const auto next = [&random] {
    random ^= random << 13;
    random ^= random >> 17;
    random ^= random << 5;
    return random;
  };
  constexpr std::array<u32, 4> caps{1, 4, 8, 16};
  for (u32 iteration = 0; iteration < 100000; ++iteration)
  {
    const u32 ring_blocks = 1 + next() % 64;
    const u32 read_block = next() % ring_blocks;
    const u32 distance_blocks = 1 + next() % (ring_blocks * 3);
    const u32 breakpoint_block = next() % ring_blocks;
    const bool breakpoint_enabled = (next() & 1) != 0;
    const auto reference = ConsumeModel(ring_blocks, read_block, distance_blocks,
                                        breakpoint_enabled, breakpoint_block, 1);
    for (const u32 cap : caps)
    {
      const auto candidate = ConsumeModel(ring_blocks, read_block, distance_blocks,
                                          breakpoint_enabled, breakpoint_block, cap);
      assert(candidate.hash == reference.hash);
      assert(candidate.consumed == reference.consumed);
      assert(candidate.read_ptr == reference.read_ptr);
    }
  }
}

struct Result
{
  std::uint64_t ns = 0;
  std::uint64_t checksum = 0;
  std::uint64_t decoder_calls = 0;
};

Result RunBookkeepingBenchmark(u32 total_blocks, u32 batch_blocks)
{
  std::atomic<u32> distance{total_blocks * 32};
  std::atomic<u32> read_ptr{0};
  std::uint64_t checksum = 0;
  std::uint64_t decoder_calls = 0;
  const auto started = std::chrono::steady_clock::now();
  for (u32 first = 0; first < total_blocks; first += batch_blocks)
  {
    const u32 blocks = std::min(batch_blocks, total_blocks - first);
    for (u32 i = 0; i < blocks; ++i)
      checksum += (static_cast<std::uint64_t>(first + i) * 0x9e3779b1u) ^ 0x85ebca6bu;
    read_ptr.store(first + blocks, std::memory_order_relaxed);
    distance.fetch_sub(blocks * 32, std::memory_order_seq_cst);
    ++decoder_calls;
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::steady_clock::now() - started)
                           .count();
  assert(distance.load() == 0 && read_ptr.load() == total_blocks);
  return {static_cast<std::uint64_t>(elapsed), checksum, decoder_calls};
}

void Benchmark()
{
  constexpr u32 blocks = 8 * 1024 * 1024;
  constexpr std::array<u32, 4> caps{1, 4, 8, 16};
  std::array<std::array<Result, 7>, caps.size()> results{};
  for (std::size_t run = 0; run < results.front().size(); ++run)
  {
    for (std::size_t cap = 0; cap < caps.size(); ++cap)
    {
      results[cap][run] = RunBookkeepingBenchmark(blocks, caps[cap]);
      assert(results[cap][run].checksum == results[0][run].checksum);
      assert(results[cap][run].decoder_calls == blocks / caps[cap]);
    }
  }
  const auto median = [](const auto& results) {
    std::array<std::uint64_t, results.size()> times{};
    std::transform(results.begin(), results.end(), times.begin(), [](const Result& r) {
      return r.ns;
    });
    std::sort(times.begin(), times.end());
    return times[times.size() / 2];
  };
  const auto baseline_ns = median(results[0]);
  for (std::size_t cap = 0; cap < caps.size(); ++cap)
  {
    const auto elapsed_ns = median(results[cap]);
    std::cout << "FIFO_BATCH_BENCH blocks=" << blocks << " cap=" << caps[cap]
              << " decoder_calls=" << results[cap].front().decoder_calls
              << " median_ms=" << elapsed_ns / 1e6 << " speedup_vs_1="
              << static_cast<double>(baseline_ns) / elapsed_ns << '\n';
  }
}
}  // namespace

int main(int argc, char** argv)
{
  TestPlans();
  TestRandomizedStreamEquivalence();
  if (argc > 1 && std::string_view(argv[1]) == "--benchmark")
    Benchmark();
  std::cout << "PASS: FIFO batches preserve ring, distance, wrap and breakpoint boundaries\n";
}

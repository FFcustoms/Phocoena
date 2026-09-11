// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>

#include "Common/CommonTypes.h"

namespace Fifo
{
// Horizon's graphics thread was spending a large amount of CPU time consuming one
// 32-byte gather-pipe block per decoder invocation. Keep the burst bounded so CP
// status changes and asynchronous requests remain responsive. The 16-block
// candidate was never hardware-validated and F-Zero GX exposed guest GP watchdog
// resets with it, so v0.1.26 returns to the proven eight-block ceiling.
inline constexpr u32 HORIZON_FIFO_LEGACY_BATCH_BLOCKS = 8;
inline constexpr u32 HORIZON_FIFO_DEFAULT_BATCH_BLOCKS = 8;
inline constexpr u32 HORIZON_FIFO_MAX_BATCH_BLOCKS = 8;

// Only the tested power-of-two development modes are exposed. Values from hand-
// edited configurations select the nearest supported value at or below the request.
constexpr u32 NormalizeFifoBatchBlocks(u32 blocks)
{
  if (blocks >= HORIZON_FIFO_MAX_BATCH_BLOCKS)
    return HORIZON_FIFO_MAX_BATCH_BLOCKS;
  if (blocks >= HORIZON_FIFO_LEGACY_BATCH_BLOCKS)
    return HORIZON_FIFO_LEGACY_BATCH_BLOCKS;
  if (blocks >= 4)
    return 4;
  return 1;
}

// SyncGPU depends on the original gather-block timing granularity.
constexpr u32 EffectiveFifoBatchBlocks(u32 configured_blocks, bool sync_gpu)
{
  return sync_gpu ? 1 : NormalizeFifoBatchBlocks(configured_blocks);
}

struct FifoBatchPlan
{
  u32 blocks = 1;
  u32 bytes = 32;
  u32 next_read_ptr = 0;
};

// Plan a batch that never crosses the FIFO ring end or an enabled breakpoint.
// The caller has already established that at least one gather block is readable
// and that read_ptr itself is not the active breakpoint.
constexpr FifoBatchPlan PlanFifoBatch(u32 read_ptr, u32 base, u32 end, u32 distance,
                                      bool breakpoint_enabled, u32 breakpoint,
                                      u32 gather_size, u32 max_blocks)
{
  FifoBatchPlan plan{1, gather_size, read_ptr};
  if (gather_size == 0 || max_blocks == 0 || distance < gather_size || read_ptr < base ||
      read_ptr > end || (end - read_ptr) % gather_size != 0)
  {
    plan.next_read_ptr = read_ptr == end ? base : read_ptr + gather_size;
    return plan;
  }

  const u32 available_blocks = distance / gather_size;
  const u32 blocks_before_wrap = (end - read_ptr) / gather_size + 1;
  plan.blocks = std::max(1u, std::min({max_blocks, available_blocks, blocks_before_wrap}));

  if (breakpoint_enabled && breakpoint > read_ptr && breakpoint <= end &&
      (breakpoint - read_ptr) % gather_size == 0)
  {
    const u32 blocks_before_breakpoint = (breakpoint - read_ptr) / gather_size;
    plan.blocks = std::max(1u, std::min(plan.blocks, blocks_before_breakpoint));
  }

  plan.bytes = plan.blocks * gather_size;
  const u32 last_read_ptr = read_ptr + (plan.blocks - 1) * gather_size;
  plan.next_read_ptr = last_read_ptr == end ? base : last_read_ptr + gather_size;
  return plan;
}
}  // namespace Fifo

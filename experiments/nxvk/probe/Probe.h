// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Returns only after Vulkan resources have been destroyed. Throws a specific
// std::runtime_error on failure. The host test omits Horizon presentation.
void RunVulkanProbe(bool present);

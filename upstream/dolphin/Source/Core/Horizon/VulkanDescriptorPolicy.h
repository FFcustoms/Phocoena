// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

namespace Horizon
{
inline constexpr std::uint32_t GX_PUSH_DESCRIPTOR_COUNT = 16;

enum class VulkanPushDescriptorDecision
{
  DisabledBySetting,
  MissingExtension,
  MissingPropertiesQuery,
  InsufficientLimit,
  Enabled,
};

constexpr VulkanPushDescriptorDecision SelectVulkanPushDescriptorMode(
    bool requested, bool extension_available, bool properties_query_available,
    std::uint32_t max_push_descriptors)
{
  if (!requested)
    return VulkanPushDescriptorDecision::DisabledBySetting;
  if (!extension_available)
    return VulkanPushDescriptorDecision::MissingExtension;
  if (!properties_query_available)
    return VulkanPushDescriptorDecision::MissingPropertiesQuery;
  if (max_push_descriptors < GX_PUSH_DESCRIPTOR_COUNT)
    return VulkanPushDescriptorDecision::InsufficientLimit;
  return VulkanPushDescriptorDecision::Enabled;
}
}

// SPDX-License-Identifier: GPL-2.0-or-later
// Test-only handle provider. Vulkan calls and CommandBufferManager are real;
// instance/device/window creation are outside this isolated manager test.
#pragma once
#include <memory>
#include "VideoBackends/Vulkan/Constants.h"
namespace Vulkan
{
struct VulkanContext
{
  VkDevice device{};
  VkQueue queue{};
  u32 family{};
  VmaAllocator allocator{};
  VkDevice GetDevice() const { return device; }
  VkQueue GetGraphicsQueue() const { return queue; }
  VkQueue GetPresentQueue() const { return queue; }
  u32 GetGraphicsQueueFamilyIndex() const { return family; }
  VmaAllocator GetMemoryAllocator() const { return allocator; }
};
extern std::unique_ptr<VulkanContext> g_vulkan_context;
}

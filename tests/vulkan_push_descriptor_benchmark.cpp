// SPDX-License-Identifier: GPL-2.0-or-later
// Host-only Vulkan descriptor API benchmark. It uses 16 uniform-buffer
// descriptors as a portable stand-in for Dolphin's 16 GX combined samplers;
// this measures descriptor allocation/update/recording overhead, not Switch
// gameplay and not texture sampling.
#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

#include <vulkan/vulkan.h>

namespace
{
constexpr std::uint32_t OPERATIONS = 4096;
constexpr std::uint32_t DESCRIPTORS = 16;
constexpr std::uint32_t ROUNDS = 11;

std::uint32_t FindMemoryType(VkPhysicalDevice physical, std::uint32_t bits)
{
  VkPhysicalDeviceMemoryProperties properties{};
  vkGetPhysicalDeviceMemoryProperties(physical, &properties);
  for (std::uint32_t i = 0; i < properties.memoryTypeCount; ++i)
  {
    if (bits & (1u << i))
      return i;
  }
  std::abort();
}

double Median(std::vector<double> values)
{
  std::sort(values.begin(), values.end());
  return values[values.size() / 2];
}

std::vector<double> Run(VkDevice device, std::uint32_t family, VkDescriptorPool pool,
                        VkDescriptorSetLayout set_layout, VkPipelineLayout layout,
                        VkBuffer buffer, bool push,
                        PFN_vkCmdPushDescriptorSetKHR cmd_push)
{
  VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pool_info.queueFamilyIndex = family;
  VkCommandPool command_pool{};
  assert(vkCreateCommandPool(device, &pool_info, nullptr, &command_pool) == VK_SUCCESS);
  VkCommandBufferAllocateInfo allocate_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  allocate_info.commandPool = command_pool;
  allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocate_info.commandBufferCount = 1;
  VkCommandBuffer command{};
  assert(vkAllocateCommandBuffers(device, &allocate_info, &command) == VK_SUCCESS);

  std::array<VkDescriptorBufferInfo, DESCRIPTORS> buffers{};
  for (auto& info : buffers)
    info = {buffer, 0, 256};
  std::vector<double> samples;
  for (std::uint32_t round = 0; round < ROUNDS; ++round)
  {
    assert(vkResetCommandPool(device, command_pool, 0) == VK_SUCCESS);
    assert(vkResetDescriptorPool(device, pool, 0) == VK_SUCCESS);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    assert(vkBeginCommandBuffer(command, &begin) == VK_SUCCESS);
    const auto started = std::chrono::steady_clock::now();
    for (std::uint32_t i = 0; i < OPERATIONS; ++i)
    {
      VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      write.dstBinding = 0;
      write.descriptorCount = DESCRIPTORS;
      write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      write.pBufferInfo = buffers.data();
      if (push)
      {
        cmd_push(command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &write);
      }
      else
      {
        VkDescriptorSetAllocateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        set_info.descriptorPool = pool;
        set_info.descriptorSetCount = 1;
        set_info.pSetLayouts = &set_layout;
        VkDescriptorSet set{};
        assert(vkAllocateDescriptorSets(device, &set_info, &set) == VK_SUCCESS);
        write.dstSet = set;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0,
                                nullptr);
      }
    }
    const auto stopped = std::chrono::steady_clock::now();
    assert(vkEndCommandBuffer(command) == VK_SUCCESS);
    if (round != 0)
    {
      samples.push_back(
          std::chrono::duration<double, std::micro>(stopped - started).count() / OPERATIONS);
    }
  }
  vkDestroyCommandPool(device, command_pool, nullptr);
  return samples;
}
}

int main()
{
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  instance_info.pApplicationInfo = &app;
  VkInstance instance{};
  assert(vkCreateInstance(&instance_info, nullptr, &instance) == VK_SUCCESS);
  std::uint32_t count = 0;
  assert(vkEnumeratePhysicalDevices(instance, &count, nullptr) == VK_SUCCESS && count > 0);
  std::vector<VkPhysicalDevice> physical_devices(count);
  assert(vkEnumeratePhysicalDevices(instance, &count, physical_devices.data()) == VK_SUCCESS);
  const VkPhysicalDevice physical = physical_devices.front();

  assert(vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr) == VK_SUCCESS);
  std::vector<VkExtensionProperties> extensions(count);
  assert(vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, extensions.data()) ==
         VK_SUCCESS);
  assert(std::ranges::any_of(extensions, [](const auto& extension) {
    return std::strcmp(extension.extensionName, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME) == 0;
  }));
  VkPhysicalDevicePushDescriptorPropertiesKHR push_properties{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR};
  VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
  properties.pNext = &push_properties;
  vkGetPhysicalDeviceProperties2(physical, &properties);
  assert(push_properties.maxPushDescriptors >= DESCRIPTORS);

  vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
  std::vector<VkQueueFamilyProperties> families(count);
  vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
  std::uint32_t family = 0;
  while (family < count && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT))
    ++family;
  assert(family < count);
  const float priority = 1.0f;
  VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  queue_info.queueFamilyIndex = family;
  queue_info.queueCount = 1;
  queue_info.pQueuePriorities = &priority;
  const char* extension = VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME;
  VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  device_info.queueCreateInfoCount = 1;
  device_info.pQueueCreateInfos = &queue_info;
  device_info.enabledExtensionCount = 1;
  device_info.ppEnabledExtensionNames = &extension;
  VkDevice device{};
  assert(vkCreateDevice(physical, &device_info, nullptr, &device) == VK_SUCCESS);
  const auto cmd_push = reinterpret_cast<PFN_vkCmdPushDescriptorSetKHR>(
      vkGetDeviceProcAddr(device, "vkCmdPushDescriptorSetKHR"));
  assert(cmd_push);

  VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  buffer_info.size = 256;
  buffer_info.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
  VkBuffer buffer{};
  assert(vkCreateBuffer(device, &buffer_info, nullptr, &buffer) == VK_SUCCESS);
  VkMemoryRequirements requirements{};
  vkGetBufferMemoryRequirements(device, buffer, &requirements);
  VkMemoryAllocateInfo memory_info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  memory_info.allocationSize = requirements.size;
  memory_info.memoryTypeIndex = FindMemoryType(physical, requirements.memoryTypeBits);
  VkDeviceMemory memory{};
  assert(vkAllocateMemory(device, &memory_info, nullptr, &memory) == VK_SUCCESS);
  assert(vkBindBufferMemory(device, buffer, memory, 0) == VK_SUCCESS);

  VkDescriptorSetLayoutBinding binding{};
  binding.binding = 0;
  binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  binding.descriptorCount = DESCRIPTORS;
  binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  VkDescriptorSetLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  layout_info.bindingCount = 1;
  layout_info.pBindings = &binding;
  VkDescriptorSetLayout legacy_layout{};
  assert(vkCreateDescriptorSetLayout(device, &layout_info, nullptr, &legacy_layout) == VK_SUCCESS);
  VkDescriptorSetLayout push_layout{};
  layout_info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
  assert(vkCreateDescriptorSetLayout(device, &layout_info, nullptr, &push_layout) == VK_SUCCESS);
  VkPipelineLayoutCreateInfo pipeline_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pipeline_info.setLayoutCount = 1;
  pipeline_info.pSetLayouts = &legacy_layout;
  VkPipelineLayout legacy_pipeline{};
  assert(vkCreatePipelineLayout(device, &pipeline_info, nullptr, &legacy_pipeline) == VK_SUCCESS);
  pipeline_info.pSetLayouts = &push_layout;
  VkPipelineLayout push_pipeline{};
  assert(vkCreatePipelineLayout(device, &pipeline_info, nullptr, &push_pipeline) == VK_SUCCESS);
  VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, OPERATIONS * DESCRIPTORS};
  VkDescriptorPoolCreateInfo descriptor_pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  descriptor_pool_info.maxSets = OPERATIONS;
  descriptor_pool_info.poolSizeCount = 1;
  descriptor_pool_info.pPoolSizes = &pool_size;
  VkDescriptorPool descriptor_pool{};
  assert(vkCreateDescriptorPool(device, &descriptor_pool_info, nullptr, &descriptor_pool) ==
         VK_SUCCESS);

  const auto legacy = Run(device, family, descriptor_pool, legacy_layout, legacy_pipeline, buffer,
                          false, cmd_push);
  const auto pushed = Run(device, family, descriptor_pool, push_layout, push_pipeline, buffer, true,
                          cmd_push);
  const double legacy_median = Median(legacy);
  const double push_median = Median(pushed);
  std::printf("legacy_median_us_per_change=%.6f push_median_us_per_change=%.6f "
              "reduction_percent=%.2f operations_per_round=%u rounds=%u "
              "legacy_allocations=%u legacy_updates=%u legacy_binds=%u push_writes=%u\n",
              legacy_median, push_median, 100.0 * (legacy_median - push_median) / legacy_median,
              OPERATIONS, ROUNDS - 1, OPERATIONS, OPERATIONS, OPERATIONS, OPERATIONS);

  vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
  vkDestroyPipelineLayout(device, push_pipeline, nullptr);
  vkDestroyPipelineLayout(device, legacy_pipeline, nullptr);
  vkDestroyDescriptorSetLayout(device, push_layout, nullptr);
  vkDestroyDescriptorSetLayout(device, legacy_layout, nullptr);
  vkDestroyBuffer(device, buffer, nullptr);
  vkFreeMemory(device, memory, nullptr);
  vkDestroyDevice(device, nullptr);
  vkDestroyInstance(instance, nullptr);
}

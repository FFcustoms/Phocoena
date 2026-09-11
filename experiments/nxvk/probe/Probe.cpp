// SPDX-License-Identifier: GPL-2.0-or-later
#include "Probe.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef __SWITCH__
#include <switch.h>
#endif
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_VI_NN
#include <vulkan/vulkan.h>
#include "Horizon/Log.h"
#include "triangle_vert.h"
#include "triangle_frag.h"

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance, const char*);

namespace
{
using Horizon::Log;
constexpr uint32_t DIM = 256;
constexpr uint64_t WAIT_NS = 5'000'000'000;

void Require(bool condition, const char* message)
{
  if (!condition)
    throw std::runtime_error(message);
}
void Check(VkResult result, const char* stage)
{
  if (result != VK_SUCCESS)
    throw std::runtime_error(std::string(stage) + ": VkResult=" + std::to_string(result));
}

#define INSTANCE_FUNCTIONS(X) \
  X(DestroyInstance) X(EnumeratePhysicalDevices) X(GetPhysicalDeviceProperties) \
  X(GetPhysicalDeviceMemoryProperties) X(GetPhysicalDeviceFeatures) \
  X(GetPhysicalDeviceQueueFamilyProperties) X(EnumerateDeviceExtensionProperties) \
  X(CreateDevice) X(GetDeviceProcAddr)
#define WSI_FUNCTIONS(X) \
  X(CreateViSurfaceNN) X(DestroySurfaceKHR) X(GetPhysicalDeviceSurfaceCapabilitiesKHR) \
  X(GetPhysicalDeviceSurfaceFormatsKHR) X(GetPhysicalDeviceSurfaceSupportKHR)
#define DEVICE_FUNCTIONS(X) \
  X(DestroyDevice) X(DeviceWaitIdle) X(GetDeviceQueue) X(CreateImage) X(DestroyImage) \
  X(GetImageMemoryRequirements) X(AllocateMemory) X(FreeMemory) X(BindImageMemory) \
  X(CreateImageView) X(DestroyImageView) X(CreateRenderPass) X(DestroyRenderPass) \
  X(CreateFramebuffer) X(DestroyFramebuffer) X(CreateShaderModule) X(DestroyShaderModule) \
  X(CreatePipelineLayout) X(DestroyPipelineLayout) X(CreateGraphicsPipelines) X(DestroyPipeline) \
  X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) X(BindBufferMemory) \
  X(MapMemory) X(UnmapMemory) X(CreateCommandPool) X(DestroyCommandPool) X(AllocateCommandBuffers) \
  X(ResetCommandBuffer) X(BeginCommandBuffer) X(EndCommandBuffer) X(CmdBeginRenderPass) \
  X(CmdEndRenderPass) X(CmdBindPipeline) X(CmdSetViewport) X(CmdSetScissor) X(CmdDraw) \
  X(CmdPipelineBarrier) X(CmdCopyImageToBuffer) X(CreateFence) X(DestroyFence) \
  X(WaitForFences) X(ResetFences) X(QueueSubmit) X(CreateSemaphore) X(DestroySemaphore)
#define SWAP_FUNCTIONS(X) \
  X(CreateSwapchainKHR) X(DestroySwapchainKHR) X(GetSwapchainImagesKHR) \
  X(AcquireNextImageKHR) X(QueuePresentKHR)

class Probe
{
public:
  ~Probe() { Cleanup(); }
  void Initialize(bool wsi);
  void Readback();
  void Present();
  void Finish();

private:
#define DECLARE(name) PFN_vk##name name = nullptr;
  INSTANCE_FUNCTIONS(DECLARE)
  WSI_FUNCTIONS(DECLARE)
  DEVICE_FUNCTIONS(DECLARE)
  SWAP_FUNCTIONS(DECLARE)
#undef DECLARE
  VkInstance instance{};
  VkPhysicalDevice physical{};
  VkDevice device{};
  VkQueue queue{};
  uint32_t family = UINT32_MAX;
  VkPhysicalDeviceMemoryProperties memory{};
  VkImage image{};
  VkDeviceMemory image_memory{}, buffer_memory{};
  VkImageView image_view{};
  VkFramebuffer framebuffer{};
  VkBuffer buffer{};
  void* mapped{};
  VkRenderPass render_pass{}, swap_render_pass{};
  VkShaderModule vertex{}, fragment{};
  VkPipelineLayout layout{};
  VkPipeline pipeline{}, swap_pipeline{};
  VkCommandPool pool{};
  VkCommandBuffer command{};
  VkFence fence{};
  VkSurfaceKHR surface{};
  VkSwapchainKHR swapchain{};
  std::vector<VkImage> swap_images;
  std::vector<VkImageView> swap_views;
  std::vector<VkFramebuffer> swap_frames;
  std::vector<VkSemaphore> present_semaphores;
  VkSemaphore acquire_semaphore{};
  bool ready = false;
  uint32_t MemoryType(uint32_t mask, VkMemoryPropertyFlags flags);
  void MakeView(VkImage target, VkFormat format, VkImageView* view);
  void MakeRenderPass(VkFormat format, VkRenderPass* pass);
  void MakePipeline(VkRenderPass pass, VkPipeline* output);
  void MakeFramebuffer(VkImageView view, VkRenderPass pass, VkExtent2D size, VkFramebuffer* output);
  void Begin();
  void Draw(VkFramebuffer target, VkRenderPass pass, VkPipeline pipe, VkExtent2D size);
  void Submit(VkSemaphore wait = {}, VkSemaphore signal = {});
  void Cleanup(bool wait = true) noexcept;
};

void Probe::Initialize(bool wsi)
{
  Log("Vulkan probe", "BEGIN instance/device; presentation=%d", wsi);
  auto create = reinterpret_cast<PFN_vkCreateInstance>(vk_icdGetInstanceProcAddr({}, "vkCreateInstance"));
  Require(create != nullptr, "missing vkCreateInstance");
  const char* extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_NN_VI_SURFACE_EXTENSION_NAME};
  VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  application.pApplicationName = "Dolphin Vulkan Probe";
  application.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  info.pApplicationInfo = &application;
  info.enabledExtensionCount = wsi ? 2 : 0;
  info.ppEnabledExtensionNames = wsi ? extensions : nullptr;
  Check(create(&info, nullptr, &instance), "vkCreateInstance");
#define LOAD_INSTANCE(name) \
  name = reinterpret_cast<PFN_vk##name>(vk_icdGetInstanceProcAddr(instance, "vk" #name)); \
  Require(name != nullptr, "missing vk" #name);
  INSTANCE_FUNCTIONS(LOAD_INSTANCE)
  if (wsi) { WSI_FUNCTIONS(LOAD_INSTANCE) }
#undef LOAD_INSTANCE
  uint32_t count = 0;
  Check(EnumeratePhysicalDevices(instance, &count, nullptr), "enumerate devices/count");
  Require(count > 0, "no Vulkan physical device");
  std::vector<VkPhysicalDevice> devices(count);
  Check(EnumeratePhysicalDevices(instance, &count, devices.data()), "enumerate devices");
  physical = devices.front();
  VkPhysicalDeviceProperties properties{};
  GetPhysicalDeviceProperties(physical, &properties);
  Log("Vulkan device", "%s api=%u.%u.%u vendor=0x%x device=0x%x driver=0x%x",
      properties.deviceName, VK_API_VERSION_MAJOR(properties.apiVersion),
      VK_API_VERSION_MINOR(properties.apiVersion), VK_API_VERSION_PATCH(properties.apiVersion),
      properties.vendorID, properties.deviceID, properties.driverVersion);
  VkPhysicalDeviceFeatures features{};
  GetPhysicalDeviceFeatures(physical, &features);
  Log("Vulkan features", "geometry=%u logicOp=%u depthClamp=%u dualSrcBlend=%u samplerAnisotropy=%u fragmentAtomics=%u",
      features.geometryShader, features.logicOp, features.depthClamp, features.dualSrcBlend,
      features.samplerAnisotropy, features.fragmentStoresAndAtomics);
  Log("Vulkan limits", "image2D=%u uniformRange=%u storageRange=%u maxPushConstants=%u",
      properties.limits.maxImageDimension2D, properties.limits.maxUniformBufferRange,
      properties.limits.maxStorageBufferRange, properties.limits.maxPushConstantsSize);
  GetPhysicalDeviceMemoryProperties(physical, &memory);
  for (uint32_t i = 0; i < memory.memoryHeapCount; ++i)
    Log("Vulkan memory", "heap=%u size=%llu flags=0x%x", i,
        static_cast<unsigned long long>(memory.memoryHeaps[i].size), memory.memoryHeaps[i].flags);
  for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
    Log("Vulkan memory", "type=%u heap=%u flags=0x%x", i, memory.memoryTypes[i].heapIndex,
        memory.memoryTypes[i].propertyFlags);
  Check(EnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr), "device extensions/count");
  std::vector<VkExtensionProperties> available(count);
  Check(EnumerateDeviceExtensionProperties(physical, nullptr, &count, available.data()), "device extensions");
  for (const auto& extension : available)
    Log("Vulkan extension", "%s revision=%u", extension.extensionName, extension.specVersion);
  GetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
  std::vector<VkQueueFamilyProperties> queues(count);
  GetPhysicalDeviceQueueFamilyProperties(physical, &count, queues.data());
  for (uint32_t i = 0; i < count; ++i)
    if (queues[i].queueCount && (queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) { family = i; break; }
  Require(family != UINT32_MAX, "no graphics queue");
  float priority = 1.0f;
  VkDeviceQueueCreateInfo qinfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qinfo.queueFamilyIndex = family;
  qinfo.queueCount = 1;
  qinfo.pQueuePriorities = &priority;
  const char* swap_extension = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
  VkDeviceCreateInfo dinfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dinfo.queueCreateInfoCount = 1;
  dinfo.pQueueCreateInfos = &qinfo;
  dinfo.enabledExtensionCount = wsi ? 1 : 0;
  dinfo.ppEnabledExtensionNames = wsi ? &swap_extension : nullptr;
  Check(CreateDevice(physical, &dinfo, nullptr, &device), "vkCreateDevice");
#define LOAD_DEVICE(name) \
  name = reinterpret_cast<PFN_vk##name>(GetDeviceProcAddr(device, "vk" #name)); \
  Require(name != nullptr, "missing vk" #name);
  DEVICE_FUNCTIONS(LOAD_DEVICE)
  if (wsi) { SWAP_FUNCTIONS(LOAD_DEVICE) }
#undef LOAD_DEVICE
  ready = true;
  GetDeviceQueue(device, family, 0, &queue);
  VkCommandPoolCreateInfo pinfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pinfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pinfo.queueFamilyIndex = family;
  Check(CreateCommandPool(device, &pinfo, nullptr, &pool), "create command pool");
  VkCommandBufferAllocateInfo cinfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cinfo.commandPool = pool;
  cinfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cinfo.commandBufferCount = 1;
  Check(AllocateCommandBuffers(device, &cinfo, &command), "allocate command buffer");
  VkFenceCreateInfo finfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  Check(CreateFence(device, &finfo, nullptr, &fence), "create fence");
  VkShaderModuleCreateInfo sinfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  sinfo.codeSize = sizeof(triangle_vert);
  sinfo.pCode = triangle_vert;
  Check(CreateShaderModule(device, &sinfo, nullptr, &vertex), "create vertex shader");
  sinfo.codeSize = sizeof(triangle_frag);
  sinfo.pCode = triangle_frag;
  Check(CreateShaderModule(device, &sinfo, nullptr, &fragment), "create fragment shader");
  VkPipelineLayoutCreateInfo linfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  Check(CreatePipelineLayout(device, &linfo, nullptr, &layout), "create pipeline layout");
  Log("Vulkan probe", "PASS instance/device/command pool/shader modules");
}

uint32_t Probe::MemoryType(uint32_t mask, VkMemoryPropertyFlags flags)
{
  for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
    if ((mask & (1u << i)) && (memory.memoryTypes[i].propertyFlags & flags) == flags) return i;
  throw std::runtime_error("no compatible Vulkan memory type for flags=" + std::to_string(flags));
}
void Probe::MakeView(VkImage target, VkFormat format, VkImageView* view)
{
  VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  info.image = target;
  info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  info.format = format;
  info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  Check(CreateImageView(device, &info, nullptr, view), "create image view");
}
void Probe::MakeRenderPass(VkFormat format, VkRenderPass* pass)
{
  VkAttachmentDescription attachment{};
  attachment.format = format;
  attachment.samples = VK_SAMPLE_COUNT_1_BIT;
  attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1;
  subpass.pColorAttachments = &reference;
  VkSubpassDependency dependency{};
  dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
  dependency.dstSubpass = 0;
  dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  info.attachmentCount = 1; info.pAttachments = &attachment;
  info.subpassCount = 1; info.pSubpasses = &subpass;
  info.dependencyCount = 1; info.pDependencies = &dependency;
  Check(CreateRenderPass(device, &info, nullptr, pass), "create render pass");
}
void Probe::MakePipeline(VkRenderPass pass, VkPipeline* output)
{
  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vertex; stages[0].pName = "main";
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fragment; stages[1].pName = "main";
  VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vp.viewportCount = vp.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.cullMode = VK_CULL_MODE_NONE;
  rs.lineWidth = 1;
  VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState blend{};
  blend.colorWriteMask = 0xf;
  VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  cb.attachmentCount = 1; cb.pAttachments = &blend;
  const VkDynamicState states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dy.dynamicStateCount = 2; dy.pDynamicStates = states;
  VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  info.stageCount = 2; info.pStages = stages;
  info.pVertexInputState = &vi; info.pInputAssemblyState = &ia;
  info.pViewportState = &vp; info.pRasterizationState = &rs;
  info.pMultisampleState = &ms; info.pColorBlendState = &cb; info.pDynamicState = &dy;
  info.layout = layout; info.renderPass = pass;
  Log("Vulkan shader", "BEGIN graphics pipeline (SPIR-V -> NAK on Switch)");
  Check(CreateGraphicsPipelines(device, {}, 1, &info, nullptr, output), "compile graphics pipeline");
  Log("Vulkan shader", "PASS graphics pipeline");
}
void Probe::MakeFramebuffer(VkImageView view, VkRenderPass pass, VkExtent2D size, VkFramebuffer* output)
{
  VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
  info.renderPass = pass; info.attachmentCount = 1; info.pAttachments = &view;
  info.width = size.width; info.height = size.height; info.layers = 1;
  Check(CreateFramebuffer(device, &info, nullptr, output), "create framebuffer");
}
void Probe::Begin()
{
  Check(ResetCommandBuffer(command, 0), "reset command buffer");
  VkCommandBufferBeginInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  Check(BeginCommandBuffer(command, &info), "begin command buffer");
}
void Probe::Draw(VkFramebuffer target, VkRenderPass pass, VkPipeline pipe, VkExtent2D size)
{
  VkClearValue clear{}; clear.color.float32[3] = 1;
  VkRenderPassBeginInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  info.renderPass = pass; info.framebuffer = target; info.renderArea.extent = size;
  info.clearValueCount = 1; info.pClearValues = &clear;
  CmdBeginRenderPass(command, &info, VK_SUBPASS_CONTENTS_INLINE);
  VkViewport viewport{0, 0, static_cast<float>(size.width), static_cast<float>(size.height), 0, 1};
  VkRect2D scissor{{0, 0}, size};
  CmdSetViewport(command, 0, 1, &viewport); CmdSetScissor(command, 0, 1, &scissor);
  CmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
  CmdDraw(command, 3, 1, 0, 0);
  CmdEndRenderPass(command);
}
void Probe::Submit(VkSemaphore wait, VkSemaphore signal)
{
  Check(EndCommandBuffer(command), "end command buffer");
  Check(ResetFences(device, 1, &fence), "reset fence");
  VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  info.commandBufferCount = 1; info.pCommandBuffers = &command;
  info.waitSemaphoreCount = wait ? 1 : 0; info.pWaitSemaphores = wait ? &wait : nullptr;
  info.pWaitDstStageMask = wait ? &wait_stage : nullptr;
  info.signalSemaphoreCount = signal ? 1 : 0; info.pSignalSemaphores = signal ? &signal : nullptr;
  Check(QueueSubmit(queue, 1, &info, fence), "queue submit");
  Check(WaitForFences(device, 1, &fence, VK_TRUE, WAIT_NS), "GPU fence (5 second timeout)");
}

void Probe::Readback()
{
  Log("Vulkan readback", "BEGIN 256x256 offscreen triangle");
  VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  info.imageType = VK_IMAGE_TYPE_2D; info.format = VK_FORMAT_R8G8B8A8_UNORM;
  info.extent = {DIM, DIM, 1}; info.mipLevels = info.arrayLayers = 1;
  info.samples = VK_SAMPLE_COUNT_1_BIT; info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  Check(CreateImage(device, &info, nullptr, &image), "create offscreen image");
  VkMemoryRequirements requirements{};
  GetImageMemoryRequirements(device, image, &requirements);
  VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocation.allocationSize = requirements.size;
  allocation.memoryTypeIndex = MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  Check(AllocateMemory(device, &allocation, nullptr, &image_memory), "allocate offscreen memory");
  Check(BindImageMemory(device, image, image_memory, 0), "bind offscreen memory");
  MakeView(image, info.format, &image_view);
  MakeRenderPass(info.format, &render_pass);
  MakePipeline(render_pass, &pipeline);
  MakeFramebuffer(image_view, render_pass, {DIM, DIM}, &framebuffer);
  VkBufferCreateInfo binfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  binfo.size = DIM * DIM * 4; binfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  Check(CreateBuffer(device, &binfo, nullptr, &buffer), "create readback buffer");
  GetBufferMemoryRequirements(device, buffer, &requirements);
  allocation.allocationSize = requirements.size;
  allocation.memoryTypeIndex = MemoryType(requirements.memoryTypeBits,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  Check(AllocateMemory(device, &allocation, nullptr, &buffer_memory), "allocate coherent readback memory");
  Check(BindBufferMemory(device, buffer, buffer_memory, 0), "bind readback memory");
  Check(MapMemory(device, buffer_memory, 0, VK_WHOLE_SIZE, 0, &mapped), "map readback memory");
  std::memset(mapped, 0xcd, DIM * DIM * 4);
  Begin(); Draw(framebuffer, render_pass, pipeline, {DIM, DIM});
  VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image; barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  CmdPipelineBarrier(command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
      0, 0, nullptr, 0, nullptr, 1, &barrier);
  VkBufferImageCopy copy{};
  copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; copy.imageExtent = {DIM, DIM, 1};
  CmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
  VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
  host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  host.buffer = buffer; host.size = VK_WHOLE_SIZE;
  CmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
      0, 0, nullptr, 1, &host, 0, nullptr);
  Submit();
  const auto* pixels = static_cast<const uint8_t*>(mapped);
  const auto* center = pixels + ((DIM / 2) * DIM + DIM / 2) * 4;
  const auto* corner = pixels + (4 * DIM + 4) * 4;
  Log("Vulkan readback", "center RGBA=%u,%u,%u,%u corner=%u,%u,%u,%u",
      center[0], center[1], center[2], center[3], corner[0], corner[1], corner[2], corner[3]);
  Require(center[0] > 20 && center[1] > 20 && center[2] > 20 && center[3] == 255,
      "triangle center pixel mismatch");
  Require(corner[0] == 0 && corner[1] == 0 && corner[2] == 0 && corner[3] == 255,
      "triangle clear/background pixel mismatch");
  Log("Vulkan readback", "PASS shader execution + GPU-to-CPU readback");
}

void Probe::Present()
{
#ifndef __SWITCH__
  throw std::runtime_error("Horizon presentation requested on host");
#else
  Log("Vulkan WSI", "BEGIN native window/swapchain");
  VkViSurfaceCreateInfoNN info{VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN};
  info.window = nwindowGetDefault();
  Check(CreateViSurfaceNN(instance, &info, nullptr, &surface), "create VI surface");
  VkBool32 supported = VK_FALSE;
  Check(GetPhysicalDeviceSurfaceSupportKHR(physical, family, surface, &supported), "query presentation support");
  Require(supported, "graphics queue cannot present to VI");
  VkSurfaceCapabilitiesKHR caps{};
  Check(GetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps), "query surface capabilities");
  Require(caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, "surface lacks color attachment support");
  Require(caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, "surface lacks opaque compositing");
  uint32_t count = 0;
  Check(GetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, nullptr), "surface format count");
  Require(count > 0, "no surface formats");
  std::vector<VkSurfaceFormatKHR> formats(count);
  Check(GetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, formats.data()), "surface formats");
  VkSurfaceFormatKHR format{};
  for (const auto& candidate : formats)
    if ((candidate.format == VK_FORMAT_R8G8B8A8_UNORM || candidate.format == VK_FORMAT_B8G8R8A8_UNORM) &&
        candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { format = candidate; break; }
  Require(format.format != VK_FORMAT_UNDEFINED, "surface lacks RGBA8/BGRA8 UNORM");
  VkExtent2D size = caps.currentExtent;
  if (size.width == UINT32_MAX)
    size = {std::clamp(1280u, caps.minImageExtent.width, caps.maxImageExtent.width),
            std::clamp(720u, caps.minImageExtent.height, caps.maxImageExtent.height)};
  Require(size.width && size.height, "zero-sized surface");
  const uint32_t wanted = std::max(3u, caps.minImageCount);
  Require(!caps.maxImageCount || wanted <= caps.maxImageCount, "surface cannot provide three buffers");
  Log("Vulkan WSI", "surface=%ux%u format=%d image_count=%u", size.width, size.height, format.format, wanted);
  VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  sci.surface = surface; sci.minImageCount = wanted;
  sci.imageFormat = format.format; sci.imageColorSpace = format.colorSpace;
  sci.imageExtent = size; sci.imageArrayLayers = 1;
  sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  sci.preTransform = caps.currentTransform;
  sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  sci.presentMode = VK_PRESENT_MODE_FIFO_KHR; sci.clipped = VK_TRUE;
  Check(CreateSwapchainKHR(device, &sci, nullptr, &swapchain), "create swapchain");
  Check(GetSwapchainImagesKHR(device, swapchain, &count, nullptr), "swapchain image count");
  Require(count >= wanted, "too few swapchain images");
  swap_images.resize(count); swap_views.resize(count); swap_frames.resize(count); present_semaphores.resize(count);
  Check(GetSwapchainImagesKHR(device, swapchain, &count, swap_images.data()), "get swapchain images");
  MakeRenderPass(format.format, &swap_render_pass); MakePipeline(swap_render_pass, &swap_pipeline);
  VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  Check(CreateSemaphore(device, &sem, nullptr, &acquire_semaphore), "create acquire semaphore");
  for (uint32_t i = 0; i < count; ++i)
  {
    MakeView(swap_images[i], format.format, &swap_views[i]);
    MakeFramebuffer(swap_views[i], swap_render_pass, size, &swap_frames[i]);
    Check(CreateSemaphore(device, &sem, nullptr, &present_semaphores[i]), "create per-image present semaphore");
  }
  PadState pad{}; padInitializeDefault(&pad);
  unsigned presented = 0;
  while (presented < 300 && appletMainLoop())
  {
    padUpdate(&pad);
    if (padGetButtonsDown(&pad) & (HidNpadButton_B | HidNpadButton_Plus)) break;
    uint32_t index = 0;
    VkResult result = AcquireNextImageKHR(device, swapchain, WAIT_NS, acquire_semaphore, {}, &index);
    if (result != VK_SUBOPTIMAL_KHR) Check(result, "acquire image (5 second timeout)");
    Require(index < count, "acquired image index out of bounds");
    Begin(); Draw(swap_frames[index], swap_render_pass, swap_pipeline, size);
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = swap_images[index]; barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    CmdPipelineBarrier(command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);
    // A semaphore is reused only when its own image is reacquired. Reuse of
    // a single present semaphore per CPU frame is not a safe WSI assumption.
    Submit(acquire_semaphore, present_semaphores[index]);
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &present_semaphores[index];
    pi.swapchainCount = 1; pi.pSwapchains = &swapchain; pi.pImageIndices = &index;
    result = QueuePresentKHR(queue, &pi);
    if (result != VK_SUBOPTIMAL_KHR) Check(result, "present image");
    ++presented;
    if (presented == 1 || presented % 60 == 0) Log("Vulkan WSI", "presented=%u", presented);
  }
  Require(presented >= 6, "presentation interrupted before cycling all buffers twice");
  Log("Vulkan WSI", "PASS %u successful presents; visible image requires tester confirmation", presented);
#endif
}
void Probe::Finish()
{
  Log("Vulkan cleanup", "BEGIN wait idle");
  Check(DeviceWaitIdle(device), "device wait idle before cleanup");
  Cleanup(false);
  Log("Vulkan cleanup", "PASS explicit object/device/instance destruction");
}
void Probe::Cleanup(bool wait) noexcept
{
  if (device && ready)
  {
    // Best effort on a failed test. A stuck driver may also stall WaitIdle;
    // the preceding durable stage log identifies this. No unsafe early free.
    Log("Vulkan cleanup", "device idle and release resources");
    if (wait)
    {
      const VkResult result = DeviceWaitIdle(device);
      if (result != VK_SUCCESS) Horizon::Error("Vulkan cleanup", "DeviceWaitIdle result=%d", result);
    }
    if (pool) DestroyCommandPool(device, pool, nullptr);
    if (fence) DestroyFence(device, fence, nullptr);
    for (auto value : present_semaphores) if (value) DestroySemaphore(device, value, nullptr);
    if (acquire_semaphore) DestroySemaphore(device, acquire_semaphore, nullptr);
    for (auto value : swap_frames) if (value) DestroyFramebuffer(device, value, nullptr);
    for (auto value : swap_views) if (value) DestroyImageView(device, value, nullptr);
    if (swap_pipeline) DestroyPipeline(device, swap_pipeline, nullptr);
    if (swap_render_pass) DestroyRenderPass(device, swap_render_pass, nullptr);
    if (swapchain) DestroySwapchainKHR(device, swapchain, nullptr);
    if (mapped) UnmapMemory(device, buffer_memory);
    if (buffer) DestroyBuffer(device, buffer, nullptr);
    if (buffer_memory) FreeMemory(device, buffer_memory, nullptr);
    if (framebuffer) DestroyFramebuffer(device, framebuffer, nullptr);
    if (pipeline) DestroyPipeline(device, pipeline, nullptr);
    if (render_pass) DestroyRenderPass(device, render_pass, nullptr);
    if (image_view) DestroyImageView(device, image_view, nullptr);
    if (image) DestroyImage(device, image, nullptr);
    if (image_memory) FreeMemory(device, image_memory, nullptr);
    if (vertex) DestroyShaderModule(device, vertex, nullptr);
    if (fragment) DestroyShaderModule(device, fragment, nullptr);
    if (layout) DestroyPipelineLayout(device, layout, nullptr);
  }
  if (device && DestroyDevice) DestroyDevice(device, nullptr);
  if (surface && DestroySurfaceKHR) DestroySurfaceKHR(instance, surface, nullptr);
  if (instance && DestroyInstance) DestroyInstance(instance, nullptr);
  device = {}; surface = {}; instance = {}; ready = false;
}
}

void RunVulkanProbe(bool present)
{
  Probe probe;
  probe.Initialize(present);
  probe.Readback();
  if (present) probe.Present();
  probe.Finish();
}

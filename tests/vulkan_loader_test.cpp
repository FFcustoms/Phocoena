// SPDX-License-Identifier: GPL-2.0-or-later
// Exercises Dolphin's actual Horizon static-loader branch and VMA on Lavapipe.
// The host loader supplies the ICD entry point; this is not Switch WSI testing.
#include <cassert>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <future>
#include <thread>
#include <vector>
#include "Common/Event.h"
#include "Common/MsgHandler.h"
#include "VideoBackends/Vulkan/CommandBufferManager.h"
#include "Horizon/Performance.h"
#include "VideoBackends/Vulkan/VulkanContext.h"
#include "VideoBackends/Vulkan/VulkanLoader.h"

namespace
{
PFN_vkGetInstanceProcAddr host_gipa;
const char* missing_function = nullptr;
std::atomic<unsigned> expected_errors{0};
PFN_vkQueueSubmit real_queue_submit;
std::atomic<bool> queue_in_use{false};
Common::Event present_entered, release_present;
std::atomic<bool> block_present{false};
std::atomic<bool> delay_present{false};
std::atomic<bool> force_present_failure{false};
std::thread::id producer_id;
bool expect_worker = false;
std::atomic<unsigned> presents{0};

VkResult CheckedSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo* info, VkFence fence)
{
  assert(!queue_in_use.exchange(true)); // Vulkan queue external synchronization
  const auto result = real_queue_submit(queue, count, info, fence);
  queue_in_use.store(false);
  return result;
}

// A controlled WSI substitute: waits on the REAL render-finished semaphore,
// then waits for a real GPU fence. No native surface or Switch driver is tested.
VkResult ControlledPresent(VkQueue queue, const VkPresentInfoKHR* info)
{
  assert(!queue_in_use.exchange(true));
  assert((std::this_thread::get_id() != producer_id) == expect_worker);
  if (block_present.exchange(false))
  {
    present_entered.Set();
    assert(release_present.WaitFor(std::chrono::seconds(10)));
  }
  assert(info->swapchainCount == 1 && info->pImageIndices[0] < 3);
  std::vector<VkPipelineStageFlags> stages(info->waitSemaphoreCount, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
  VkSubmitInfo wait{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  wait.waitSemaphoreCount = info->waitSemaphoreCount;
  wait.pWaitSemaphores = info->pWaitSemaphores;
  wait.pWaitDstStageMask = stages.data();
  VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VkFence fence{};
  const auto device = Vulkan::g_vulkan_context->GetDevice();
  assert(vkCreateFence(device, &fci, nullptr, &fence) == VK_SUCCESS);
  assert(real_queue_submit(queue, 1, &wait, fence) == VK_SUCCESS);
  assert(vkWaitForFences(device, 1, &fence, VK_TRUE, 5'000'000'000) == VK_SUCCESS);
  vkDestroyFence(device, fence, nullptr);
  const unsigned ordinal = presents.fetch_add(1);
  if (delay_present.load() && (ordinal & 7) == 3)
    std::this_thread::sleep_for(std::chrono::microseconds(50 + (ordinal * 37) % 250));
  const VkResult result = force_present_failure.exchange(false) ?
                              VK_ERROR_SURFACE_LOST_KHR : VK_SUCCESS;
  queue_in_use.store(false);
  return result;
}

// The completion tracker is the CPU-side WSI ownership protocol. Stress delayed completion,
// large worker backlog, rapid two/three-slot wraparound and waits on intermediate serials without
// requiring a real platform surface.
void TestPresentCompletionTracker(unsigned frame_resources)
{
  constexpr u64 transitions = 50'000;
  Vulkan::PresentCompletionTracker tracker;
  std::vector<u64> serials;
  serials.reserve(transitions);
  for (u64 i = 0; i < transitions; ++i)
    serials.push_back(tracker.Queue());

  std::thread worker([&] {
    for (u64 i = 0; i < transitions; ++i)
    {
      tracker.Begin(serials[i]);
      if ((i & 511) == 127)
        std::this_thread::sleep_for(std::chrono::microseconds(25 + (i * 17) % 100));
      tracker.Complete(serials[i]);
    }
  });

  std::vector<u64> slots(frame_resources);
  for (u64 frame = 0; frame < transitions; ++frame)
  {
    const unsigned slot = static_cast<unsigned>(frame % frame_resources);
    if (slots[slot] != 0)
    {
      tracker.WaitFor(slots[slot]);
      assert(tracker.Read().completed >= slots[slot]);
    }
    slots[slot] = serials[frame];
    if ((frame & 4095) == 0)
      tracker.WaitFor(serials[frame]);
  }
  tracker.WaitForLastQueued();
  worker.join();
  const auto state = tracker.Read();
  assert(state.queued == transitions && state.begun == transitions &&
         state.completed == transitions && state.active == 0 &&
         state.peak_pending == transitions);
}

void TestCommandManager(VkDevice device, VkQueue queue, uint32_t family, VmaAllocator allocator,
                        VkBuffer buffer, VmaAllocation allocation, void* mapped, bool threaded,
                        uint32_t frame_resources)
{
  const auto perf_before = Horizon::GetPerfCounters().Read();
  Vulkan::g_vulkan_context = std::make_unique<Vulkan::VulkanContext>(device, queue, family, allocator);
  real_queue_submit = vkQueueSubmit;
  const auto real_present = vkQueuePresentKHR;
  vkQueueSubmit = CheckedSubmit;
  vkQueuePresentKHR = ControlledPresent;
  producer_id = std::this_thread::get_id();
  expect_worker = threaded;
  presents.store(0);
  delay_present.store(true);
  force_present_failure.store(false);
  // Enough frames to repeatedly wrap the actual eight-buffer ring and two/three frame slots.
  constexpr unsigned frames = 64;
  {
    Vulkan::CommandBufferManager manager(threaded, frame_resources);
    assert(manager.Initialize(3));
    const auto record = [&](unsigned frame) {
      const auto command = manager.GetCurrentCommandBuffer();
      vkCmdFillBuffer(command, buffer, frame * 4, 4, 0xdec00000 + frame);
      VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
      barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                           0, 1, &barrier, 0, nullptr, 0, nullptr);
    };
    block_present.store(threaded);
    record(0);
    const auto fake_swapchain = reinterpret_cast<VkSwapchainKHR>(uintptr_t{1});
    manager.SubmitCommandBuffer(true, false, true, fake_swapchain, 0);
    if (threaded)
      assert(present_entered.WaitFor(std::chrono::seconds(10)));
    record(1); // producer can record the next frame while worker present is blocked
    if (threaded)
    {
      assert(queue_in_use.load());
      auto guarded_acquire = std::async(std::launch::async, [&manager] {
        manager.WaitForLastPresentCompletion();
      });
      assert(guarded_acquire.wait_for(std::chrono::milliseconds(20)) ==
             std::future_status::timeout);
      release_present.Set();
      assert(guarded_acquire.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
      guarded_acquire.get();
      const auto completed = manager.GetPresentCompletionSnapshot();
      assert(completed.queued == 1 && completed.completed == 1 && completed.active == 0);
    }
    manager.SubmitCommandBuffer(true, false, true, fake_swapchain, 1);
    for (unsigned frame = 2; frame < frames; ++frame)
    {
      record(frame);
      if (threaded)
        manager.WaitForLastPresentCompletion();
      if (threaded && frame == 7)
        force_present_failure.store(true);
      manager.SubmitCommandBuffer(true, false, true, fake_swapchain, frame % 3);
      if (threaded && frame == 7)
      {
        manager.WaitForLastPresentCompletion();
        assert(manager.GetAndClearLastPresentResult() == VK_ERROR_SURFACE_LOST_KHR);
        assert(manager.GetAndClearLastPresentResult() == VK_SUCCESS);
      }
    }
    if (threaded)
      manager.WaitForLastPresentCompletion();
    // A synchronous flush must drain prior worker jobs before touching the queue.
    manager.SubmitCommandBuffer(false, true);
    manager.WaitForWorkerThreadIdle();
    assert(!queue_in_use.load() && presents.load() == frames);
    const auto presentation = manager.GetPresentCompletionSnapshot();
    if (threaded)
      assert(presentation.queued == frames && presentation.begun == frames &&
             presentation.completed == frames && presentation.active == 0);
    else
      assert(presentation.queued == 0 && presentation.completed == 0);
    assert(vkDeviceWaitIdle(device) == VK_SUCCESS);
  } // real manager drains/joins worker and destroys its pools/fences/semaphores
  const auto perf = Horizon::PerformanceDelta(perf_before, Horizon::GetPerfCounters().Read());
  const auto present = perf[static_cast<size_t>(Horizon::PerfMetric::VulkanPresent)];
  assert(present.calls == frames && present.cpu_calls == frames);
  assert(perf[static_cast<size_t>(Horizon::PerfMetric::VulkanSyncSubmit)].calls >= 1);
  if (threaded)
  {
    assert(perf[static_cast<size_t>(Horizon::PerfMetric::VulkanWorkerWait)].calls >= 1);
    const auto worker = Horizon::GetVulkanWorkerUsage().Read();
    assert(worker.valid && worker.id != Horizon::ReadCurrentThreadUsage().id);
  }
  assert(vmaInvalidateAllocation(allocator, allocation, 0, VK_WHOLE_SIZE) == VK_SUCCESS);
  for (unsigned frame = 0; frame < frames; ++frame)
    assert(static_cast<uint32_t*>(mapped)[frame] == 0xdec00000 + frame);
  vkQueueSubmit = real_queue_submit;
  vkQueuePresentKHR = real_present;
  delay_present.store(false);
  Vulkan::g_vulkan_context.reset();
}
}
namespace Vulkan
{
std::unique_ptr<VulkanContext> g_vulkan_context;
}
namespace Common
{
void SetCurrentThreadName(const char*) {}
bool MsgAlertFmtImpl(bool, MsgType, Log::LogType, const char*, int, fmt::string_view text,
                     const fmt::format_args& args)
{
  std::fprintf(stderr, "Unexpected Dolphin alert: %s\n", fmt::vformat(text, args).c_str());
  std::abort();
}
}
extern "C" PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance, const char* name)
{
  if (missing_function && std::strcmp(name, missing_function) == 0)
    return nullptr;
  return host_gipa(instance, name);
}
namespace Horizon
{
void Log(const char*, const char*, ...) {}
void Error(const char*, const char*, ...) { ++expected_errors; }
}
namespace Common::Log
{
void GenericLogFmtImpl(LogLevel level, LogType, const char*, int, fmt::string_view,
                       const fmt::format_args&)
{
  if (level == LogLevel::LERROR)
    ++expected_errors;
}
}

int main()
{
  TestPresentCompletionTracker(2);
  TestPresentCompletionTracker(3);
  void* library = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
  assert(library);
  host_gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(library, "vkGetInstanceProcAddr"));
  assert(host_gipa);
  missing_function = "vkCreateInstance";
  assert(!Vulkan::LoadVulkanLibrary());
  assert(!vkCreateInstance && !vkGetInstanceProcAddr && !vkGetDeviceProcAddr);
  missing_function = nullptr;

  for (unsigned cycle = 0; cycle < 3; ++cycle)
  {
    assert(Vulkan::LoadVulkanLibrary());
    assert(Vulkan::LoadVulkanLibrary()); // repeated load must not reset live pointers
    assert(!vkGetDeviceProcAddr); // instance-scoped, not a null-instance export
    uint32_t layer_count = 999;
    assert(vkEnumerateInstanceLayerProperties(&layer_count, nullptr) == VK_SUCCESS);
    assert(layer_count == 0);

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    create.pApplicationInfo = &app;
    VkInstance instance{};
    assert(vkCreateInstance(&create, nullptr, &instance) == VK_SUCCESS);
    missing_function = "vkGetDeviceProcAddr";
    assert(!Vulkan::LoadVulkanInstanceFunctions(instance));
    missing_function = "vkGetPhysicalDeviceProperties";
    assert(!Vulkan::LoadVulkanInstanceFunctions(instance));
    missing_function = nullptr;
    assert(Vulkan::LoadVulkanInstanceFunctions(instance));
    assert(vkGetDeviceProcAddr && !vkCreateViSurfaceNN); // VI is optional for headless enumeration

    uint32_t count = 0;
    assert(vkEnumeratePhysicalDevices(instance, &count, nullptr) == VK_SUCCESS && count);
    std::vector<VkPhysicalDevice> devices(count);
    assert(vkEnumeratePhysicalDevices(instance, &count, devices.data()) == VK_SUCCESS);
    VkPhysicalDevice physical = devices.front();
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
    uint32_t family = 0;
    while (family < count && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) ++family;
    assert(family < count);
    float priority = 1.f;
    VkDeviceQueueCreateInfo qinfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qinfo.queueFamilyIndex = family; qinfo.queueCount = 1; qinfo.pQueuePriorities = &priority;
    VkDeviceCreateInfo dinfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dinfo.queueCreateInfoCount = 1; dinfo.pQueueCreateInfos = &qinfo;
    VkDevice device{};
    assert(vkCreateDevice(physical, &dinfo, nullptr, &device) == VK_SUCCESS);
    assert(Vulkan::LoadVulkanDeviceFunctions(device));

    VmaAllocatorCreateInfo ainfo{};
    ainfo.instance = instance; ainfo.physicalDevice = physical; ainfo.device = device;
    ainfo.vulkanApiVersion = VK_API_VERSION_1_2;
    ainfo.flags = VMA_ALLOCATOR_CREATE_EXTERNALLY_SYNCHRONIZED_BIT;
    VmaAllocator allocator{};
    assert(vmaCreateAllocator(&ainfo, &allocator) == VK_SUCCESS);
    VkBufferCreateInfo binfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    binfo.size = 4096; binfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO;
    alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VkBuffer buffer{};
    VmaAllocation allocation{};
    VmaAllocationInfo allocation_info{};
    assert(vmaCreateBuffer(allocator, &binfo, &alloc, &buffer, &allocation, &allocation_info) == VK_SUCCESS);
    assert(allocation_info.pMappedData);
    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.queueFamilyIndex = family;
    VkCommandPool pool{};
    assert(vkCreateCommandPool(device, &pool_info, nullptr, &pool) == VK_SUCCESS);
    VkCommandBufferAllocateInfo cmd_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cmd_info.commandPool = pool; cmd_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cmd_info.commandBufferCount = 1;
    VkCommandBuffer command{};
    assert(vkAllocateCommandBuffers(device, &cmd_info, &command) == VK_SUCCESS);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    assert(vkBeginCommandBuffer(command, &begin) == VK_SUCCESS);
    vkCmdFillBuffer(command, buffer, 0, 4096, 0xdec0deda);
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 1, &barrier, 0, nullptr, 0, nullptr);
    assert(vkEndCommandBuffer(command) == VK_SUCCESS);
    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence{};
    assert(vkCreateFence(device, &fence_info, nullptr, &fence) == VK_SUCCESS);
    VkQueue queue{}; vkGetDeviceQueue(device, family, 0, &queue);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
    assert(vkQueueSubmit(queue, 1, &submit, fence) == VK_SUCCESS);
    assert(vkWaitForFences(device, 1, &fence, VK_TRUE, 5'000'000'000) == VK_SUCCESS);
    assert(vmaInvalidateAllocation(allocator, allocation, 0, VK_WHOLE_SIZE) == VK_SUCCESS);
    for (unsigned i = 0; i < 1024; ++i)
      assert(static_cast<uint32_t*>(allocation_info.pMappedData)[i] == 0xdec0deda);
    vkDestroyFence(device, fence, nullptr);
    vkDestroyCommandPool(device, pool, nullptr);
    for (const uint32_t frame_resources : {2u, 3u})
    {
      TestCommandManager(device, queue, family, allocator, buffer, allocation,
                         allocation_info.pMappedData, false, frame_resources);
      TestCommandManager(device, queue, family, allocator, buffer, allocation,
                         allocation_info.pMappedData, true, frame_resources);
    }
    vmaDestroyBuffer(allocator, buffer, allocation);
    vmaDestroyAllocator(allocator);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    Vulkan::UnloadVulkanLibrary();
    assert(!vkGetInstanceProcAddr && !vkGetDeviceProcAddr && !vkCreateDevice && !vkQueueSubmit);
  }
  assert(expected_errors.load() == 13); // seven loader failures + six forced async presents
  dlclose(library);
  std::puts("PASS: Horizon static loader failure/retry + 3 Vulkan/VMA buffer readback/teardown cycles on host Lavapipe");
  std::puts("PASS: 100000 serialized WSI ownership transitions with delayed completion, backlog and 2/3-slot wraparound");
  std::puts("PASS: actual Dolphin command manager, worker Off/On, 2/3 frame resources, exact present-completion guard, forced async failure delivery, delayed presentation, queue exclusion, ring reuse, semaphore waits, readback and drain/join (host WSI substitute)");
}

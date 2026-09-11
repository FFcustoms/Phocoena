#!/usr/bin/env python3
"""Check the AArch64 companion ELF and the graphics fatal-log link wrappers."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('elf', type=Path)
    parser.add_argument('--renderer', choices=['OpenGL', 'Vulkan'], default='OpenGL')
    parser.add_argument('--binutils', type=Path, required=True,
                        help='devkitA64/bin directory')
    args = parser.parse_args()

    def run(tool, *options):
        return subprocess.check_output([str(args.binutils / ('aarch64-none-elf-' + tool)),
                                        *options, str(args.elf)], text=True)

    headers = run('readelf', '-h', '-n', '-S')
    if 'AArch64' not in headers or '.debug_line' not in headers:
        raise RuntimeError('Expected AArch64 ELF with debug line tables')
    undefined = run('nm', '--undefined-only').strip()
    if undefined:
        raise RuntimeError('Undefined symbols: ' + undefined)
    symbols = run('nm', '--defined-only')
    driver_symbols = []
    if args.renderer == 'Vulkan':
        driver_symbols = ['vk_icdGetInstanceProcAddr', 'vkCmdPushDescriptorSetKHR',
                          'nvk_CreateDevice', 'nak_compile_shader',
                          'wsi_switch_init_wsi', '__libnx_exception_handler']
        for name in driver_symbols:
            if not re.search(r'\b' + name + r'$', symbols, re.M):
                raise RuntimeError('Missing linked Vulkan driver/exception symbol: ' + name)
        for name in ['eglInitialize', 'eglCreateContext', 'glDrawArrays', 'SDL_InitSubSystem']:
            if re.search(r'\b' + name + r'$', symbols, re.M):
                raise RuntimeError('Unexpected old graphics/audio driver: ' + name)
    wrapper_calls = {}
    required_calls = {
        'switch_swap_buffers': ['__wrap_fatalThrow', '__wrap_nwindowQueueBuffer'],
        'switch_st_framebuffer_validate': ['__wrap_nwindowDequeueBuffer'],
        'switch_egl_surface_cleanup': ['__wrap_nwindowReleaseBuffers'],
        'ConsoleSwRenderer_init': ['__wrap_framebufferCreate'],
        'framebufferBegin': ['__wrap_diagAbortWithResult', '__wrap_nwindowDequeueBuffer'],
        'framebufferEnd': ['__wrap_nwindowQueueBuffer'],
        'framebufferClose': ['__wrap_nwindowReleaseBuffers'],
        'nwindowDequeueBuffer': ['__wrap_bqDequeueBuffer', '__wrap_bqRequestBuffer'],
        # The capability probe and real GL init must both execute the window
        # handoff helper. Confirm the final binary calls it, not just that it exists.
        '_ZN7Horizon15GLContextSwitch10InitializeERK16WindowSystemInfobb':
            ['_ZN7Horizon20PrepareDisplayWindowEPKc'],
        '_ZN7Horizon15GLContextSwitch4SwapEv': ['_ZN7Horizon12RecordGLSwapEv'],
        '_ZN7Horizon22ReadCurrentThreadUsageEv': ['svcGetThreadId', 'svcGetInfo'],
        # GCC inlines PowerPCManager::InitializeCPUCore into Init at this pin.
        '_ZN7PowerPC14PowerPCManager4InitENS_7CPUCoreE':
            ['_ZN7Horizon17InitializeCPUCoreERN4Core6SystemEN7PowerPC7CPUCoreE'],
        # Native JIT: actual stores and pointer-table writes must translate RX
        # destinations. Requiring these calls catches a linked but unused port.
        '_ZN8Arm64Gen13ARM64XEmitter7Write32Ej':
            ['_ZN6Common21GetWritableJitAddressEPvm'],
        '_ZN8Arm64Gen13ARM64XEmitter13SetJumpTargetERKNS_11FixupBranchE':
            ['_ZN6Common21GetWritableJitAddressEPvm'],
        '_ZN8Arm64Gen13ARM64XEmitter11FlushIcacheEv':
            ['_ZN6Common12FlushJitCodeEPvm'],
        '_ZN8JitArm6422GenerateQuantizedLoadsEv':
            ['_ZN6Common21GetWritableJitAddressEPvm'],
        '_ZN8JitArm6423GenerateQuantizedStoresEv':
            ['_ZN6Common21GetWritableJitAddressEPvm'],
        '_ZN7Horizon17InitializeCPUCoreERN4Core6SystemEN7PowerPC7CPUCoreE':
            ['_ZN7Horizon21RunJitEmitterSelfTestEv'],
        # libnx has one exception entry point. It must try Dolphin's fastmem
        # recovery first, while Crash.c retains ownership of unhandled faults.
        '__libnx_exception_handler': ['HorizonTryHandlePpcFastmemException'],
        'HorizonTryHandlePpcFastmemException':
            ['_ZN12JitInterface11HandleFaultEmP8SContext'],
    }
    if args.renderer == 'Vulkan':
        for function in ['switch_swap_buffers', 'switch_st_framebuffer_validate', 'switch_egl_surface_cleanup',
                         '_ZN7Horizon15GLContextSwitch10InitializeERK16WindowSystemInfobb',
                         '_ZN7Horizon15GLContextSwitch4SwapEv']:
            del required_calls[function]
        required_calls.update({
            'wsi_switch_swapchain_acquire_next_image': ['__wrap_nwindowDequeueBuffer'],
            'wsi_switch_swapchain_queue_present': ['__wrap_nwindowQueueBuffer'],
            'wsi_switch_swapchain_destroy': ['__wrap_nwindowReleaseBuffers'],
            '_ZN6Vulkan9SwapChain19CreateVulkanSurfaceEP12VkInstance_TRK16WindowSystemInfo':
                ['_ZN7Horizon20PrepareDisplayWindowEPKc'],
            '_ZN6Vulkan17LoadVulkanLibraryEb': ['vk_icdGetInstanceProcAddr'],
            # Guarded overlap waits for the exact worker-owned present serial;
            # the full-drain rollback and lifecycle paths must remain linked too.
            '_ZN6Vulkan5VKGfx14BindBackbufferERKSt5arrayIfLm4EE':
                ['_ZN6Vulkan20CommandBufferManager23WaitForWorkerThreadIdleEv',
                 '_ZN6Vulkan24PresentCompletionTracker17WaitForLastQueuedEv'],
            '_ZN6Vulkan12VideoBackend8ShutdownEv':
                ['_ZN6Vulkan20CommandBufferManager23WaitForWorkerThreadIdleEv'],
            '_ZN6Vulkan12StateTracker21UpdateGXDescriptorSetEv':
                ['_ZN7Horizon31RecordVulkanPushDescriptorWriteEv',
                 '_ZN7Horizon40RecordVulkanLegacySamplerDescriptorWriteEv'],
        })
    for function, wrappers in required_calls.items():
        code = run('objdump', '-d', '--disassemble=' + function)
        for wrapper in wrappers:
            if not re.search(r'\b(?:bl|b)\s+[^\n]*<' + wrapper + r'>', code):
                raise RuntimeError(f'{function} does not call the expected {wrapper}')
        wrapper_calls[function] = wrappers
    # GCC may inline the small timer destructor or call either destructor alias.
    # In all cases the actual instrumented function must reach the timer, not
    # merely leave an otherwise-unused counter symbol in the executable.
    timing_hooks = {}
    timing_functions = [
        '_ZN7Horizon15GLContextSwitch4SwapEv',
        '_ZN3OGL18ProgramShaderCache19CompileSingleShaderEjSt17basic_string_viewIcSt11char_traitsIcEE',
        '_ZN3OGL18ProgramShaderCache18GetPipelineProgramEPKNS_14GLVertexFormatEPKNS_9OGLShaderES6_S6_PKvm',
        '_ZN8JitArm643JitEj',
        '_ZN3DVD9DVDThread18ProcessReadRequestEONS0_11ReadRequestE',
        '_ZN13AsyncRequests17WaitForEmptyQueueEv',
        '_ZN4Fifo11FifoManager16WaitForGpuThreadEi',
    ]
    if args.renderer == 'Vulkan':
        timing_functions = timing_functions[3:] + [
            '_ZN6Vulkan9SwapChain16AcquireNextImageEv',
            '_ZN6Vulkan20CommandBufferManager19SubmitCommandBufferEjP16VkSwapchainKHR_Tj',
            '_ZN6Vulkan8VKShader16CreateFromSourceE11ShaderStageSt17basic_string_viewIcSt11char_traitsIcEEPN11VideoCommon14ShaderIncluderES5_',
            '_ZN6Vulkan10VKPipeline6CreateERK22AbstractPipelineConfig',
            '_ZN6Vulkan20CommandBufferManager23WaitForWorkerThreadIdleEv',
            '_ZN6Vulkan20CommandBufferManager30WaitForCommandBufferCompletionEj',
        ]
    for function in timing_functions:
        code = run('objdump', '-d', '--disassemble=' + function)
        candidates = ['_ZN7Horizon15GetPerfCountersEv',
                      '_ZN7Horizon16ScopedPerfSampleD1Ev', '_ZN7Horizon16ScopedPerfSampleD2Ev']
        found = [name for name in candidates if re.search(r'\b(?:bl|b)\s+[^\n]*<' + name + r'>', code)]
        if not found:
            raise RuntimeError(f'{function} does not reach the performance timer')
        timing_hooks[function] = found
    print(json.dumps({'architecture': 'AArch64', 'debug_line_tables': True,
                      'renderer': args.renderer, 'driver_symbols': driver_symbols,
                      'undefined_symbols': 0, 'verified_wrapper_call_targets': wrapper_calls,
                      'verified_performance_call_targets': timing_hooks,
                      'build_id': re.search(r'Build ID: (\w+)', headers).group(1),
                      'sha256': hashlib.sha256(args.elf.read_bytes()).hexdigest(),
                      'hardware_tested': False}, indent=2))


if __name__ == '__main__':
    main()

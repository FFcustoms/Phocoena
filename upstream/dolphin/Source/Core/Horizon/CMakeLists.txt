set(HORIZON_PORT_VERSION "0.1.27")
set(HORIZON_UPSTREAM_COMMIT "4f8af23db516d8b6e9cd00e7b261a65b026514a8")
set(HORIZON_TOOLCHAIN_DIGEST "sha256:82575ea78651b530b2e232bb3799cfd1fe331514e053d5f724bb4b28191fb79d")
set(HORIZON_TOOLCHAIN_INFO "devkitA64 r29.2 GCC 15.2.0; libnx 4.12.0; switch-tools 1.13.1; Mesa 20.1.0-5; libnx audout audio")
if(HORIZON_RENDERER STREQUAL "Vulkan")
  set(HORIZON_TOOLCHAIN_INFO "devkitA64 r29.2 GCC 15.2.0; libnx 4.12.0; switch-tools 1.13.1; NXVK 69ec283dbda64e65347a36274efb349122e85363 Mesa 26.1.4; Rust nightly-2026-07-02; libnx audout audio")
endif()
string(TIMESTAMP HORIZON_BUILD_DATE "%Y-%m-%dT%H:%M:%SZ" UTC)
if(NOT HORIZON_PATCHSET_SHA256)
  set(HORIZON_PATCHSET_SHA256 "unrecorded-manual-build")
endif()
if(NOT HORIZON_PHOCOENA_COMMIT)
  set(HORIZON_PHOCOENA_COMMIT "unknown")
endif()
if(NOT HORIZON_PHOCOENA_SOURCE_STATE)
  set(HORIZON_PHOCOENA_SOURCE_STATE "unknown")
endif()
configure_file(BuildInfo.h.in "${PROJECT_BINARY_DIR}/Source/Core/Horizon/BuildInfo.h" @ONLY)
add_executable(phocoena-switch Main.cpp Host.cpp Input.cpp Diagnostics.cpp Console.cpp Display.cpp
  ClockPolicy.cpp ClockControl.cpp TwilightPrincessBenchmark.cpp SupportDiagnostics.cpp Crash.c PThread.cpp)
target_sources(audiocommon PRIVATE ../Horizon/Audio.cpp)
target_sources(core PRIVATE ../Horizon/Safety.cpp ../Horizon/CPU.cpp ../Horizon/RuntimeDiagnostics.cpp ../Horizon/UnavailableTAP.cpp)
target_sources(mbedcrypto PRIVATE MbedTLSEntropy.c)
target_link_libraries(phocoena-switch PRIVATE core uicommon MINIZIP::minizip-ng)
# Keep line tables in the companion ELF for hardware crash symbolication.
# elf2nro omits this debug information from the SD executable.
foreach(target common core uicommon audiocommon inputcommon videocommon videoogl phocoena-switch)
  target_compile_options(${target} PRIVATE -g1)
endforeach()
if(TARGET videovulkan)
  target_compile_options(videovulkan PRIVATE -g1)
endif()
set_target_properties(phocoena-switch PROPERTIES OUTPUT_NAME Phocoena)
target_link_options(phocoena-switch PRIVATE -Wl,--gc-sections -Wl,--build-id=sha1
  -Wl,--wrap=pthread_create -Wl,--wrap=fatalThrow -Wl,--wrap=diagAbortWithResult
  -Wl,--wrap=framebufferCreate -Wl,--wrap=nwindowDequeueBuffer -Wl,--wrap=nwindowQueueBuffer
  -Wl,--wrap=nwindowReleaseBuffers -Wl,--wrap=bqDequeueBuffer -Wl,--wrap=bqRequestBuffer)
nx_generate_nacp(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/Phocoena.nacp"
  NAME "Phocoena" AUTHOR "Phocoena contributors" VERSION "${HORIZON_PORT_VERSION}")
nx_create_nro(phocoena_nro TARGET phocoena-switch
  OUTPUT "${CMAKE_BINARY_DIR}/Binaries/Phocoena.nro"
  NACP "${CMAKE_CURRENT_BINARY_DIR}/Phocoena.nacp")

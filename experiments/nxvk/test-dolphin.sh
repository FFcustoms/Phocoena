#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail
source "$(dirname "$0")/env.sh"
cd "$NXVK_REPO"
out="$NXVK_WORK/dolphin-host-tests"
mkdir -p "$out" build/nxvk/dolphin
g++ -std=c++23 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
  -Wno-missing-field-initializers -fsanitize=address,undefined \
  -DHORIZON_VULKAN -DFMT_HEADER_ONLY -ffunction-sections -fdata-sections \
  -I tests/fakes-vulkan \
  -I upstream/dolphin/Externals/fmt/fmt/include -I upstream/dolphin/Source/Core \
  -I upstream/dolphin/Externals/Vulkan-Headers/include \
  -I upstream/dolphin/Externals/VulkanMemoryAllocator/include \
  tests/vulkan_loader_test.cpp upstream/dolphin/Source/Core/VideoBackends/Vulkan/VulkanLoader.cpp \
  upstream/dolphin/Source/Core/VideoBackends/Vulkan/CommandBufferManager.cpp \
  upstream/dolphin/Source/Core/Horizon/Performance.cpp \
  -Wl,--gc-sections -pthread -ldl -o "$out/vulkan-loader-test"
export VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json
export VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
export VK_LOADER_DEBUG=layer
"$out/vulkan-loader-test" > build/nxvk/dolphin/host-loader-test.log 2>&1
if grep -E 'Validation Error|VUID-|runtime error:|ERROR:.*Sanitizer' build/nxvk/dolphin/host-loader-test.log; then
  exit 1
fi
grep -q 'Insert instance layer.*VK_LAYER_KHRONOS_validation' build/nxvk/dolphin/host-loader-test.log
tail -3 build/nxvk/dolphin/host-loader-test.log

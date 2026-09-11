#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/host-tests build/analysis/v0.1.24
g++ -std=c++23 -O2 -g -Wall -Wextra -Werror -Wno-missing-field-initializers \
  -I upstream/dolphin/Externals/Vulkan-Headers/include \
  tests/vulkan_push_descriptor_benchmark.cpp -Wl,-l:libvulkan.so.1 \
  -o build/host-tests/vulkan-push-descriptor-benchmark
export VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json
export VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
build/host-tests/vulkan-push-descriptor-benchmark | \
  tee build/analysis/v0.1.24/VULKAN-PUSH-DESCRIPTOR-BENCHMARK.txt

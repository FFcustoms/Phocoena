#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/host-tests
args=(-std=c++23 -O1 -g -pthread -Wall -Wextra -Werror -Wno-unused-parameter
  -D__SWITCH__ -DFMT_HEADER_ONLY -ffunction-sections -fdata-sections -Wl,--gc-sections
  -I tests/fakes-jit -I upstream/dolphin/Source/Core -I upstream/dolphin/Externals/fmt/fmt/include
  tests/jit_memory_test.cpp upstream/dolphin/Source/Core/Horizon/JitMemory.cpp
  upstream/dolphin/Source/Core/Horizon/JitTest.cpp
  upstream/dolphin/Source/Core/Common/Arm64Emitter.cpp)
g++ "${args[@]}" -fsanitize=address,undefined -o build/host-tests/jit-alias-test
build/host-tests/jit-alias-test
if [[ "${1:-}" == --aarch64 ]]; then
  aarch64-linux-gnu-g++ "${args[@]}" -o build/host-tests/jit-alias-test-aarch64
  qemu-aarch64 -L /usr/aarch64-linux-gnu build/host-tests/jit-alias-test-aarch64
fi

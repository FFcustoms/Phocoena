#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/host-tests
root=upstream/dolphin
args=(-std=c++23 -O3 -g -Wall -Wextra -Werror -Wno-unused-parameter -Wno-switch
  -D_M_ARM_64 -DFMT_HEADER_ONLY -ffunction-sections -fdata-sections -Wl,--gc-sections
  -I "$root/Source/Core" -I "$root/Externals/fmt/fmt/include"
  tests/vertex_loader_arm64_benchmark.cpp
  "$root/Source/Core/Common/Arm64Emitter.cpp"
  "$root/Source/Core/VideoCommon/VertexLoaderBase.cpp"
  "$root/Source/Core/VideoCommon/VertexLoader.cpp"
  "$root/Source/Core/VideoCommon/VertexLoaderARM64.cpp"
  "$root/Source/Core/VideoCommon/VertexLoader_Color.cpp"
  "$root/Source/Core/VideoCommon/VertexLoader_Normal.cpp"
  "$root/Source/Core/VideoCommon/VertexLoader_Position.cpp"
  "$root/Source/Core/VideoCommon/VertexLoader_TextCoord.cpp")
aarch64-linux-gnu-g++ "${args[@]}" -o build/host-tests/vertex-loader-arm64-benchmark
qemu-aarch64 -L /usr/aarch64-linux-gnu build/host-tests/vertex-loader-arm64-benchmark |
  tee build/host-tests/vertex-loader-arm64-benchmark.log

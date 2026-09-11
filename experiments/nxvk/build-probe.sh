#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail
source "$(dirname "$0")/env.sh"
python3 "$NXVK_REPO/experiments/nxvk/prepare-probe.py"
out="$NXVK_WORK/probe-switch"
core="$NXVK_REPO/upstream/dolphin/Source/Core"
probe="$NXVK_REPO/experiments/nxvk/probe"
gcc="$DEVKITA64/bin/aarch64-none-elf-gcc"
gxx="$DEVKITA64/bin/aarch64-none-elf-g++"
flags=(-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
  -O2 -g1 -ffunction-sections -fdata-sections -D__SWITCH__ -D_GNU_SOURCE -D_DEFAULT_SOURCE
  -Wall -Wextra -Werror -Wno-missing-field-initializers
  -I "$out" -I "$core" -I "$SRC/include" -I "$DEVKITPRO/libnx/include")
glslangValidator -V --target-env vulkan1.0 --vn triangle_vert -o "$out/triangle_vert.h" "$probe/triangle.vert"
glslangValidator -V --target-env vulkan1.0 --vn triangle_frag -o "$out/triangle_frag.h" "$probe/triangle.frag"
for file in "$probe/Probe.cpp" "$probe/SwitchMain.cpp" "$out/Console.cpp" "$core/Horizon/Display.cpp" "$core/Horizon/PThread.cpp"; do
  "$gxx" -std=c++20 "${flags[@]}" -c "$file" -o "$out/$(basename "${file%.cpp}").o"
done
"$gcc" "${flags[@]}" -c "$out/Crash.c" -o "$out/Crash.o"
"$gcc" "${flags[@]}" -c "$SRC/switch/smoke/nvk_compat.c" -o "$out/nvk_compat.o"
if [[ "${1:-}" == --compile-only ]]; then
  printf 'ARM64 diagnostic objects compiled; link not attempted\n'
  exit 0
fi
mapfile -t archives < <(python3 "$NXVK_REPO/experiments/nxvk/list-archives.py" "$SRC/Makefile")
cd "$SRC/switch/build/cross"
"$gxx" -specs="$DEVKITPRO/libnx/switch.specs" -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE \
  -L "$DEVKITPRO/portlibs/switch/lib" -L "$DEVKITPRO/libnx/lib" \
  -Wl,--gc-sections -Wl,--build-id=sha1 -Wl,-u,vk_icdGetInstanceProcAddr \
  -Wl,--wrap=pthread_create -Wl,--wrap=fatalThrow -Wl,--wrap=diagAbortWithResult \
  -Wl,--wrap=framebufferCreate -Wl,--wrap=nwindowDequeueBuffer -Wl,--wrap=nwindowQueueBuffer \
  -Wl,--wrap=nwindowReleaseBuffers -Wl,--wrap=bqDequeueBuffer -Wl,--wrap=bqRequestBuffer \
  "$out/Probe.o" "$out/SwitchMain.o" "$out/Console.o" "$out/Display.o" "$out/PThread.o" "$out/Crash.o" "$out/nvk_compat.o" \
  -Wl,--whole-archive "${archives[0]}" -Wl,--no-whole-archive \
  -Wl,--start-group "${archives[@]:1}" -lz -lexpat -lnx -lc -lm -lstdc++ -pthread -Wl,--end-group \
  -o "$out/vulkan-probe.elf"
"$DEVKITPRO/tools/bin/nacptool" --create "Vulkan Probe for Dolphin" "Dolphin Horizon / NXVK" "0.1.0" "$out/probe.nacp"
"$DEVKITPRO/tools/bin/elf2nro" "$out/vulkan-probe.elf" "$out/vulkan-probe.nro" \
  --icon="$DEVKITPRO/libnx/default_icon.jpg" --nacp="$out/probe.nacp"
mkdir -p "$NXVK_REPO/build/nxvk/Binaries"
cp "$out/vulkan-probe.elf" "$out/vulkan-probe.nro" "$out/source-inputs.json" "$NXVK_REPO/build/nxvk/Binaries/"
python3 "$NXVK_REPO/scripts/verify-nro.py" "$out/vulkan-probe.nro" | tee "$NXVK_REPO/build/nxvk/nro-verification.json"

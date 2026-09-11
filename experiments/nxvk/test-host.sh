#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail
source "$(dirname "$0")/env.sh"
out="$NXVK_WORK/probe-host"
mkdir -p "$out"
glslangValidator -V --target-env vulkan1.0 --vn triangle_vert \
  -o "$out/triangle_vert.h" "$NXVK_REPO/experiments/nxvk/probe/triangle.vert"
glslangValidator -V --target-env vulkan1.0 --vn triangle_frag \
  -o "$out/triangle_frag.h" "$NXVK_REPO/experiments/nxvk/probe/triangle.frag"
g++ -std=c++20 -O1 -g -Wall -Wextra -Werror -Wno-missing-field-initializers \
  -fsanitize=address,undefined -I "$out" -I "$SRC/include" \
  -I "$NXVK_REPO/upstream/dolphin/Source/Core" \
  "$NXVK_REPO/experiments/nxvk/probe/Probe.cpp" \
  "$NXVK_REPO/experiments/nxvk/probe/HostMain.cpp" -ldl -o "$out/probe"
export VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json
export VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
export VK_LOADER_DEBUG=layer
# Lavapipe is the host software driver; do not mistake this for Switch GPU testing.
"$out/probe" > "$NXVK_REPO/build/nxvk/host-probe.log" 2>&1
if grep -E 'Validation Error|VUID-|runtime error:|ERROR:.*Sanitizer' "$NXVK_REPO/build/nxvk/host-probe.log"; then
  exit 1
fi
grep -q 'Insert instance layer.*VK_LAYER_KHRONOS_validation' "$NXVK_REPO/build/nxvk/host-probe.log"
tail -6 "$NXVK_REPO/build/nxvk/host-probe.log"

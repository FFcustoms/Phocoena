#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail
source "$(dirname "$0")/env.sh"
stage="${1:?usage: build.sh tools|std|native|cross|archives|samples|probe}"
case "$stage" in tools|std|native|cross|archives|samples|probe) ;; *) exit 2 ;; esac
cd "$SRC"
log="$NXVK_WORK/logs/$stage.log"
# Keep the full compiler output and exit code. Do not hide a failed .so link;
# only build static archive targets, which are what Horizon actually uses.
exec > >(tee "$log") 2>&1
trap 'result=$?; cp "$log" "$NXVK_REPO/build/nxvk/$stage.log"; exit "$result"' EXIT
case "$stage" in
tools)
  cargo install --locked --version 0.72.1 -j "${JOBS:-4}" bindgen-cli
  cargo install --locked --version 0.29.4 -j "${JOBS:-4}" cbindgen
  rustc -Vv
  cargo -V
  bindgen --version
  cbindgen --version
  ;;
std)
  bash "$NXVK_REPO/experiments/nxvk/build-std.sh"
  ;;
native)
  bash switch/build/build-native-tools.sh
  ;;
cross)
  bash switch/build/configure-mesa.sh
  ;;
archives)
  export PATH="$SRC/switch/build/native-tools/bin:$PATH"
  # Preserve upstream's authoritative target list without executing its
  # container, installation or cleaning rules.
  mapfile -t archives < <(python3 "$NXVK_REPO/experiments/nxvk/list-archives.py" "$SRC/Makefile")
  (( ${#archives[@]} > 10 ))
  ninja -j "${JOBS:-4}" -C switch/build/cross "${archives[@]}"
  ;;
samples)
  bash switch/smoke/shaders/gen-shaders.sh
  for sample in nvk_smoke nvk_tri nvk_vi_swapchain; do
    bash switch/build/build-nro.sh "$sample"
  done
  ;;
probe)
  bash "$NXVK_REPO/experiments/nxvk/build-probe.sh"
  ;;
esac

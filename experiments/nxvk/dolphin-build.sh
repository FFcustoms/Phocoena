#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Separate Vulkan cache; its legacy directory name is not the release version.
# Matching ELF/NRO artifacts are frozen by record-dolphin-build.py per release.
set -euo pipefail
source "$(dirname "$0")/env.sh"
cd "$NXVK_REPO"
expected=69ec283dbda64e65347a36274efb349122e85363
if [[ -e "$SRC/.git" ]]; then
  actual=$(git -C "$SRC" rev-parse HEAD)
else
  actual=$(cat "$SRC/NXVK_UPSTREAM_COMMIT")
fi
[[ "$actual" == "$expected" ]] || { echo 'ERROR: NXVK revision mismatch' >&2; exit 1; }
version=$(sed -n 's/^set(HORIZON_PORT_VERSION "\([0-9.]*\)").*/\1/p' upstream/dolphin/Source/Core/Horizon/CMakeLists.txt)
[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo 'ERROR: invalid port version' >&2; exit 1; }
native="$NXVK_WORK/dolphin-v$version"
mkdir -p "$native/source" "$native/build" build/nxvk/dolphin
if [[ ! -f "$native/source-ready" ]]; then
  rsync -a --exclude=.git upstream/dolphin/ "$native/source/"
  touch "$native/source-ready"
else
  python3 scripts/changed-source-files.py > "$native/changed-files"
  rsync -a --files-from="$native/changed-files" upstream/dolphin/ "$native/source/"
  while IFS= read -r relative; do
    [[ -z "$relative" ]] || touch "$native/source/$relative"
  done < "$native/changed-files"
fi
python3 experiments/nxvk/list-archives.py "$SRC/Makefile" > "$native/archives-relative.txt"
while IFS= read -r archive; do
  test -s "$SRC/switch/build/cross/$archive"
  printf '%s\n' "$SRC/switch/build/cross/$archive"
done < "$native/archives-relative.txt" > "$native/archives.txt"
export DOLPHIN_SOURCE_DIR="$native/source"
export DOLPHIN_BUILD_DIR="$native/build"
export JOBS="${JOBS:-6}"
if git -C "$NXVK_REPO" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  PHOCOENA_COMMIT=$(git -C "$NXVK_REPO" rev-parse HEAD)
  if [[ -n "$(git -C "$NXVK_REPO" status --porcelain --untracked-files=normal)" ]]; then
    PHOCOENA_SOURCE_STATE=dirty
  else
    PHOCOENA_SOURCE_STATE=clean
  fi
else
  PHOCOENA_COMMIT=unknown
  PHOCOENA_SOURCE_STATE=unknown
fi
trap 'cp "$native/build/"*.log build/nxvk/dolphin/ 2>/dev/null || true; if [[ -d "$native/build/Binaries" ]]; then rsync -a "$native/build/Binaries/" build/nxvk/dolphin/Binaries/; fi' EXIT
bash scripts/build-switch.sh \
  -DDISTRIBUTOR="Atmosphere-v$version-NXVK" \
  -DHORIZON_RENDERER=Vulkan \
  -DHORIZON_NXVK_SOURCE="$SRC" -DHORIZON_NXVK_ARCHIVES="$native/archives.txt" \
  -DHORIZON_PHOCOENA_COMMIT="$PHOCOENA_COMMIT" \
  -DHORIZON_PHOCOENA_SOURCE_STATE="$PHOCOENA_SOURCE_STATE" \
  -DDOLPHIN_WC_REVISION=4f8af23db516d8b6e9cd00e7b261a65b026514a8 \
  -DDOLPHIN_WC_DESCRIBE="Horizon-v$version-NXVK" -DDOLPHIN_WC_BRANCH="horizon-v$version" "$@"

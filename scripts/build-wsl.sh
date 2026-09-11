#!/usr/bin/env bash
# Native WSL scratch directory avoids extremely slow C++ header I/O over /mnt/c.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$PWD"
# /tmp is cleared when this WSL instance stops; /var/tmp preserves incremental objects.
NATIVE="${HORIZON_WSL_CACHE:-/var/tmp/dolphin-atmosphere-port-v01-$UID}"
mkdir -p "$NATIVE/source" "$NATIVE/devkitpro" "$NATIVE/build" build/switch
if [[ ! -f "$NATIVE/toolchain.lock.json" ]]; then
  rsync -a .toolchain/image/opt/devkitpro/ "$NATIVE/devkitpro/"
  cp toolchain.lock.json "$NATIVE/toolchain.lock.json"
elif ! cmp -s toolchain.lock.json "$NATIVE/toolchain.lock.json"; then
  echo 'ERROR: native scratch toolchain differs; choose a fresh scratch directory.' >&2
  exit 1
fi
if [[ ! -f "$NATIVE/source-ready" ]]; then
  rsync -a --exclude=.git upstream/dolphin/ "$NATIVE/source/"
  touch "$NATIVE/source-ready"
else
  # Dependencies are immutable at the pinned revisions. Avoid walking their
  # tens of thousands of unchanged files on /mnt/c for every compile iteration.
  python3 scripts/changed-source-files.py > "$NATIVE/changed-files"
  rsync -a --files-from="$NATIVE/changed-files" upstream/dolphin/ "$NATIVE/source/"
  while IFS= read -r relative; do
    [[ -z "$relative" ]] || touch "$NATIVE/source/$relative"
  done < "$NATIVE/changed-files"
fi
export DEVKITPRO="$NATIVE/devkitpro"
export DOLPHIN_SOURCE_DIR="$NATIVE/source"
export DOLPHIN_BUILD_DIR="$NATIVE/build"
trap 'mkdir -p build/switch/native; cp "$NATIVE/build/"*.log build/switch/native/ 2>/dev/null || true; if [[ -d "$NATIVE/build/Binaries" ]]; then rsync -a "$NATIVE/build/Binaries/" build/switch/native/Binaries/; fi' EXIT
bash scripts/build-switch.sh \
  -DDOLPHIN_WC_REVISION=4f8af23db516d8b6e9cd00e7b261a65b026514a8 \
  -DDOLPHIN_WC_DESCRIBE=Horizon-v0.1.9 -DDOLPHIN_WC_BRANCH=horizon-v0.1.9 "$@"

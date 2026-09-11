#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$PWD"
export DEVKITPRO="${DEVKITPRO:-$ROOT/.toolchain/image/opt/devkitpro}"
export DEVKITA64="$DEVKITPRO/devkitA64"
export PATH="$DEVKITA64/bin:$DEVKITPRO/tools/bin:$ROOT/.toolchain/host/cmake/data/bin:$ROOT/.toolchain/host/bin:$PATH"
export SOURCE_DATE_EPOCH=1787875200
export PKG_CONFIG_PATH="$DEVKITPRO/portlibs/switch/lib/pkgconfig"
SOURCE_DIR="${DOLPHIN_SOURCE_DIR:-$ROOT/upstream/dolphin}"
BUILD_DIR="${DOLPHIN_BUILD_DIR:-$ROOT/build/switch}"
PATCH_ID=$(python3 -c 'import hashlib,pathlib; print(hashlib.sha256(pathlib.Path("source.lock.json").read_bytes()).hexdigest())')
mkdir -p "$BUILD_DIR"
cmake -S "$SOURCE_DIR" -B "$BUILD_DIR" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" \
  -DPKG_CONFIG_EXECUTABLE="$ROOT/scripts/switch-pkg-config.py" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DDISTRIBUTOR=Atmosphere-v0.1.11 -DENABLE_LTO=OFF \
  -DHORIZON_PATCHSET_SHA256="$PATCH_ID" \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON "$@" 2>&1 | tee "$BUILD_DIR/configure.log"
cmake --build "$BUILD_DIR" --target phocoena_nro --parallel "${JOBS:-8}" -- -k 0 2>&1 | tee "$BUILD_DIR/build.log"

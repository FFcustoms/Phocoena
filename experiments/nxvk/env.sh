#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Source this from scripts in this directory. Does not alter shell startup files.
NXVK_REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NXVK_WORK="${NXVK_WORK:-/var/tmp/dolphin-nxvk-69ec283d}"
export NXVK_WORK
export SRC="$NXVK_WORK/source"
export DEVKITPRO="$NXVK_WORK/devkitpro"
export DEVKITA64="$DEVKITPRO/devkitA64"
export RUSTUP_HOME="$NXVK_WORK/rustup"
export CARGO_HOME="$NXVK_WORK/cargo"
export RUSTUP_TOOLCHAIN=nightly-2026-07-02
export RUST_NIGHTLY="$RUSTUP_TOOLCHAIN"
export LIBCLANG_PATH=/usr/lib/llvm-19/lib
export LLVM_CONFIG=/usr/bin/llvm-config-19
export PYTHONPATH="$NXVK_WORK/python:$NXVK_REPO/.toolchain/host"
export PATH="$NXVK_WORK/python/bin:$CARGO_HOME/bin:$NXVK_REPO/.toolchain/host/bin:/usr/lib/llvm-19/bin:$DEVKITPRO/tools/bin:$PATH"
export SOURCE_DATE_EPOCH=1787875200
mkdir -p "$NXVK_REPO/build/nxvk" "$NXVK_WORK/logs"

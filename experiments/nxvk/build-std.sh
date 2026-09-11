#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Original sysroot recipe Copyright 2026 PalindromicBreadLoaf.
# Adapted from NXVK switch/rust/build-std-sysroot.sh, without global /tmp paths
# or recursive deletion. Each Rust nightly uses its own scratch/sysroot.
set -euo pipefail
source "$(dirname "$0")/env.sh"
export RUST_TARGET_PATH="$SRC/switch/rust" RUSTC_BOOTSTRAP=1
export RUSTFLAGS="-Zunstable-options -L $DEVKITA64/aarch64-none-elf/lib -L $DEVKITPRO/libnx/lib"
target=aarch64-switch-horizon
crate="$NXVK_WORK/std-crate"
sysroot="$SRC/switch/rust/sysroot"
mkdir -p "$crate/src" "$sysroot/lib/rustlib/$target/lib"
printf '#![no_main]\n' > "$crate/src/lib.rs"
cat > "$crate/Cargo.toml" <<'EOF'
[package]
name = "nxvk-std-bootstrap"
version = "0.0.0"
edition = "2021"
[lib]
crate-type = ["rlib"]
[profile.release]
panic = "abort"
EOF
cd "$crate"
cargo build --release -j "${JOBS:-4}" -Zbuild-std=core,alloc,std,panic_abort --target "$target"
find "target/$target/release/deps" -maxdepth 1 \( -name '*.rlib' -o -name '*.rmeta' \) \
  -exec cp -t "$sysroot/lib/rustlib/$target/lib" {} +
rust_sysroot="$(rustc --print sysroot)"
for directory in "$rust_sysroot/lib/rustlib/"*; do
  base="$(basename "$directory")"
  [[ "$base" == "$target" || -e "$sysroot/lib/rustlib/$base" ]] && continue
  ln -s "$directory" "$sysroot/lib/rustlib/$base"
done
printf 'pub fn n() -> usize { let v = vec![1, 2, 3]; v.len() }\n' > "$crate/probe.rs"
rustc -Zunstable-options --target "$target" --sysroot "$sysroot" \
  --crate-type=rlib -C panic=abort "$crate/probe.rs" -o "$crate/probe.rlib"
printf 'Switch Rust sysroot compile probe PASS (not executed)\n'

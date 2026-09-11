#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Requires the host packages in README.md. Never installs system packages or
# changes the normal Dolphin SDK. Network downloads use the pinned revisions.
set -euo pipefail
source "$(dirname "$0")/env.sh"
commit=69ec283dbda64e65347a36274efb349122e85363
if [[ ! -d "$SRC/.git" && ! -f "$SRC/NXVK_UPSTREAM_COMMIT" ]]; then
  if [[ -d "$SRC" && -n "$(ls -A "$SRC")" ]]; then
    printf 'ERROR: refusing to replace an unidentified source directory\n' >&2
    exit 1
  fi
  git init "$SRC"
  git -C "$SRC" fetch --depth 1 https://github.com/PalindromicBreadLoaf/nxvk.git "$commit"
  git -C "$SRC" checkout --detach FETCH_HEAD
fi
if [[ ! -f "$NXVK_REPO/.toolchain/image/opt/devkitpro/libnx/lib/libnx.a" && ! -f "$DEVKITPRO/libnx/lib/libnx.a" ]]; then
  python3 "$NXVK_REPO/scripts/bootstrap-toolchain.py"
fi
if [[ ! -f "$NXVK_REPO/.toolchain/host/bin/ninja" ]]; then
  bash "$NXVK_REPO/scripts/bootstrap-host.sh"
fi
python3 "$NXVK_REPO/experiments/nxvk/prepare.py"
mkdir -p "$NXVK_WORK/downloads"
installer="$NXVK_WORK/downloads/rustup-init"
if [[ ! -f "$installer" ]]; then
  curl --fail --location --proto '=https' --tlsv1.2 \
    -o "$installer" https://static.rust-lang.org/rustup/archive/1.28.2/x86_64-unknown-linux-gnu/rustup-init
fi
printf '20a06e644b0d9bd2fbdbfd52d42540bdde820ea7df86e92e533c073da0cdd43c  %s\n' "$installer" | sha256sum --check
chmod +x "$installer"
if [[ ! -x "$CARGO_HOME/bin/rustup" ]]; then
  "$installer" -y --no-modify-path --default-toolchain "$RUSTUP_TOOLCHAIN" --profile minimal --component rust-src,rustfmt
fi
rustup toolchain install "$RUSTUP_TOOLCHAIN" --profile minimal --component rust-src,rustfmt
bash "$NXVK_REPO/experiments/nxvk/host-tools.sh"
bash "$NXVK_REPO/experiments/nxvk/build.sh" tools
bash "$NXVK_REPO/experiments/nxvk/build.sh" std

#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

python3 scripts/verify-tester-baseline.py
printf '==> Bootstrapping pinned toolchain/NXVK dependencies\n'
bash experiments/nxvk/bootstrap.sh
printf '==> Building NXVK native tools\n'
bash experiments/nxvk/build.sh native
printf '==> Configuring the Switch NXVK build\n'
bash experiments/nxvk/build.sh cross
printf '==> Building the static NXVK archives\n'
bash experiments/nxvk/build.sh archives
printf '==> Building Phocoena\n'
bash experiments/nxvk/dolphin-build.sh

NRO="$ROOT/build/nxvk/dolphin/Binaries/Phocoena.nro"
if [[ ! -f "$NRO" ]]; then
  printf 'ERROR: build completed without %s\n' "$NRO" >&2
  exit 1
fi
printf '==> Verifying NRO structure and metadata\n'
python3 scripts/verify-nro.py "$NRO"
printf '\nPhocoena build complete: %s\n' "$NRO"

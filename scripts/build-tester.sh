#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
python3 scripts/verify-tester-baseline.py
bash scripts/build-phocoena.sh
python3 scripts/package-testers.py --build build/nxvk/dolphin
printf '\nTester package complete: %s\n' "$ROOT/dist/Phocoena-v0.1.27-tester.zip"

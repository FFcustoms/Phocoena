#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail
source "$(dirname "$0")/env.sh"
python3 -m pip install --disable-pip-version-check --target "$NXVK_WORK/python" \
  --report "$NXVK_REPO/build/nxvk/python-install.json" \
  meson==1.12.0 mako==1.4.1 pyyaml==6.0.3 MarkupSafe==3.0.3
python3 -m pip list --path "$NXVK_WORK/python" --format=json > "$NXVK_REPO/build/nxvk/python-packages.json"

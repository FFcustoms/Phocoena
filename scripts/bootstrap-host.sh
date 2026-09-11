#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
# --target keeps host dependencies in this repository, not the system Python.
python3 -m pip install --disable-pip-version-check --target .toolchain/host \
  cmake==4.4.2 ninja==1.13.0

#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
export PATH="$PWD/.toolchain/host/cmake/data/bin:$PWD/.toolchain/host/bin:$PATH"
cmake -S tests/upstream-common -B build/upstream-tests -G Ninja -DCMAKE_BUILD_TYPE=Debug "$@"
cmake --build build/upstream-tests --parallel 4
build/upstream-tests/upstream-common-tests --gtest_output=xml:build/upstream-tests/results.xml

#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/host-tests
g++ -std=c++20 -O3 -Wall -Wextra -Werror \
  -I upstream/dolphin/Source/Core tests/fifo_batch_test.cpp \
  -o build/host-tests/fifo-batch-benchmark
build/host-tests/fifo-batch-benchmark --benchmark

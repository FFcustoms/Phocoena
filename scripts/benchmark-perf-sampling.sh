#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/host-tests
g++ -std=c++20 -O3 -DNDEBUG -Wall -Wextra -Werror \
  -I upstream/dolphin/Source/Core tests/performance_sampling_benchmark.cpp \
  upstream/dolphin/Source/Core/Horizon/Performance.cpp \
  -o build/host-tests/performance-sampling-benchmark
for scale in 64 1024; do
  for run in 1 2 3 4 5; do
    printf 'run=%s ' "$run"
    build/host-tests/performance-sampling-benchmark "$scale"
  done
done

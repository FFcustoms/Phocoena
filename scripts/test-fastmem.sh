#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/build/host/fastmem"
mkdir -p "$OUT"

${CXX:-g++} -std=c++23 -O1 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  "$ROOT/tests/horizon_fastmem_model_test.cpp" -o "$OUT/horizon_fastmem_model_test"
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
  "$OUT/horizon_fastmem_model_test" | tee "$OUT/correctness.log"

${CXX:-g++} -std=c++23 -O1 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$ROOT/upstream/dolphin/Source/Core" \
  "$ROOT/tests/fastmem_stats_test.cpp" \
  "$ROOT/upstream/dolphin/Source/Core/Horizon/Fastmem.cpp" \
  -o "$OUT/fastmem_stats_test"
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
  "$OUT/fastmem_stats_test" | tee "$OUT/stats.log"

${CXX:-g++} -std=c++23 -O3 -DNDEBUG -Wall -Wextra -Werror \
  "$ROOT/tests/horizon_fastmem_model_test.cpp" -o "$OUT/horizon_fastmem_benchmark"
"$OUT/horizon_fastmem_benchmark" --benchmark | tee "$OUT/benchmark.log"

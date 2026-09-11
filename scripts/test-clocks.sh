#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/clock-tests
g++ -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I upstream/dolphin/Source/Core tests/clock_policy_test.cpp \
  upstream/dolphin/Source/Core/Horizon/ClockPolicy.cpp \
  -o build/clock-tests/clock-policy-test
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
  build/clock-tests/clock-policy-test
python3 -m unittest tests.test_clock_safety -v

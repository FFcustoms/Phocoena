#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/host-tests
TEST_DIR=$(mktemp -d "$PWD/build/host-tests/case.XXXXXX")
scripts/test-clocks.sh
g++ -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I upstream/dolphin/Source/Core tests/frontend_model_test.cpp \
  upstream/dolphin/Source/Core/Horizon/Performance.cpp -o build/host-tests/frontend-model-test
build/host-tests/frontend-model-test "$TEST_DIR"
g++ -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I tests/fakes -I upstream/dolphin/Source/Core tests/console_handoff_test.cpp \
  upstream/dolphin/Source/Core/Horizon/Console.cpp upstream/dolphin/Source/Core/Horizon/Display.cpp \
  -o build/host-tests/console-handoff-test
build/host-tests/console-handoff-test

# The Vulkan build uses the same ownership guards with a different handoff label.
g++ -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  -DHORIZON_VULKAN -I tests/fakes -I upstream/dolphin/Source/Core \
  tests/console_handoff_test.cpp upstream/dolphin/Source/Core/Horizon/Console.cpp \
  upstream/dolphin/Source/Core/Horizon/Display.cpp -o build/host-tests/vulkan-console-handoff-test
build/host-tests/vulkan-console-handoff-test
g++ -std=c++20 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -fsanitize=address,undefined \
  -DFMT_HEADER_ONLY -I upstream/dolphin/Externals/fmt/fmt/include \
  -I upstream/dolphin/Source/Core tests/cached_interpreter_memory_test.cpp \
  upstream/dolphin/Source/Core/Core/PowerPC/CachedInterpreter/CachedInterpreterEmitter.cpp \
  -o build/host-tests/cached-interpreter-memory-test
build/host-tests/cached-interpreter-memory-test
g++ -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I upstream/dolphin/Source/Core tests/fifo_batch_test.cpp \
  -o build/host-tests/fifo-batch-test
build/host-tests/fifo-batch-test
python3 -m unittest discover -s tests -p 'test_*.py' -v

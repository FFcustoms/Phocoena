#!/usr/bin/env python3
"""Fail the build if the frozen Phocoena tester baseline was accidentally changed."""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
checks = []

def require(path, text, label=None):
    data = (ROOT / path).read_text(encoding='utf-8')
    if text not in data:
        raise SystemExit(f'BASELINE CHECK FAILED: {label or text!r} missing from {path}')
    checks.append(label or text)

def forbid(path, text, label=None):
    data = (ROOT / path).read_text(encoding='utf-8')
    if text in data:
        raise SystemExit(f'BASELINE CHECK FAILED: forbidden {label or text!r} found in {path}')
    checks.append('no ' + (label or text))

require('upstream/dolphin/Source/Core/Horizon/ClockPolicy.h', "PERFORMANCE_GPU_TARGET_HZ = 768'000'000", 'GPU target 768 MHz')
require('upstream/dolphin/Source/Core/Horizon/ClockPolicy.h', "PERFORMANCE_CPU_TARGET_HZ = 1'785'000'000", 'CPU target 1785 MHz')
require('upstream/dolphin/Source/Core/Horizon/Main.cpp', '.shader_mode = "Sync specialized"', 'synchronous specialized shader mode')
require('upstream/dolphin/Source/Core/Horizon/CMakeLists.txt', 'add_executable(phocoena-switch', 'Phocoena Switch target')
require('upstream/dolphin/Source/Core/Horizon/CMakeLists.txt', 'OUTPUT "${CMAKE_BINARY_DIR}/Binaries/Phocoena.nro"', 'Phocoena.nro output')
require('upstream/dolphin/Source/Core/Horizon/CMakeLists.txt', 'NAME "Phocoena"', 'Phocoena NACP title')
require('upstream/dolphin/Source/Core/Horizon/Log.h', 'sdmc:/switch/Phocoena/', 'Phocoena SD root')
for bad in ('921600000', "921'600'000", 'AsynchronousUberShaders', 'async_shaders=1'):
    for path in ('upstream/dolphin/Source/Core/Horizon/Main.cpp',
                 'upstream/dolphin/Source/Core/Horizon/ClockPolicy.h',
                 'scripts/package-testers.py'):
        forbid(path, bad)
print(f'Frozen tester baseline: PASS ({len(checks)} checks)')

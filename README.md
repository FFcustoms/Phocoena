# Phocoena

Phocoena is an experimental Nintendo GameCube emulator for Nintendo Switch homebrew, based on Dolphin Emulator.

It runs natively on Horizon OS and currently uses Dolphin's AArch64 core with a Vulkan/NXVK graphics path.

> **Current status:** GameCube is the active working target. Wii support is not currently considered working or supported.

## Get Started

**Want to run Phocoena?**
See [INSTALL.md](INSTALL.md).

**Testing Phocoena?**
See [SWITCH_TESTING.md](SWITCH_TESTING.md).

**Want to build it yourself?**
See [BUILDING.md](BUILDING.md).

**Having a problem?**
See [KNOWN_ISSUES.md](KNOWN_ISSUES.md).

## Current Release

Current development/test release:

**Phocoena v0.1.27**

Phocoena is still experimental. Compatibility and performance vary between games, and crashes or incomplete emulation should be expected.

Current tester baseline:

- AArch64 JIT
- Dual Core
- Vulkan through NXVK
- 1x internal resolution
- Synchronous specialized shaders
- FIFO batch size 8
- CPU target: 1785 MHz
- GPU target: 768 MHz
- Memory clock is never written by Phocoena

## Quick Install

Download the latest tester ZIP from GitHub Releases and extract it to the root of the Switch SD card.

Phocoena should end up at:

`SD:/switch/Phocoena/Phocoena.nro`

Full instructions are in [INSTALL.md](INSTALL.md).

## Game Support

GameCube is the current focus.

Supported GameCube image formats include:

- RVZ
- ISO
- GCM

Use your own legally obtained game dumps.

Wii support is not currently considered working or supported.

## Tester Diagnostics

Phocoena automatically records diagnostic sessions while games are running.

Support bundles can be generated from:

**About / Diagnostics -> X**

They are saved under:

`SD:/switch/Phocoena/diagnostics/bundles/`

The newest bundle is marked `UNSENT_`.

After another successful bundle is generated, older `UNSENT_` bundles are changed to `SENT_`.

This makes it easier for testers to tell which report still needs to be returned.

See [SWITCH_TESTING.md](SWITCH_TESTING.md) for the full testing workflow.

## Building From Source

The modified Dolphin source used to build Phocoena is included in this repository.

The current build produces:

`build/nxvk/dolphin/Binaries/Phocoena.nro`

See [BUILDING.md](BUILDING.md) for setup and build instructions.

## Repository Layout

- `upstream/dolphin/` - Dolphin source with the Phocoena Horizon port
- `experiments/nxvk/` - NXVK/Vulkan integration and build scripts
- `scripts/` - Build, verification and packaging tools
- `patches/` - Source and reproducibility patches
- `tests/` - Host-side and regression tests
- `docs/` - Attribution and project documentation

## Source and Licensing

Phocoena is based on Dolphin Emulator.

The current upstream Dolphin revision used by Phocoena is:

`4f8af23db516d8b6e9cd00e7b261a65b026514a8`

NXVK is currently pinned to:

`69ec283dbda64e65347a36274efb349122e85363`

Phocoena retains the applicable Dolphin, NXVK, Mesa and third-party licensing requirements.

See:

- [COPYING](COPYING)
- `upstream/dolphin/LICENSES/`
- the license notices distributed with release packages

Dolphin Emulator: https://dolphin-emu.org/

Dolphin source: https://github.com/dolphin-emu/dolphin

## Legal

Phocoena does not include Nintendo game images, encryption keys, firmware, NAND data or copyrighted game assets.

Users are responsible for supplying their own legally obtained game dumps and complying with applicable laws.

## Project Status

Phocoena is experimental software under active development.

Testing, compatibility reports and generated Phocoena support bundles are welcome.

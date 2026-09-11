# NXVK / Vulkan Build Integration

This directory contains the NXVK/Vulkan build integration used by the current Phocoena Horizon build.

It is no longer a standalone Vulkan probe workflow. The current Phocoena build uses NXVK as its Vulkan driver path.

## Current Pin

NXVK:

`69ec283dbda64e65347a36274efb349122e85363`

Mesa base:

`26.1.4`

The default build workspace is:

`/var/tmp/dolphin-nxvk-69ec283d`

## Host Environment

The currently tested environment is Ubuntu 26.04 under WSL on x86-64 Windows.

General requirements include:

- Git
- Python
- Bash
- make
- pkg-config
- rsync
- curl
- GCC/G++
- LLVM 19
- SPIR-V tools
- Mesa/DRM development packages

The current NXVK bootstrap expects the host packages used by the pinned build environment.

## First-Time Setup

From the Phocoena repository root:

`bash experiments/nxvk/bootstrap.sh`

Then build the required NXVK components:

`bash experiments/nxvk/build.sh native`

`bash experiments/nxvk/build.sh cross`

`bash experiments/nxvk/build.sh archives`

The bootstrap process fetches the pinned NXVK revision and prepares the isolated build workspace.

## Build Phocoena

After NXVK setup:

`python3 scripts/verify-tester-baseline.py`

Then:

`bash experiments/nxvk/dolphin-build.sh`

The resulting NRO is copied to:

`build/nxvk/dolphin/Binaries/Phocoena.nro`

See the repository root [BUILDING.md](../../BUILDING.md) for the complete build process.

## Custom Workspace

`NXVK_WORK` may be used to choose another absolute Linux build directory.

Do not point it at the Dolphin source tree or an unrelated existing build directory.

Example:

`NXVK_WORK=/var/tmp/phocoena-nxvk bash experiments/nxvk/bootstrap.sh`

## Licensing

NXVK, Mesa and their dependencies retain their respective licenses.

The Phocoena tester packaging process includes the required NXVK/Mesa license notices with distributed builds.

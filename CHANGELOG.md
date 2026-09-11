# Changelog

This changelog tracks public Phocoena releases.

## v0.1.27

Initial public tester release.

### Emulator

- GameCube emulation is the current supported target.
- Dolphin-based AArch64 JIT core.
- Dual Core enabled in the tester baseline.
- Vulkan rendering through NXVK.
- 1x internal resolution.
- FIFO batch size 8.
- Synchronous specialized shaders.

### Switch Integration

- Native Horizon homebrew frontend.
- Game browser for RVZ, ISO and GCM images.
- Joy-Con and compatible controller input.
- Automatic CPU/GPU performance profile.
- Phocoena does not write the Switch memory clock.

### Diagnostics

- Per-game diagnostic sessions.
- Support bundle generation from About / Diagnostics.
- New support bundles are marked `UNSENT_`.
- Previous `UNSENT_` bundles become `SENT_` after a new bundle is successfully generated.
- Generating with no new game sessions leaves existing bundle names unchanged.

### Packaging

- SD-ready tester ZIP.
- Dolphin Sys data included.
- Required Dolphin and third-party license notices included.
- Games, saves, keys and Nintendo account data are not included.

## Earlier Development

Phocoena went through numerous private development builds before v0.1.27 while the Horizon frontend, AArch64 execution, Vulkan/NXVK integration, graphics path, performance work and diagnostics system were being developed.

Those internal development checkpoints are not treated as public releases.

# Source attribution

The emulator is the Dolphin Emulator Project, licensed GPL-2.0-or-later.
Upstream copyright notices and bundled third-party licenses are retained.

- Primary: `dolphin-emu/dolphin`, commit
  `4f8af23db516d8b6e9cd00e7b261a65b026514a8`.
- Historical platform reference: `xerpi/dolphin-switch`, commit
  `2079cf564f251fadcb5bb305c7c2faa9e8b8789c`.
- Modern libnx reference: `NaGaa95/dolphin-nx`, commit
  `84535ced59025dcfb2edd479c1b314325a0c9ddc`.
  The guarded libnx additions in `Common/DirectIOFile.cpp` and `.h` were reused:
  shared descriptor ownership and serialized seek/read/write replace unavailable
  POSIX descriptor duplication and positioned I/O. Preserve the existing
  copyright/license headers. The rest of its frontend and Vulkan stack were not
  imported.
- Platform API implementation checked against `switchbrew/libnx` tag v4.12.0,
  commit `7644c9b26099aa2d2145bc72a21ee24190e92085`.

New port glue, scripts and tests use GPL-2.0-or-later unless a file states
otherwise. `scripts/make-smoke-dol.py` generates original test instructions; the
result contains no Nintendo code or assets. devkitPro SDK, SDL2, Mesa and bundled
dependencies retain their own licenses; packaging includes upstream licenses.

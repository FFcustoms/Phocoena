# Patch layout

## v0.1.11 scope

The one behavioral optimization batches periodic INFO timing output in
Horizon/LogReport.h, Log.cpp and Main.cpp; errors retain immediate flushing.
The rest is instrumentation: coherent counters, sampled wall/thread CPU times,
worker identity and wait attribution. Portable video hooks are __SWITCH__-guarded;
Vulkan hooks are HORIZON_VULKAN-guarded. No CPU instructions, waits, buffer counts,
driver implementation or guest/save/settings semantics change. All are exported
in cumulative 0001-horizon-platform.patch; the SFML patch is unchanged.
See docs/VULKAN_PROFILING.md for the unavailable newest raw trace and limits.

The working source and pristine patch application match (114 main-tree files,
one SFML file). The source archive includes these patches and complete source.

## v0.1.10 scope

Allow the frontend's Vulkan submission-thread choice through the Horizon safety
layer, add wait counters, and explicitly drain before Horizon Vulkan shutdown.
Existing BindBackbuffer already serializes acquire/resize with worker completion.
No driver source change or CPU instruction-semantic change. The final ELF check
requires actual handoff/drain and timing calls. See docs/VULKAN_SUBMISSION.md.

The export helper normalizes line endings and ignores NTFS permission differences
so Windows Git and WSL Git do not export unrelated dependency changes. Native
Windows Git is substantially faster when the checkout is on the Windows drive.

## v0.1.9 Vulkan scope

The main patch adds a build-time Horizon renderer choice, static ICD loading,
VI surface creation, three-buffer enforcement and diagnostic/timing hooks in
the existing Vulkan backend. Non-Horizon dynamic loading is unchanged. Three
portable init-failure cleanup fixes destroy surfaces/swapchains before their
device. Audio is omitted only from the Vulkan variant to avoid SDL's EGL linkage.
The CPU implementation and native-JIT execution semantics are unchanged.

`CMake/HorizonNXVK.cmake` consumes an explicit pinned driver/source/archive list.
The separate driver recipe and its seven build-relocation changes are documented
under `experiments/nxvk/`; no NXVK runtime source change or libnx patch is required.
Original SDK archives remain unchanged; the NXVK SDK copy has relocated metadata.

The following sections describe the cumulative port patch and earlier versions.

Apply with `scripts/fetch-source.py`, which verifies revisions and SHA-256 hashes.

- `0001-horizon-platform.patch`: changes in the main Dolphin checkout, including
  new files in `Source/Core/Horizon` and `CMake/Horizon.cmake`.
- Further patches: separately applied in the submodule directory specified in
  `source.lock.json` (currently SFML's explicit Horizon platform detection).

No patch is applied to libnx or to the installed SDK. Externals build wrappers
remain in the main patch when they are owned by the Dolphin repository. Bundled
mbedTLS uses its supported user-configuration header hook and a libnx entropy
callback, rather than pretending Horizon is Linux.

The Horizon target uses link adapters for the default console's framebuffer
allocation and native/producer buffer calls in `Horizon/Display.cpp`. These keep
console/Mesa buffer counts consistent and retain diagnostics before libnx's
generic abort. They do not change SDK archives. `scripts/verify-elf.py` confirms
the real library call sites use the adapters. See docs/GRAPHICS_DIAGNOSIS.md for
the hardware evidence, candidate fix and remaining uncertainty.

The patches are an export of the local source edits, not an upstream submission.
Run `scripts/export-patches.py` after changing the source and test application on
a fresh pinned checkout before distributing a revision.

## v0.1.5 scope

`Horizon/Display.cpp` now owns the checked released-window reset; Console and
every GLContextSwitch initialization use it, including the core's temporary
probe -> emulation sequence. `Horizon/BootProgress.h` and frontend event hooks
provide synchronized diagnostics without modifying CPU emulation semantics.
The only new portable graphics behavior is making CustomShaderCache and
CustomResourceManager respect the existing bSupportsBackgroundCompiling flag.
OGLConfig sets that flag false under `__SWITCH__`; other targets retain their
capability selection. These edits are in 0001, not a library/SDK patch.

## v0.1.6 scope

The new Horizon/CPU initializer is called by a __SWITCH__-guarded dispatch in
PowerPC.cpp, so missing cached support/allocation failure never selects the
unsafe default AArch64 JIT. The existing CachedInterpreter implementation is
unchanged. Common/CodeBlock.h now throws bad_alloc on a null allocation before
pointer arithmetic; a host regression covers this portable failure check.
Frontend/Safety changes expose the optional cache mode and keep virtual entry
maps and executable JIT paths off. Details: docs/CACHED_INTERPRETER.md.

## v0.1.7 scope

Horizon/JitMemory and JitTest implement the CodeMemory registry and production
emitter self-test. Common/MemoryUtil adds a Switch-only writable-address/cache
hook (identity on other targets). Arm64Emitter routes stores/fixups/poisoning
through it; JitAsm writes its four quantized function tables through RW while
keeping RX table/entry addresses. CodeBlock registers child ownership before
vector growth can throw. Native CPU selection is explicit, single-core for the
first trial, with both interpreter fallbacks retained. PPC semantics, renderer
and SDK libraries are unchanged. See docs/HORIZON_JIT.md for audits and limits.

## v0.1.8 scope

Main/Safety honor the explicit frontend dual-core setting for native JIT.
Thread/GL adapters add ownership and scheduling/tick diagnostics. Performance
contains bounded snapshot counters/timers; BootProgress carries thread samples.
Small __SWITCH__ guards instrument JitArm64 top-level compilation, DVD worker
read/result wait, OGL shader/pipeline misses and GPU waits. Other platforms have
no instrumentation. No portable execution semantics or SDK binaries changed.
JitTest adds synchronized worker execution/patch handoff; no concurrent patch.
See docs/PERFORMANCE.md for source review, test evidence and limitations.

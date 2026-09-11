# Phocoena Release Workflow

This document describes the basic process for creating a public Phocoena tester release.

## 1. Verify The Source

Before building:

`python3 scripts/verify-tester-baseline.py`

Resolve any unexpected baseline differences before distributing the build.

## 2. Build Phocoena

From Linux/WSL:

`bash experiments/nxvk/dolphin-build.sh`

The resulting NRO should be:

`build/nxvk/dolphin/Binaries/Phocoena.nro`

## 3. Verify The NRO

Run:

`python3 scripts/verify-nro.py build/nxvk/dolphin/Binaries/Phocoena.nro`

Confirm that:

- The application name is `Phocoena`
- The embedded version matches the current source
- The build completes without unexpected verification errors

## 4. Hardware Test

Before publishing a release, test the NRO on real Switch hardware.

At minimum confirm:

- Phocoena launches
- The game browser works
- A GameCube title reaches gameplay
- Returning to Phocoena works
- A diagnostic session is recorded
- A support bundle can be generated
- The newest bundle receives the `UNSENT_` prefix
- Previous `UNSENT_` bundles become `SENT_` after another successful bundle is generated
- `no new game sessions to bundle` does not rename existing bundles

## 5. Create The Tester Package

Run:

`python3 scripts/package-testers.py`

The SD-ready tester ZIP will be created under:

`dist/`

The package should install Phocoena at:

`SD:/switch/Phocoena/Phocoena.nro`

## 6. Inspect The Package

Before uploading it, confirm the archive contains the expected Phocoena installation and does not contain:

- Game images
- Save files
- Console keys
- Personal diagnostic bundles
- Build caches
- Unrelated local files

## 7. Create The GitHub Release

Create a GitHub Release for the matching Phocoena version.

Attach the generated tester ZIP to the release.

The Git repository contains the corresponding source.

The GitHub Release contains the ready-to-install tester package.

## 8. Tester Instructions

Direct testers to:

- [INSTALL.md](INSTALL.md) for installation
- [SWITCH_TESTING.md](SWITCH_TESTING.md) for testing
- [KNOWN_ISSUES.md](KNOWN_ISSUES.md) for current limitations

## Release Rule

Do not publish a new tester release until the corresponding source, version, tester package and documentation agree with one another.

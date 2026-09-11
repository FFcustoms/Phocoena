# Installing Phocoena

For building from source instead, see [BUILDING.md](BUILDING.md).

## What You Need

- A modded Nintendo Switch running homebrew
- Atmosphere or a compatible environment
- An SD card
- HBMenu with full application/title takeover
- Your own legally obtained GameCube dumps

## Install

1. Download the latest **Phocoena tester ZIP** from GitHub Releases.
2. Extract the ZIP directly to the **root of your SD card**.
3. Confirm this file exists:

`SD:/switch/Phocoena/Phocoena.nro`

Do not accidentally end up with:

`SD:/switch/switch/Phocoena/Phocoena.nro`

## Add Games

Place your own GameCube dumps in:

`SD:/switch/Phocoena/games/`

Supported formats currently include:

- `.rvz`
- `.iso`
- `.gcm`

You can also browse to another SD-card folder from inside Phocoena.

## Launch

Launch HBMenu using **full application/title takeover**, not Album/applet mode.

Then launch **Phocoena**.

## Updating

Download the newer tester ZIP and extract it to the SD root again.

Allow the existing Phocoena files to be replaced.

## Support Bundles

When testing is finished:

1. Open **About / Diagnostics**.
2. Press **X - Generate support bundle**.
3. Open:

`SD:/switch/Phocoena/diagnostics/bundles/`

4. Send the newest ZIP beginning with:

`UNSENT_`

Example:

`UNSENT_Phocoena-Support-1234567890.zip`

If Phocoena says:

`no new game sessions to bundle`

no new report was created.

For the full testing procedure, see [SWITCH_TESTING.md](SWITCH_TESTING.md).

# Building Phocoena

This guide covers the currently tested Windows build setup for Phocoena.

The tested setup keeps the Phocoena repository on the Windows `C:` drive and accesses it from Ubuntu through `/mnt/c/`.

If you only want to run Phocoena, you do not need any of this. Download the tester ZIP from GitHub Releases and see [INSTALL.md](INSTALL.md).

#============================================================#
# SECTION 1 - WINDOWS SETUP
# DO THIS ONCE
#============================================================#

## Step 1 - Install WSL2 + Ubuntu

Open **PowerShell as Administrator** and run:

```powershell
wsl --install
```

Restart Windows if asked.

After restarting, open **Ubuntu** from the Windows Start menu.

The first launch may ask you to create a Linux username and password.

Everything after this point should be run inside the Ubuntu terminal unless a step specifically says otherwise.

## Step 2 - Put Phocoena On Your Windows Drive

Keep the Phocoena repository somewhere easy to access on your Windows `C:` drive.

Example:

```text
C:\Users\YourName\Desktop\Phocoena
```

You can clone the repository from GitHub or download it there.

Inside Ubuntu, that same folder appears as:

```text
/mnt/c/Users/YourName/Desktop/Phocoena
```

For example:

```bash
cd /mnt/c/Users/YourName/Desktop/Phocoena
```

Replace `YourName` with your actual Windows username.

#============================================================#
# SECTION 2 - UBUNTU / WSL FIRST-TIME SETUP
# DO THIS ONCE PER BUILD ENVIRONMENT
#============================================================#

## Step 1 - Update Ubuntu

Inside the Ubuntu terminal:

```bash
sudo apt update
```

## Step 2 - Install Host Build Packages

Copy and paste:

```bash
sudo apt install -y \
  git \
  python3 \
  python3-pip \
  python3-venv \
  make \
  gcc \
  g++ \
  cmake \
  ninja-build \
  pkg-config \
  rsync \
  curl \
  ca-certificates \
  bison \
  flex \
  clang-19 \
  llvm-19-dev \
  libclang-19-dev \
  libclc-19-dev \
  libclc-19 \
  llvm-spirv-19 \
  libllvmspirvlib-19-dev \
  libdrm-dev \
  glslang-tools \
  spirv-tools \
  spirv-tools-dev \
  spirv-headers
```

Phocoena's currently tested build environment is Ubuntu 26.04 under WSL2.

## Step 3 - Enter The Phocoena Repository

Example:

```bash
cd /mnt/c/Users/YourName/Desktop/Phocoena
```

Make sure you are in the repository root.

You should see files such as:

```text
README.md
BUILDING.md
source.lock.json
scripts/
experiments/
upstream/
```

## Step 4 - Bootstrap The Phocoena Toolchain

Run:

```bash
python3 scripts/bootstrap-toolchain.py
```

Then:

```bash
bash scripts/bootstrap-host.sh
```

This prepares the pinned Switch/devkitPro environment used by Phocoena.

## Step 5 - Bootstrap NXVK

Run:

```bash
bash experiments/nxvk/bootstrap.sh
```

The default NXVK workspace is created in Linux at:

```text
/var/tmp/dolphin-nxvk-69ec283d
```

This is intentional.

The Phocoena repository can remain on `/mnt/c/` while the heavy NXVK build workspace stays in the Linux filesystem.

## Step 6 - Build The NXVK Components

Run these commands in order:

```bash
bash experiments/nxvk/build.sh native
```

```bash
bash experiments/nxvk/build.sh cross
```

```bash
bash experiments/nxvk/build.sh archives
```

These are first-time setup steps.

You normally do not need to repeat them for every Phocoena build.

## Step 7 - Verify The Tester Baseline

Run:

```bash
python3 scripts/verify-tester-baseline.py
```

If this reports an unexpected baseline difference, do not distribute the build until the difference is understood.

## Step 8 - Build Phocoena

Run:

```bash
bash experiments/nxvk/dolphin-build.sh
```

When it finishes successfully, the NRO will be copied back into the Windows-side repository at:

```text
build/nxvk/dolphin/Binaries/Phocoena.nro
```

On Windows, that will be inside your normal Phocoena folder.

Example:

```text
C:\Users\YourName\Desktop\Phocoena\build\nxvk\dolphin\Binaries\Phocoena.nro
```

## Step 9 - Verify The Finished NRO

Run:

```bash
python3 scripts/verify-nro.py build/nxvk/dolphin/Binaries/Phocoena.nro
```

The application should identify itself as:

```text
Phocoena
```

## Step 10 - Create The Complete SD-Ready Tester ZIP

The NRO by itself is not the normal finished package.

Create the complete SD-ready tester package with:

```bash
python3 scripts/package-testers.py
```

The finished ZIP will be written under:

```text
dist/
```

For v0.1.27, the expected package is:

```text
dist/Phocoena-v0.1.27-tester.zip
```

This ZIP contains the Phocoena NRO along with the supporting files, configuration, directories, licenses, and other files expected by Phocoena.

For normal installation and testing, use this complete ZIP instead of copying only the NRO.

Extract the ZIP directly to the root of the Switch SD card.

After extraction, Phocoena should be located at:

```text
SD:/switch/Phocoena/Phocoena.nro
```

#============================================================#
# SECTION 3 - BUILD PHOCOENA AGAIN LATER
# THIS IS THE NORMAL BUILD PROCESS
#============================================================#

Once Section 2 has been completed successfully, future builds are much simpler.

## Step 1 - Open Ubuntu

Open Ubuntu from the Windows Start menu.

## Step 2 - Enter Your Phocoena Repository

Example:

```bash
cd /mnt/c/Users/YourName/Desktop/Phocoena
```

## Step 3 - Update The Repository

If you cloned Phocoena with Git:

```bash
git pull
```

Skip this step if you are building local source changes that you do not want replaced.

## Step 4 - Verify The Tester Baseline

Run:

```bash
python3 scripts/verify-tester-baseline.py
```

## Step 5 - Build Phocoena

Run:

```bash
bash experiments/nxvk/dolphin-build.sh
```

## Step 6 - Verify The Finished NRO

Run:

```bash
python3 scripts/verify-nro.py build/nxvk/dolphin/Binaries/Phocoena.nro
```

## Step 7 - Create The Complete SD-Ready Tester ZIP

Run:

```bash
python3 scripts/package-testers.py
```

## Step 8 - Find The Finished Package

The complete tester package will be under:

```text
dist/
```

For v0.1.27:

```text
dist/Phocoena-v0.1.27-tester.zip
```

This is the package normal users and testers should install.

Extract it directly to the root of the Switch SD card.

The standalone NRO is also available at:

```text
build/nxvk/dolphin/Binaries/Phocoena.nro
```

but the NRO alone is not the recommended installation package.

#============================================================#
# VERIFY OR CREATE A SHA-256 CHECKSUM
#============================================================#

## Verify An Official Tester Release

Official Phocoena tester releases include both the tester ZIP and its SHA-256 checksum file.

Download both files from:

https://github.com/FFcustoms/Phocoena/releases

For v0.1.27, the files are:

- `Phocoena-v0.1.27-tester.zip`
- `Phocoena-v0.1.27-tester.zip.sha256`

Place both files in the same folder.

### Verify From Windows PowerShell

Open PowerShell in that folder and run:

`$expected = (Get-Content ".\Phocoena-v0.1.27-tester.zip.sha256").Split()[0].Trim().ToLower()`

`$actual = (Get-FileHash ".\Phocoena-v0.1.27-tester.zip" -Algorithm SHA256).Hash.ToLower()`

Then run:

`if ($expected -eq $actual) { "MATCH - ZIP verified successfully" } else { "MISMATCH - ZIP does not match the published checksum" }`

A valid official download should report:

`MATCH - ZIP verified successfully`

### Verify From Ubuntu / WSL

With both files in the same folder, run:

`sha256sum -c Phocoena-v0.1.27-tester.zip.sha256`

A valid official download should report:

`Phocoena-v0.1.27-tester.zip: OK`

## Create A SHA-256 Checksum For Your Own Build

After creating your tester ZIP with:

`python3 scripts/package-testers.py`

create its SHA-256 checksum from the Phocoena repository root with:

`sha256sum dist/Phocoena-v0.1.27-tester.zip > dist/Phocoena-v0.1.27-tester.zip.sha256`

This creates:

`dist/Phocoena-v0.1.27-tester.zip.sha256`

To immediately verify your own ZIP and checksum:

`cd dist`

`sha256sum -c Phocoena-v0.1.27-tester.zip.sha256`

A successful verification should report:

`Phocoena-v0.1.27-tester.zip: OK`

The checksum only proves that a ZIP matches the checksum file being compared against it.

To verify an official Phocoena release, use the `.sha256` file downloaded from the same GitHub Release as the tester ZIP.
#============================================================#
# OPTIONAL - LOWER RAM USAGE
#============================================================#

If the build system is low on RAM, reduce the number of parallel build jobs:

```bash
JOBS=4 bash experiments/nxvk/dolphin-build.sh
```

#============================================================#
# CURRENT SOURCE PINS
#============================================================#

Current Dolphin base:

```text
4f8af23db516d8b6e9cd00e7b261a65b026514a8
```

Current NXVK revision:

```text
69ec283dbda64e65347a36274efb349122e85363
```

NXVK Mesa base:

```text
26.1.4
```

Additional pinned build information is stored in:

- `source.lock.json`
- `toolchain.lock.json`
- `toolchain-packages.json`

#============================================================#
# IF THE BUILD FAILS
#============================================================#

When reporting a build problem, include:

1. The command that failed.
2. The complete error output.
3. Your Ubuntu version.
4. Whether the repository is being accessed through `/mnt/c/`.

Check the Ubuntu version with:

```bash
lsb_release -a
```

Do not randomly replace or update the pinned NXVK, Dolphin, or Switch toolchain components while troubleshooting.


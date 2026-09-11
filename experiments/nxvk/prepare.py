#!/usr/bin/env python3
"""Relocate only NXVK's build recipes into an isolated SDK; no driver edits."""
# SPDX-License-Identifier: GPL-2.0-or-later
import json
import difflib
import os
from pathlib import Path
import shutil
import subprocess

REPO = Path(__file__).resolve().parents[2]
LOCK = json.loads((Path(__file__).parent / "source.lock.json").read_text())
WORK = Path(os.environ.get("NXVK_WORK", "/var/tmp/dolphin-nxvk-69ec283d")).resolve()
SRC = WORK / "source"
SDK = WORK / "devkitpro"

def git(*args):
    return subprocess.check_output(["git", "-C", str(SRC), *args])

def original(relative):
    if (SRC / ".git").exists():
        return git("show", "HEAD:" + relative).decode()
    return (SRC / ".nxvk-originals" / relative).read_text()

def main():
    revision = git("rev-parse", "HEAD").decode().strip() if (SRC / ".git").exists() else (SRC / "NXVK_UPSTREAM_COMMIT").read_text().strip()
    if revision != LOCK["commit"]:
        raise SystemExit("ERROR: NXVK revision differs from the lock")
    if not (SDK / "devkitA64/bin/aarch64-none-elf-gcc").is_file():
        origin = Path(os.environ.get("NXVK_SDK_BASE", str(REPO / ".toolchain/image/opt/devkitpro")))
        subprocess.run(["rsync", "-a", str(origin) + "/", str(SDK) + "/"], check=True)
    if (WORK / "toolchain.lock.json").exists() and (WORK / "toolchain.lock.json").read_bytes() != (REPO / "toolchain.lock.json").read_bytes():
        raise SystemExit("ERROR: isolated SDK lock differs; choose a fresh NXVK_WORK")
    if WORK != REPO:
        shutil.copyfile(REPO / "toolchain.lock.json", WORK / "toolchain.lock.json")
    include = WORK / "cross-include"
    include.mkdir(exist_ok=True)
    for name in ("xf86drm.h", "xf86drmMode.h"):
        shutil.copyfile(Path("/usr/include") / name, include / name)
    shutil.copytree("/usr/include/libdrm", include, dirs_exist_ok=True)
    shutil.copytree("/usr/include/libdrm", include / "libdrm", dirs_exist_ok=True)
    shutil.copytree(SRC / "switch/docker/cross-include", include, dirs_exist_ok=True)
    paths = ["switch/crossfiles/native.txt", "switch/crossfiles/switch.cross",
             "switch/crossfiles/rust.cross", "switch/rust/rustc-switch.sh",
             "switch/build/configure-mesa.sh", "switch/build/build-nro.sh",
             "switch/build/build-native-tools.sh"]
    patch = []
    for relative in paths:
        # Regenerate from the pinned original, not a previously relocated file.
        before = original(relative)
        text = before.replace("/opt/devkitpro", str(SDK))
        text = text.replace("/opt/switch-cross-include", str(include))
        text = text.replace("/work/switch", str(SRC / "switch"))
        text = text.replace("/usr/bin/llvm-config-15", "/usr/bin/llvm-config-19")
        text = text.replace("clang clang-15 clang-16 clang-17 clang-18", "clang-19")
        text = text.replace('RECONF="--wipe"', 'RECONF="--reconfigure"')
        if relative == "switch/build/configure-mesa.sh":
            # Target SPIRV-Tools is only an optional assembly dumper. The
            # container's copied host .pc would falsely enable it against old
            # portlib headers, and could introduce x86 linker search paths.
            # Keep full SPIRV-Tools in the native CLC build, not in the NRO.
            text = text.replace("-Dvulkan-beta=false", "-Dspirv-tools=disabled -Dvulkan-beta=false")
        # Limit native compile parallelism on this WSL host.
        text = text.replace('ninja -C "$BUILD"', 'ninja -j "${JOBS:-4}" -C "$BUILD"')
        (SRC / relative).write_text(text)
        patch.extend(difflib.unified_diff(before.splitlines(True), text.splitlines(True),
            fromfile="a/" + relative, tofile="b/" + relative))
    # The isolated package metadata must not refer back to /opt's SDK.
    for pc in (SDK / "portlibs/switch").rglob("*.pc"):
        pc.write_text(pc.read_text().replace("/opt/devkitpro", str(SDK)))
    # These are host-only descriptions of OpenCL/SPIR-V inputs, as in NXVK's
    # container recipe. Do not use their x86 archives in the final ARM64 link.
    pc_dir = SDK / "portlibs/switch/lib/pkgconfig"
    for name in ("libclc", "LLVMSPIRVLib", "SPIRV-Tools", "SPIRV-Tools-shared", "SPIRV-Headers"):
        candidates = list(Path("/usr/lib/llvm-19").rglob(name + ".pc"))
        candidates += list(Path("/usr/lib/x86_64-linux-gnu/pkgconfig").glob(name + ".pc"))
        candidates += list(Path("/usr/share/pkgconfig").glob(name + ".pc"))
        if candidates:
            shutil.copyfile(candidates[0], pc_dir / (name + ".pc"))
    subprocess.run([str(SDK / "devkitA64/bin/aarch64-none-elf-ar"), "rcs",
                    str(SDK / "portlibs/switch/lib/libdl.a")], check=True)
    output = REPO / "build/nxvk"
    output.mkdir(parents=True, exist_ok=True)
    (output / "build-relocation.patch").write_text("".join(patch))
    (output / "host-packages.tsv").write_bytes(subprocess.check_output(
        ["dpkg-query", "-W", "-f=${binary:Package}\t${Version}\n"]))
    print("Prepared isolated SDK and relocated build scripts:", WORK)

if __name__ == "__main__":
    main()

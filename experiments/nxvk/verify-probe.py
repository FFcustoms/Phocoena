#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check the actual linked driver and adapters, not just source definitions."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]
WORK = Path(os.environ.get("NXVK_WORK", "/var/tmp/dolphin-nxvk-69ec283d"))
BIN = WORK / "devkitpro/devkitA64/bin"
ELF = ROOT / "build/nxvk/Binaries/vulkan-probe.elf"
NRO = ELF.with_suffix(".nro")

def run(tool, *args):
    return subprocess.check_output([str(BIN / ("aarch64-none-elf-" + tool)), *args, str(ELF)], text=True)

def main():
    spec = importlib.util.spec_from_file_location("nro", ROOT / "scripts/verify-nro.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    report = module.verify(NRO.read_bytes())
    assert report["name"] == "Vulkan Probe for Dolphin" and report["version"] == "0.1.0"
    header = run("readelf", "-h", "-n", "-S")
    assert "AArch64" in header and ".debug_line" in header
    assert not run("nm", "-u").strip(), "undefined linked symbols"
    symbols = run("nm", "--defined-only")
    required = ["vk_icdGetInstanceProcAddr", "nvk_CreateDevice", "nak_compile_shader", "wsi_switch_init_wsi",
                "__libnx_exception_handler", "__wrap_diagAbortWithResult"]
    for name in required:
        assert re.search(r"\b" + name + r"$", symbols, re.M), name
    # This driver has no fatalThrow callers; the linker correctly drops that
    # adapter. Do not force dead OpenGL-era code into the binary to pass a test.
    if re.search(r"\bfatalThrow$", symbols, re.M):
        assert re.search(r"\b__wrap_fatalThrow$", symbols, re.M)
    for name in ("eglInitialize", "eglCreateContext", "glDrawArrays", "BootCore"):
        assert not re.search(r"\b" + name + r"$", symbols, re.M), "unexpected backend/emulator: " + name
    disassembly = run("objdump", "-d")
    adapters = ["framebufferCreate", "nwindowDequeueBuffer", "nwindowQueueBuffer",
                "nwindowReleaseBuffers", "bqDequeueBuffer", "bqRequestBuffer", "pthread_create"]
    for name in adapters:
        assert re.search(r"\b(?:bl|b)\s+[0-9a-f]+\s+<__wrap_" + name + r">", disassembly), "unused adapter " + name
    inputs = json.loads((ELF.parent / "source-inputs.json").read_text())
    for relative, expected in inputs.items():
        assert hashlib.sha256((ROOT / relative).read_bytes()).hexdigest() == expected, "stale build input " + relative
    report.update({"elf_sha256": hashlib.sha256(ELF.read_bytes()).hexdigest(),
                   "elf_size": ELF.stat().st_size,
                   "elf_build_id": re.search(r"Build ID: ([0-9a-f]+)", header)[1],
                   "required_driver_symbols": required, "linked_display_thread_adapters": adapters,
                   "source_inputs_verified": len(inputs), "hardware_tested": False})
    (ROOT / "build/nxvk/release-verification.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))

if __name__ == "__main__":
    main()

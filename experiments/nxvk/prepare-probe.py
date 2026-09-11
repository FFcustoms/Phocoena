#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generate the probe's build identity and reuse tested libnx display adapters."""
import datetime
import hashlib
import json
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WORK = Path(os.environ["NXVK_WORK"])
OUT = WORK / "probe-switch"
OUT.mkdir(exist_ok=True)
LOCK = json.loads((ROOT / "experiments/nxvk/source.lock.json").read_text())

inputs = list((ROOT / "experiments/nxvk").rglob("*"))
inputs = [p for p in inputs if p.is_file() and p.suffix in {".py", ".sh", ".cpp", ".h", ".c", ".json", ".vert", ".frag"}]
inputs += [ROOT / "toolchain.lock.json", ROOT / "toolchain-packages.json"]
core = ROOT / "upstream/dolphin/Source/Core/Horizon"
inputs += [core / name for name in ("Console.cpp", "Console.h", "Display.cpp", "Display.h", "Crash.c", "PThread.cpp", "Log.h")]
manifest = {p.relative_to(ROOT).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(inputs)}
encoded = json.dumps(manifest, sort_keys=True, indent=2).encode() + b"\n"
identity = hashlib.sha256(encoded).hexdigest()
(OUT / "source-inputs.json").write_bytes(encoded)
date = datetime.datetime.fromtimestamp(int(os.environ["SOURCE_DATE_EPOCH"]), datetime.timezone.utc).isoformat()
sdk = json.loads((ROOT / "toolchain.lock.json").read_text())["manifest"]
(OUT / "BuildIdentity.h").write_text(
    '#pragma once\n#define PROBE_VERSION "0.1.0"\n' +
    f'#define NXVK_COMMIT "{LOCK["commit"]}"\n' +
    f'#define PROBE_DATE "{date}"\n' +
    f'#define SDK_DIGEST "{sdk}"\n' +
    f'#define PROBE_RECIPE_SHA256 "{identity}"\n')
# Keep the proven display/buffer lifecycle; only change the diagnostic label.
(OUT / "Console.cpp").write_text((core / "Console.cpp").read_text().replace('PrepareWindow("OpenGL")', 'PrepareWindow("Vulkan probe")'))
# A separate app must not rotate or truncate the emulation crash evidence.
(OUT / "Crash.c").write_text((core / "Crash.c").read_text()
    .replace("/logs/crash.log", "/logs/vulkan-probe-crash.log")
    .replace("/logs/crash.previous.log", "/logs/vulkan-probe-crash.previous.log"))
print("Probe source identity:", identity)

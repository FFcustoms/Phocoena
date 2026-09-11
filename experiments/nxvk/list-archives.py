#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
import re
import sys
from pathlib import Path

makefile = Path(sys.argv[1]).read_text()
match = re.search(r"^SUPPORT_LIBS\s*:=\s*(.*?)(?=\n\n)", makefile, re.M | re.S)
if not match:
    raise SystemExit("ERROR: cannot find NXVK support archive list")
archives = match[1].replace("\\\n", " ").split()
if not all(re.fullmatch(r"src/[A-Za-z0-9_/.]+\.a", a) and ".." not in a for a in archives):
    raise SystemExit("ERROR: unexpected NXVK archive path")
print("src/nouveau/vulkan/libnvk.a", *archives, sep="\n")

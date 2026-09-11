#!/usr/bin/env python3
"""Query target-only .pc files and relocate an extracted devkitPro image."""
import os
import subprocess
import sys
from pathlib import Path
prefix = Path(os.environ['DEVKITPRO']).resolve().as_posix()
env = os.environ.copy()
env.update(PKG_CONFIG_PATH='', PKG_CONFIG_DIR='', PKG_CONFIG_SYSROOT_DIR='',
           PKG_CONFIG_LIBDIR=prefix + '/portlibs/switch/lib/pkgconfig')
result = subprocess.run(['pkg-config', '--static', *sys.argv[1:]], env=env,
                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
sys.stdout.write(result.stdout.replace('/opt/devkitpro', prefix))
sys.stderr.write(result.stderr)
raise SystemExit(result.returncode)

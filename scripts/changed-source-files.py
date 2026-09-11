#!/usr/bin/env python3
"""List exported port files for the optional WSL source cache.

Run export-patches.py before cached builds after editing source. For source
deletions, unmodified file reversions, or changing revisions, use a
fresh scratch directory; this development accelerator never deletes cache data.
"""
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'upstream/dolphin'
lock = json.loads((ROOT / 'source.lock.json').read_text())
paths = set()
for patch in lock['patches']:
    prefix = '' if patch['directory'] == '.' else patch['directory'] + '/'
    for line in (ROOT / patch['path']).read_text().splitlines():
        if line.startswith('+++ b/'):
            name = prefix + line[6:]
            if (SOURCE / name).is_file():
                paths.add(name)
print('\n'.join(sorted(paths)))

#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Record final build inputs and freeze matching crash-symbolication artifacts."""
import hashlib
import json
import os
import re
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
WORK = Path(os.environ.get('NXVK_WORK', '/var/tmp/dolphin-nxvk-69ec283d'))
OUT = ROOT / 'build/nxvk/dolphin'

def sha(path):
    with path.open('rb') as file:
        return hashlib.file_digest(file, 'sha256').hexdigest()

def main():
    elf = json.loads((OUT / 'elf-verification.json').read_text())
    nro = json.loads((OUT / 'nro-verification.json').read_text())
    version = nro['version']
    assert elf['renderer'] == 'Vulkan' and re.fullmatch(r'\d+\.\d+\.\d+', version)
    assert f'set(HORIZON_PORT_VERSION "{version}")' in (ROOT / 'upstream/dolphin/Source/Core/Horizon/CMakeLists.txt').read_text()
    for suffix, report in [('elf', elf), ('nro', nro)]:
        assert sha(OUT / f'Binaries/dolphin.{suffix}') == report['sha256']
    lock_hash = sha(ROOT / 'source.lock.json')
    assert lock_hash.encode() in (OUT / 'Binaries/Phocoena.nro').read_bytes()
    changed = subprocess.check_output(['python3', str(ROOT / 'scripts/changed-source-files.py')], text=True).splitlines()
    native_root = WORK / f'dolphin-v{version}'
    native = native_root / 'source'
    for relative in changed:
        assert (native / relative).read_bytes().replace(b'\r\n', b'\n') == (ROOT / 'upstream/dolphin' / relative).read_bytes().replace(b'\r\n', b'\n'), relative
    archives = (native_root / 'archives.txt').read_text().splitlines()
    inputs = {'source_lock_sha256': lock_hash, 'nxvk_lock_sha256': sha(ROOT / 'experiments/nxvk/source.lock.json'),
              'nxvk_commit': '69ec283dbda64e65347a36274efb349122e85363',
              'patched_source_files_compared_to_build': len(changed),
              'release_isolated_build_directory': str(native_root),
              'cross_release_object_reuse': False,
              'driver_archives': {str(Path(p).relative_to(WORK / 'source/switch/build/cross')): sha(Path(p)) for p in archives},
              'toolchain_lock_sha256': sha(ROOT / 'toolchain.lock.json'),
              'nro_sha256': nro['sha256'], 'elf_sha256': elf['sha256'], 'elf_build_id': elf['build_id']}
    (OUT / 'BUILD_INPUTS.json').write_text(json.dumps(inputs, indent=2) + '\n')
    freeze = ROOT / f'build/switch/v{version}/Binaries'
    freeze.mkdir(parents=True, exist_ok=True)
    for suffix in ['elf', 'nro']:
        shutil.copyfile(OUT / f'Binaries/dolphin.{suffix}', freeze / f'dolphin.{suffix}')
    # Update only the current identity block, not historical v0.1.8 evidence.
    status = ROOT / 'BUILD_STATUS.md'
    text = status.read_text()
    before, historical = text.split('## Historical build record', 1)
    before = re.sub(r'NRO: [0-9,]+ bytes', f'NRO: {nro["size"]:,} bytes', before, count=1)
    before = re.sub(r'ELF: [0-9,]+ bytes', f'ELF: {(OUT / "Binaries/dolphin.elf").stat().st_size:,} bytes', before, count=1)
    before = re.sub(r'NRO SHA256: [0-9a-f]+', 'NRO SHA256: ' + nro['sha256'], before, count=1)
    before = re.sub(r'ELF SHA256: [0-9a-f]+', 'ELF SHA256: ' + elf['sha256'], before, count=1)
    before = re.sub(r'ELF build ID: [0-9a-f]+', 'ELF build ID: ' + elf['build_id'], before, count=1)
    before = re.sub(r'source.lock SHA256: [0-9a-f]+', 'source.lock SHA256: ' + lock_hash, before, count=1)
    status.write_text(before + '## Historical build record' + historical, newline='\n')
    print(json.dumps(inputs, indent=2))

if __name__ == '__main__':
    main()

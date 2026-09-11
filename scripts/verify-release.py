#!/usr/bin/env python3
"""Audit a versioned SD ZIP and complete source archive without extracting either."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path, PurePosixPath
import tarfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    with path.open('rb') as file:
        return hashlib.file_digest(file, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT / 'upstream/dolphin')
    parser.add_argument('--build', type=Path, required=True)
    args = parser.parse_args()
    build, source = args.build.resolve(), args.source.resolve()
    spec = importlib.util.spec_from_file_location('verify_nro', ROOT / 'scripts/verify-nro.py')
    verifier = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(verifier)
    smoke_spec = importlib.util.spec_from_file_location('smoke_dol', ROOT / 'scripts/make-smoke-dol.py')
    smoke = importlib.util.module_from_spec(smoke_spec)
    smoke_spec.loader.exec_module(smoke)
    nro = build / 'Binaries/dolphin.nro'
    metadata = verifier.verify(nro.read_bytes())
    version = metadata['version']
    prefix = 'dolphin-horizon-v' + version + '/'
    release_root = ROOT / 'dist' / ('v' + version)
    sd_archive = ROOT / 'dist' / (prefix[:-1] + '.zip')
    source_archive = ROOT / 'dist' / (prefix[:-1] + '-source.tar.gz')
    lock_sha = digest(ROOT / 'source.lock.json')
    assert lock_sha.encode() in nro.read_bytes(), 'NRO/source lock mismatch'
    with zipfile.ZipFile(sd_archive) as archive:
        assert archive.testzip() is None
        for entry in archive.infolist():
            path = PurePosixPath(entry.filename)
            assert not path.is_absolute() and '..' not in path.parts
        manifest = json.loads(archive.read('switch/dolphin/BUILD-MANIFEST.json'))
        assert manifest['version'] == version
        assert manifest['source_lock_sha256'] == lock_sha
        smoke_data = archive.read('switch/dolphin/games/horizon-smoke.dol')
        smoke.validate_dol(smoke_data)
        assert smoke_data == smoke.make_dol(), 'Packaged smoke test differs from generator'
        for name, expected in manifest['files'].items():
            assert hashlib.sha256(archive.read('switch/dolphin/' + name)).hexdigest() == expected, name
        for folder in ['config', 'games', 'logs', 'shaders', 'cache', 'cache/shaders', 'cache/tmp']:
            assert archive.getinfo('switch/dolphin/' + folder + '/').is_dir()
        assert manifest['sha256'] == digest(nro)
        assert digest(release_root / 'switch/dolphin/dolphin.nro') == digest(nro)
        assert manifest['companion_elf_sha256'] == digest(build / 'Binaries/dolphin.elf')

    count = 0
    seen = set()
    with tarfile.open(source_archive, 'r|gz') as archive:
        for entry in archive:
            path = PurePosixPath(entry.name)
            assert not path.is_absolute() and '..' not in path.parts and '.git' not in path.parts
            assert entry.name.startswith(prefix)
            if not entry.isfile():
                continue
            name = entry.name[len(prefix):]
            seen.add(name)
            data = archive.extractfile(entry).read()
            if name == 'SOURCE_ARCHIVE_README.txt':
                assert b'do not run fetch-source.py' in data.lower()
                assert version.encode() in data
                continue
            if name == 'dependencies/nxvk-source.tar.gz':
                assert hashlib.sha256(data).hexdigest() == '99e041e3f24237486b19ab1bfd088522aefd97c16514a3acab68d44f9ce4600e'
                count += 1
                continue
            original = source / name[len('upstream/dolphin/'):] if name.startswith('upstream/dolphin/') else ROOT / name
            assert hashlib.sha256(data).hexdigest() == digest(original), name
            count += 1
    lock = json.loads((ROOT / 'source.lock.json').read_text())
    if 'nxvk_commit' in manifest:
        assert 'dependencies/nxvk-source.tar.gz' in seen, 'Missing corresponding driver source'
        assert 'experiments/nxvk/dolphin-build.sh' in seen
    for dependency in lock['submodules']:
        assert any(name.startswith('upstream/dolphin/' + dependency['path'] + '/') for name in seen)
    for required in ['upstream/dolphin/Source/Core/Horizon/Console.cpp', 'COPYING', 'BUILD_STATUS.md',
                     'scripts/build-switch.sh', 'scripts/fetch-source.py', 'GRAPHICS_RETEST.md']:
        assert required in seen
    report = {'version': version, 'zip_crc': 'PASS', 'smoke_dol_aligned_bounds': 'PASS',
              'packaged_files_sha256': len(manifest['files']),
              'source_files_compared': count, 'source_dependencies_present': len(lock['submodules']),
              'source_lock_sha256': lock_sha, 'hardware_tested': False, 'artifacts': {}}
    for path in [sd_archive, source_archive, release_root / 'switch/dolphin/dolphin.nro',
                 build / 'Binaries/dolphin.elf']:
        report['artifacts'][path.relative_to(ROOT).as_posix()] = {'bytes': path.stat().st_size, 'sha256': digest(path)}
    (release_root / 'RELEASE-VERIFICATION.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()

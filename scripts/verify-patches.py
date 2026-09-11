#!/usr/bin/env python3
"""Apply exported patches to pristine touched files and compare to the working tree.

Uses a new directory under build/patch-checks, never resets the source or deletes
user data. This verifies patch application/content, not compilation.
"""
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'upstream/dolphin'


def git(repo, *args):
    return subprocess.check_output(['git', '-c', f'safe.directory={repo.as_posix()}',
                                    '-C', str(repo), *args], stderr=subprocess.PIPE)


def main():
    lock = json.loads((ROOT / 'source.lock.json').read_text())
    checks = ROOT / 'build/patch-checks'
    checks.mkdir(parents=True, exist_ok=True)
    scratch = Path(tempfile.mkdtemp(prefix='verify-', dir=checks)).resolve()
    if not scratch.is_relative_to(checks.resolve()):
        raise RuntimeError('Unexpected scratch path')
    for index, patch in enumerate(lock['patches']):
        file = ROOT / patch['path']
        if hashlib.sha256(file.read_bytes()).hexdigest() != patch['sha256']:
            raise RuntimeError('Patch digest mismatch')
        repo = SOURCE / patch['directory']
        target = scratch / str(index)
        target.mkdir()
        paths = [line[6:] for line in file.read_text().splitlines() if line.startswith('+++ b/')]
        tracked = set(git(repo, 'ls-tree', '-r', '--name-only', 'HEAD').decode().splitlines())
        for name in paths:
            if name in tracked:
                path = target / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(git(repo, 'show', 'HEAD:' + name))
        git(target, 'init', '--quiet')
        git(target, 'apply', '--check', str(file))
        git(target, 'apply', str(file))
        for name in paths:
            actual = (target / name).read_bytes().replace(b'\r\n', b'\n')
            expected = (repo / name).read_bytes().replace(b'\r\n', b'\n')
            if actual != expected:
                raise RuntimeError('Applied patch differs from working source: ' + name)
        print('PASS:', patch['path'], f'({len(paths)} files)')
    print('Pristine patch application and source comparison passed:', scratch)


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Fetch pinned Dolphin and apply the port. Never reset or overwrite an existing checkout."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def git(repo, *args, capture=False):
    command = ['git', '-C', str(repo), *args]
    if capture:
        return subprocess.check_output(command, text=True).strip()
    subprocess.run(command, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--destination', type=Path, default=ROOT / 'upstream/dolphin')
    args = parser.parse_args()
    source = args.destination.resolve()
    lock = json.loads((ROOT / 'source.lock.json').read_text())
    for patch in lock['patches']:
        if hashlib.sha256((ROOT / patch['path']).read_bytes()).hexdigest() != patch['sha256']:
            raise RuntimeError('Patch checksum mismatch: ' + patch['path'])
    if not source.exists():
        source.mkdir(parents=True)
        git(source, 'init')
        git(source, 'remote', 'add', 'origin', lock['upstream']['url'])
        git(source, 'fetch', '--depth', '1', 'origin', lock['upstream']['commit'])
        git(source, 'checkout', '--detach', 'FETCH_HEAD')
    if git(source, 'rev-parse', 'HEAD', capture=True) != lock['upstream']['commit']:
        raise RuntimeError('Existing checkout has a different commit; use a fresh destination')
    # Refuse incompatible changes. An applied patch is idempotent and unrelated
    # local edits are preserved. Never move an existing submodule off its commit.
    for module in lock['submodules']:
        directory = source / module['path']
        if (directory / '.git').exists() and git(directory, 'rev-parse', 'HEAD', capture=True) != module['commit']:
            raise RuntimeError('Existing submodule has a different commit; use a fresh destination: ' + module['path'])
    for patch in lock['patches']:
        directory = source / patch['directory']
        if not directory.exists() or not (directory / '.git').exists():
            continue
        if git(directory, 'status', '--porcelain', '--ignore-submodules=all', capture=True):
            result = subprocess.run(['git', '-C', str(directory), 'apply', '--reverse', '--check',
                                     str(ROOT / patch['path'])], capture_output=True)
            if result.returncode:
                raise RuntimeError('Existing changes do not match the port; preserve them and choose a fresh destination')
    git(source, 'submodule', 'update', '--init', '--depth', '1', '--jobs', '8', '--',
        *(module['path'] for module in lock['submodules']))
    for module in lock['submodules']:
        if git(source / module['path'], 'rev-parse', 'HEAD', capture=True) != module['commit']:
            raise RuntimeError('Submodule revision mismatch: ' + module['path'])
    for patch in lock['patches']:
        directory = source / patch['directory']
        file = str(ROOT / patch['path'])
        applied = subprocess.run(['git', '-C', str(directory), 'apply', '--reverse', '--check', file],
                                 capture_output=True)
        if applied.returncode == 0:
            print('Already applied:', patch['path'])
        else:
            git(directory, 'apply', '--check', file)
            git(directory, 'apply', file)
    print('Ready:', source)


if __name__ == '__main__':
    main()

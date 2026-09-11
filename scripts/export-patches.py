#!/usr/bin/env python3
"""Export the reviewed upstream working-tree changes without altering its Git index."""
import configparser
import difflib
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'upstream/dolphin'
UPSTREAM = '4f8af23db516d8b6e9cd00e7b261a65b026514a8'


def git(repo, *args):
    # The same NTFS checkout can be read by Windows Git and WSL Git. Ignore
    # mount-mode differences and normalize CRLF so neither becomes a core patch.
    return subprocess.check_output(['git', '-c', 'core.filemode=false', '-c', 'core.autocrlf=true',
                                    '-c', f'safe.directory={repo.as_posix()}',
                                    '-C', str(repo), *args])


def export(repo, output):
    patch = git(repo, 'diff', '--binary', '--full-index', '--ignore-submodules=all', 'HEAD')
    for name in git(repo, 'ls-files', '--others', '--exclude-standard', '-z').decode().split('\0'):
        if not name:
            continue
        lines = (repo / name).read_text(encoding='utf-8').splitlines(keepends=True)
        if lines and not lines[-1].endswith('\n'):
            raise RuntimeError('New text file must end with newline: ' + name)
        diff = ''.join(difflib.unified_diff([], lines, fromfile='/dev/null', tofile='b/' + name))
        patch += (f'diff --git a/{name} b/{name}\nnew file mode 100644\n' + diff).encode()
    output.write_bytes(patch)
    return hashlib.sha256(patch).hexdigest()


def main():
    if git(SOURCE, 'rev-parse', 'HEAD').decode().strip() != UPSTREAM:
        raise RuntimeError('Unexpected upstream commit; update the port deliberately before exporting')
    patches = ROOT / 'patches'
    patches.mkdir(exist_ok=True)
    manifest = [{'path': 'patches/0001-horizon-platform.patch', 'directory': '.',
                 'sha256': export(SOURCE, patches / '0001-horizon-platform.patch')}]
    modules = configparser.ConfigParser()
    modules.read(SOURCE / '.gitmodules')
    initialized = []
    for section in modules.sections():
        path = modules[section]['path']
        repo = SOURCE / path
        if not (repo / '.git').exists():
            continue
        # Pin the gitlink from the upstream commit, not an accidentally moved checkout.
        commit = git(SOURCE, 'ls-tree', 'HEAD', path).decode().split()[2]
        if git(repo, 'rev-parse', 'HEAD').decode().strip() != commit:
            raise RuntimeError('Submodule revision mismatch: ' + path)
        initialized.append({'path': path, 'url': modules[section]['url'], 'commit': commit})
        if git(repo, 'status', '--porcelain').strip():
            name = f'{len(manifest)+1:04d}-' + path.replace('/', '-').lower() + '.patch'
            manifest.append({'path': 'patches/' + name, 'directory': path,
                             'sha256': export(repo, patches / name)})
    lock = {'upstream': {'url': 'https://github.com/dolphin-emu/dolphin.git', 'commit': UPSTREAM},
            'submodules': initialized, 'patches': manifest,
            'references': [
                {'url': 'https://github.com/xerpi/dolphin-switch',
                 'commit': '2079cf564f251fadcb5bb305c7c2faa9e8b8789c'},
                {'url': 'https://github.com/NaGaa95/dolphin-nx',
                 'commit': '84535ced59025dcfb2edd479c1b314325a0c9ddc'},
                {'url': 'https://github.com/switchbrew/libnx',
                 'commit': '7644c9b26099aa2d2145bc72a21ee24190e92085', 'tag': 'v4.12.0'},
                {'url': 'https://github.com/retronx-team/sys-clk',
                 'commit': '1fd95096eb08e21e390b50d8cc05223d0ebb6795',
                 'note': 'temporary override IPC API reference; not bundled'},
                {'url': 'https://github.com/hanai3Bi/Switch-OC-Suite',
                 'commit': '822556e6e4e627e5b9a444a934a82cc7f4a48db2',
                 'note': 'sysclkOC compatibility reference; not bundled'}]}
    (ROOT / 'source.lock.json').write_text(json.dumps(lock, indent=2) + '\n', encoding='utf-8', newline='\n')
    print(f'Exported {len(manifest)} patches and {len(initialized)} pinned submodules')


if __name__ == '__main__':
    main()

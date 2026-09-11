#!/usr/bin/env python3
"""Extract the official devkitPro image's toolchain without Docker or root.

Run on Linux/WSL. All downloads and writes are within .toolchain. The first
--resolve call records the manifest; ordinary runs require the checked-in lock.
No image entrypoint, package installer, or downloaded shell script is executed.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
CACHE = ROOT / '.toolchain' / 'downloads'
DEST = ROOT / '.toolchain' / 'image'
LOCK = ROOT / 'toolchain.lock.json'
REPO = 'devkitpro/devkita64'
BASE = f'https://registry-1.docker.io/v2/{REPO}'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--resolve', action='store_true')
    args = parser.parse_args()
    token_url = f'https://auth.docker.io/token?service=registry.docker.io&scope=repository:{REPO}:pull'
    with urllib.request.urlopen(token_url, timeout=60) as response:
        token = json.load(response)['token']
    headers = {'Authorization': 'Bearer ' + token,
               'Accept': ', '.join(['application/vnd.docker.distribution.manifest.v2+json',
                                   'application/vnd.docker.distribution.manifest.list.v2+json',
                                   'application/vnd.oci.image.index.v1+json',
                                   'application/vnd.oci.image.manifest.v1+json'])}

    def get_manifest(ref):
        req = urllib.request.Request(BASE + '/manifests/' + ref, headers=headers)
        with urllib.request.urlopen(req, timeout=60) as response:
            body = response.read()
        digest = 'sha256:' + hashlib.sha256(body).hexdigest()
        if ref.startswith('sha256:') and digest != ref:
            raise RuntimeError('Manifest digest mismatch')
        return json.loads(body), digest

    if args.resolve:
        manifest, digest = get_manifest('latest')
        if 'manifests' in manifest:
            ref = next(m['digest'] for m in manifest['manifests']
                       if m['platform']['architecture'] == 'amd64' and m['platform']['os'] == 'linux')
            manifest, digest = get_manifest(ref)
        LOCK.write_text(json.dumps({'image': REPO, 'manifest': digest, 'layers': manifest['layers']}, indent=2) + '\n')
        print('Pinned official image:', digest, flush=True)
    else:
        lock = json.loads(LOCK.read_text())
        manifest, digest = get_manifest(lock['manifest'])
        if manifest['layers'] != lock['layers']:
            raise RuntimeError('Layer lock mismatch')
    CACHE.mkdir(parents=True, exist_ok=True)
    DEST.mkdir(parents=True, exist_ok=True)
    for layer in manifest['layers']:
        digest = layer['digest'].split(':')[1]
        archive = CACHE / (digest + '.tar.gz')
        if not archive.exists():
            print(f"Downloading {layer['size'] // (1024 * 1024)} MiB layer {digest[:12]}", flush=True)
            req = urllib.request.Request(BASE + '/blobs/' + layer['digest'], headers=headers)
            partial = archive.with_suffix('.part')
            with urllib.request.urlopen(req, timeout=120) as response, partial.open('wb') as out:
                shutil.copyfileobj(response, out)
            partial.replace(archive)
        with archive.open('rb') as source:
            actual = hashlib.file_digest(source, 'sha256').hexdigest()
        if actual != digest:
            raise RuntimeError('Layer checksum mismatch: ' + str(archive))
        marker = CACHE / (digest + '.extracted')
        if marker.exists():
            continue
        print('Extracting toolchain/package metadata from', digest[:12], flush=True)
        with tarfile.open(archive, 'r:*') as tar:
            for member in tar:
                name = member.name.removeprefix('./')
                path = PurePosixPath(name)
                if not (name.startswith('opt/devkitpro/') or name.startswith('var/lib/pacman/local/')):
                    continue
                if path.is_absolute() or '..' in path.parts:
                    raise RuntimeError('Unsafe or unsupported image member: ' + name)
                if path.name.startswith('.wh.'):
                    # OCI whiteout: remove only the superseded member inside
                    # this workspace's extracted image, never a host path.
                    parent = (DEST / str(path.parent)).resolve()
                    target = parent / path.name[4:]
                    if path.name == '.wh..wh..opq':
                        targets = list(parent.iterdir()) if parent.exists() else []
                    else:
                        targets = [target]
                    for target in targets:
                        if not target.resolve().is_relative_to(DEST.resolve()):
                            raise RuntimeError('Escaping whiteout: ' + name)
                        if target.is_symlink() or target.is_file():
                            target.unlink()
                        elif target.is_dir():
                            shutil.rmtree(target)
                    continue
                # tarfile's data filter also rejects escaping links and special files.
                tar.extract(member, path=DEST, filter='data')
        marker.write_text('verified\n')
    print('DEVKITPRO=' + str(DEST / 'opt/devkitpro'), flush=True)


if __name__ == '__main__':
    main()

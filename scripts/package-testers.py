#!/usr/bin/env python3
"""Create the one-ZIP Phocoena hardware tester package from a local Switch build."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def load_tool(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'scripts' / f'{name}.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def write_text(path: Path, text: str):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding='utf-8', newline='\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=ROOT / 'build/nxvk/dolphin')
    parser.add_argument('--nxvk-source', type=Path,
                        default=Path(os.environ.get('PHOCOENA_NXVK_SOURCE',
                                                    '/var/tmp/dolphin-nxvk-69ec283d/source')),
                        help='Pinned NXVK source tree; defaults to the normal Phocoena build workspace')
    args = parser.parse_args()

    nro = args.build.resolve() / 'Binaries/Phocoena.nro'
    if not nro.is_file():
        raise SystemExit(f'Missing {nro}; build Phocoena first')
    nro_data = nro.read_bytes()
    report = load_tool('verify-nro').verify(nro_data)
    if report['name'] != 'Phocoena':
        raise SystemExit(f"NACP title is {report['name']!r}, expected 'Phocoena'; rebuild the current source")
    version = report['version']
    expected_version = None
    cmake = ROOT / 'upstream/dolphin/Source/Core/Horizon/CMakeLists.txt'
    for line in cmake.read_text(encoding='utf-8').splitlines():
        if line.startswith('set(HORIZON_PORT_VERSION '):
            expected_version = line.split('"')[1]
            break
    if version != expected_version:
        raise SystemExit(f'NRO version {version!r} does not match source version {expected_version!r}')

    lock_sha256 = hashlib.sha256((ROOT / 'source.lock.json').read_bytes()).hexdigest()
    if lock_sha256.encode() not in nro_data:
        raise SystemExit('NRO does not embed the current source.lock.json hash; rebuild after applying the tester overlay')

    stage_root = ROOT / 'dist' / f'Phocoena-v{version}-tester'
    if stage_root.exists():
        shutil.rmtree(stage_root)
    app = stage_root / 'switch' / 'Phocoena'
    app.mkdir(parents=True)
    shutil.copy2(nro, app / 'Phocoena.nro')
    shutil.copytree(ROOT / 'upstream/dolphin/Data/Sys', app / 'Sys')
    shutil.copy2(ROOT / 'upstream/dolphin/COPYING', app / 'COPYING')
    shutil.copytree(ROOT / 'upstream/dolphin/LICENSES', app / 'LICENSES')

    # NXVK/Mesa legal notices come from the exact pinned workspace used for the build.
    nxvk_licenses = args.nxvk_source / 'licenses'
    if nxvk_licenses.is_dir():
        shutil.copytree(nxvk_licenses, app / 'LICENSES' / 'NXVK', dirs_exist_ok=True)
    else:
        raise SystemExit(
            f'Pinned NXVK license directory not found at {nxvk_licenses}. '
            'Run scripts/build-phocoena.sh first, or pass --nxvk-source.')

    for directory in ['games', 'cache/shaders', 'cache/tmp', 'shaders', 'logs',
                      'diagnostics/sessions', 'diagnostics/bundles', 'config']:
        (app / directory).mkdir(parents=True, exist_ok=True)

    # Frozen tester baseline: mirrors the hardware-tested v0.1.26 behavior.
    # In particular: synchronous specialized shaders, no shader workers, FIFO 8,
    # CPU 1785 / GPU 768 performance profile, and no memory-clock writes.
    write_text(app / 'config' / 'switch.ini', '''# Phocoena tester baseline v0.1.27 — frozen from the hardware-tested v0.1.26 runtime
jit=0
native_jit=1
cached_interpreter=0
vsync=1
shader_cache=1
audio=1
dual_core=1
vulkan_submit_thread=1
vulkan_present_overlap=0
vulkan_triple_buffering=0
horizon_vulkan_push_descriptors=1
vulkan_present_completion_guard=1
native_vertex_loader=1
jit_small_code_pooling=1
fifo_batching=1
fifo_batch_size=8
horizon_ppc_fastmem=1
phocoena_auto_clocks=1
phocoena_clock_profile=performance
tp_benchmark_save=0
horizon_perf_sample_scale=1024
''')

    write_text(app / 'TESTER-README.txt', f'''PHOCOENA v{version} HARDWARE TEST BUILD

INSTALL
1. Extract this ZIP to the ROOT of the Switch SD card.
2. The app must end up at /switch/Phocoena/Phocoena.nro.
3. Put only your own GameCube dumps in /switch/Phocoena/games/ (RVZ, ISO, or GCM),
   or browse to another SD folder from Phocoena.
4. Launch Phocoena with full application memory/title takeover.

BASELINE — PLEASE LEAVE THESE SETTINGS ALONE
- AArch64 JIT + Dual Core
- Vulkan / NXVK at 1x
- Synchronous specialized shaders; shader compiler workers remain off
- FIFO batch size 8
- CPU target 1785 MHz / GPU target 768 MHz
- Memory clock is NEVER written by Phocoena

TEST
- Please try at least 5 GameCube games; more is better.
- Get into actual gameplay and play about 10 minutes per game when practical.
- Return to Phocoena between games. Each run is automatically saved as a diagnostics session.
- If a game fails, hangs, reboots, or runs poorly, leave the settings alone so the report is comparable.

WHEN FINISHED
1. Open About / Diagnostics.
2. Press X: Generate support bundle.
3. Copy the newest ZIP from /switch/Phocoena/diagnostics/bundles/ and send that one ZIP back.

PRIVACY
The support ZIP contains Phocoena settings/logs and per-game diagnostics. It intentionally excludes
GameCube images, save files, keys, Nintendo account data, and arbitrary files elsewhere on the SD card.
''')

    manifest = {
        'product': 'Phocoena',
        'version': version,
        'nro': 'switch/Phocoena/Phocoena.nro',
        'nro_sha256': report['sha256'],
        'source_lock_sha256': lock_sha256,
        'tester_profile': {
            'cpu_target_hz': 1785000000,
            'gpu_target_hz': 768000000,
            'memory_clock_write': False,
            'shader_mode': 'Synchronous specialized',
            'shader_compiler_workers': 0,
            'fifo_blocks': 8,
            'dual_core': True,
            'internal_resolution': '1x',
        },
        'privacy': 'No games, saves, keys, account data, or arbitrary SD files are included in generated support bundles.',
    }
    write_text(app / 'TESTER-MANIFEST.json', json.dumps(manifest, indent=2) + '\n')

    archive = ROOT / 'dist' / f'Phocoena-v{version}-tester.zip'
    archive.parent.mkdir(parents=True, exist_ok=True)
    if archive.exists():
        archive.unlink()
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as output:
        for path in sorted(stage_root.rglob('*')):
            name = path.relative_to(stage_root).as_posix() + ('/' if path.is_dir() else '')
            info = zipfile.ZipInfo(name, (2026, 9, 11, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = (0o40755 if path.is_dir() else 0o100644) << 16
            output.writestr(info, b'' if path.is_dir() else path.read_bytes())

    print(json.dumps({'tester_zip': str(archive), 'nro': str(nro),
                      'nro_sha256': report['sha256'], 'version': version}, indent=2))


if __name__ == '__main__':
    main()

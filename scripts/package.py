#!/usr/bin/env python3
"""Stage a built NRO, required Dolphin resources and the original smoke DOL for SD."""
import argparse
import hashlib
import importlib.util
import json
import re
from pathlib import Path
import shutil
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def module(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'scripts' / (name + '.py'))
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=ROOT / 'build/switch/native')
    parser.add_argument('--nxvk-source', type=Path, help='Pinned NXVK source, for driver license notices')
    args = parser.parse_args()
    nro = args.build.resolve() / 'Binaries/dolphin.nro'
    nro_data = nro.read_bytes()
    report = module('verify-nro').verify(nro_data)
    lock_sha256 = hashlib.sha256((ROOT / 'source.lock.json').read_bytes()).hexdigest()
    if lock_sha256.encode() not in nro_data:
        raise RuntimeError('NRO does not embed the current source.lock.json hash; rebuild before packaging')
    if report['name'] != 'Dolphin for Switch':
        raise RuntimeError('Unexpected NACP application name')
    version = report['version']
    if not re.fullmatch(r'\d+\.\d+\.\d+', version):
        raise RuntimeError('Expected a semantic release version in the NACP')
    release_root = ROOT / 'dist' / ('v' + version)
    stage = release_root / 'switch/dolphin'
    stage.mkdir(parents=True, exist_ok=True)
    shutil.copy2(nro, stage / 'dolphin.nro')
    shutil.copytree(ROOT / 'upstream/dolphin/Data/Sys', stage / 'Sys', dirs_exist_ok=True)
    shutil.copytree(ROOT / 'upstream/dolphin/LICENSES', stage / 'LICENSES', dirs_exist_ok=True)
    shutil.copy2(ROOT / 'upstream/dolphin/COPYING', stage / 'COPYING')
    for directory in ['games', 'config', 'cache/shaders', 'cache/tmp', 'shaders', 'logs']:
        (stage / directory).mkdir(parents=True, exist_ok=True)
    (stage / 'games/horizon-smoke.dol').write_bytes(module('make-smoke-dol').make_dol())
    (stage / 'games/SMOKE-TEST.txt').write_text(
        'horizon-smoke.dol is original test code. It intentionally shows no graphics.\n'
        'v0.1.4: 288 bytes, corrected 32-byte code-section padding.\n'
        'Expect Core state Running in logs; hold Plus+Minus to stop.\n'
        'This is not a playable game and does not establish game compatibility.\n')
    for name in ['source.lock.json', 'toolchain.lock.json', 'toolchain-packages.json',
                 'SWITCH_TESTING.md', 'GRAPHICS_RETEST.md', 'HARDWARE_TEST_RESULTS.md',
                 'KNOWN_ISSUES.md', 'CHANGELOG.md', 'BUILD_STATUS.md', 'NEXT_STEPS.md']:
        shutil.copy2(ROOT / name, stage / name)
    shutil.copy2(ROOT / 'docs/V0.1.13_PERFORMANCE.md', stage / 'V0.1.13_PERFORMANCE.md')
    shutil.copy2(ROOT / 'docs/V0.1.14_SYNC_FIX.md', stage / 'V0.1.14_SYNC_FIX.md')
    shutil.copy2(ROOT / 'docs/V0.1.15_FIFO_BATCHING.md', stage / 'V0.1.15_FIFO_BATCHING.md')
    shutil.copy2(ROOT / 'docs/V0.1.16_JIT_POOLING.md', stage / 'V0.1.16_JIT_POOLING.md')
    shutil.copy2(ROOT / 'docs/V0.1.17_HORIZON_FASTMEM.md', stage / 'V0.1.17_HORIZON_FASTMEM.md')
    shutil.copy2(ROOT / 'docs/V0.1.18_AUTO_CLOCKS.md', stage / 'V0.1.18_AUTO_CLOCKS.md')
    shutil.copy2(ROOT / 'docs/V0.1.19_FIFO_BATCH16.md', stage / 'V0.1.19_FIFO_BATCH16.md')
    shutil.copy2(ROOT / 'docs/TP_BENCHMARK.md', stage / 'TP_BENCHMARK.md')
    shutil.copy2(ROOT / 'docs/V0.1.23_PERF_SAMPLING.md', stage / 'V0.1.23_PERF_SAMPLING.md')
    shutil.copy2(ROOT / 'docs/V0.1.24_PUSH_DESCRIPTORS.md', stage / 'V0.1.24_PUSH_DESCRIPTORS.md')
    shutil.copy2(ROOT / 'docs/V0.1.25_BUILD_PROVENANCE.md', stage / 'V0.1.25_BUILD_PROVENANCE.md')
    report['upstream'] = json.loads((ROOT / 'source.lock.json').read_text())['upstream']['commit']
    report['source_lock_sha256'] = lock_sha256
    elf = args.build.resolve() / 'Binaries/dolphin.elf'
    if elf.is_file():
        report['companion_elf_sha256'] = hashlib.sha256(elf.read_bytes()).hexdigest()
    report['cpu'] = 'Interpreter default; Cached Interpreter; experimental AArch64 JIT with emitter self-test required'
    vulkan = b'NXVK 69ec283dbda64e65347a36274efb349122e85363' in nro_data
    if vulkan:
        if not args.nxvk_source or not (args.nxvk_source / 'licenses').is_dir():
            raise RuntimeError('Vulkan packaging requires --nxvk-source with license notices')
        shutil.copytree(args.nxvk_source / 'licenses', stage / 'LICENSES/NXVK', dirs_exist_ok=True)
        (stage / 'NXVK-COPYING.txt').write_text(
            'NXVK fork additions: GPL-2.0-or-later; Mesa retains per-file licenses.\n'
            'Complete corresponding driver source and build recipes are included\n'
            'inside the matching Dolphin source archive at dependencies/nxvk-source.tar.gz.\n'
            'See LICENSES/NXVK and VULKAN_BUILD.md. No games are included.\n')
        shutil.copy2(ROOT / 'experiments/nxvk/source.lock.json', stage / 'nxvk.source.lock.json')
        shutil.copy2(ROOT / 'docs/VULKAN_TESTING.md', stage / 'VULKAN_TESTING.md')
        shutil.copy2(ROOT / 'docs/VULKAN_BUILD.md', stage / 'VULKAN_BUILD.md')
        shutil.copy2(ROOT / 'docs/VULKAN_SUBMISSION.md', stage / 'VULKAN_SUBMISSION.md')
        shutil.copy2(ROOT / 'docs/VULKAN_PROFILING.md', stage / 'VULKAN_PROFILING.md')
        shutil.copy2(ROOT / 'build/nxvk/dolphin/BUILD_INPUTS.json', stage / 'BUILD-INPUTS.json')
        report['renderer'] = (f'Existing Dolphin Vulkan backend / static NXVK; v{version} '
                              'verified GX sampler push-descriptor candidate, hardware test pending')
        report['vulkan_submission'] = 'Worker On by default; configurable Off for comparison; shader workers remain Off'
        report['vertex_loader'] = 'AArch64 JIT by default; configurable Software rollback; per-format fallback logged'
        report['clock_control'] = ('Phocoena auto clocks On by default: performance profile CPU 1785 MHz '
                                   'and GPU 768 MHz; legacy profile restores 1581/614.4; temporary manager '
                                   'override preferred; direct libnx fallback; Off performs no writes')
        report['memory_clock'] = 'read-only diagnostics only; no memory-clock write or restore path'
        report['fifo_batching'] = ('16 blocks by default; Settings/config A/B values 1/4/8/16; '
                                   '8 reproduces v0.1.18; SyncGPU forces one block')
        report['hot_scope_sampling'] = ('1/1024 default; horizon_perf_sample_scale=64 restores '
                                        'v0.1.22 developer profiling overhead for A/B')
        report['gx_sampler_descriptors'] = ('VK_KHR_push_descriptor requested by default for the '
                                            '16-binding GX sampler set; capability-gated legacy '
                                            'fallback; horizon_vulkan_push_descriptors=0 for A/B')
        report['nxvk_commit'] = '69ec283dbda64e65347a36274efb349122e85363'
        report['audio'] = 'libnx audout output; DSP HLE; enabled for OpenGL and Vulkan builds.'
    else:
        report['renderer'] = f'Existing Dolphin OpenGL backend; v{version} core boot awaits hardware retest'
    report['files'] = {}
    for path in sorted(stage.rglob('*')):
        if path.is_file() and path.name != 'BUILD-MANIFEST.json':
            report['files'][path.relative_to(stage).as_posix()] = hashlib.sha256(path.read_bytes()).hexdigest()
    (stage / 'BUILD-MANIFEST.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8', newline='\n')
    archive = ROOT / 'dist' / ('dolphin-horizon-v' + version + '.zip')
    # Stable entry ordering and timestamps; the NRO itself may retain build paths.
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as output:
        for path in sorted((release_root / 'switch').rglob('*')):
            name = path.relative_to(release_root).as_posix()
            info = zipfile.ZipInfo(name + ('/' if path.is_dir() else ''), (2026, 8, 28, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = (0o40755 if path.is_dir() else 0o100644) << 16
            output.writestr(info, b'' if path.is_dir() else path.read_bytes())
    print(json.dumps({'nro': str(stage / 'dolphin.nro'), 'zip': str(archive),
                      'nro_sha256': report['sha256'], 'hardware_tested': False}, indent=2))


if __name__ == '__main__':
    main()

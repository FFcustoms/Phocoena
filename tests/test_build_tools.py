# SPDX-License-Identifier: GPL-2.0-or-later
import importlib.util
from pathlib import Path
import struct
import hashlib
import json
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'scripts' / (name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class BuildToolsTests(unittest.TestCase):
    def test_vulkan_release_build_cannot_reuse_cross_version_objects(self):
        build = (ROOT / 'experiments/nxvk/dolphin-build.sh').read_text()
        record = (ROOT / 'experiments/nxvk/record-dolphin-build.py').read_text()
        verify = (ROOT / 'scripts/verify-elf.py').read_text()
        self.assertIn('native="$NXVK_WORK/dolphin-v$version"', build)
        self.assertNotIn('native="$NXVK_WORK/dolphin-v0.1.9"', build)
        self.assertIn('touch "$native/source/$relative"', build)
        self.assertIn("native_root = WORK / f'dolphin-v{version}'", record)
        self.assertIn('_ZN6Vulkan12StateTracker21UpdateGXDescriptorSetEv', verify)
        self.assertIn('_ZN7Horizon31RecordVulkanPushDescriptorWriteEv', verify)

    def test_original_smoke_dol_layout(self):
        dol = load('make-smoke-dol').make_dol()
        # Dolphin's real DolReader rounds each section read up to 32 bytes.
        # The original 268-byte fixture was rejected before Core::Init.
        for index in range(18):
            offset = struct.unpack_from('>I', dol, index * 4)[0]
            size = struct.unpack_from('>I', dol, 0x90 + index * 4)[0]
            if size:
                self.assertLessEqual(offset + ((size + 31) & ~31), len(dol),
                                     'DolReader would read beyond the file')
        self.assertEqual(len(dol), 0x120)
        for offset, expected in [(0, 0x100), (0x48, 0x80003100), (0x90, 32), (0xE0, 0x80003100)]:
            self.assertEqual(struct.unpack_from('>I', dol, offset)[0], expected)
        self.assertEqual(struct.unpack_from('>III', dol, 0x100), (0x3860002A, 0x38630001, 0x4BFFFFFC))
        self.assertEqual(struct.unpack_from('>5I', dol, 0x10C), (0x60000000,) * 5)

    def test_smoke_dol_rejects_truncated_aligned_reads_and_bad_entry(self):
        smoke = load('make-smoke-dol')
        dol = smoke.make_dol()
        smoke.validate_dol(dol)
        original = bytearray(dol[:0x10C])
        struct.pack_into('>I', original, 0x90, 12)
        with self.assertRaisesRegex(ValueError, 'aligned read'):
            smoke.validate_dol(original)
        for size in [0, 0xFF, len(dol) - 1]:
            with self.assertRaises(ValueError):
                smoke.validate_dol(dol[:size])
        bad_entry = bytearray(dol)
        struct.pack_into('>I', bad_entry, 0xE0, 0x80004000)
        with self.assertRaisesRegex(ValueError, 'entry point'):
            smoke.validate_dol(bad_entry)

    def test_nro_rejects_truncated_and_foreign_files(self):
        verify = load('verify-nro').verify
        for data in [b'', b'\x7fELF' + bytes(1000), b'NRO0', bytes(1024)]:
            with self.assertRaises(ValueError): verify(data)

    def test_nro_ranges_and_metadata(self):
        verify = load('verify-nro').verify
        # A structural fixture only; never distributed as an executable.
        data = bytearray(0x3000 + 0x38 + 0x4000)
        data[0x10:0x14] = b'NRO0'
        struct.pack_into('<III', data, 0x14, 0, 0x3000, 0)
        struct.pack_into('<IIIIII', data, 0x20, 0, 0x1000, 0x1000, 0x1000, 0x2000, 0x1000)
        struct.pack_into('<I', data, 4, 0x80)
        data[0x80:0x84] = b'MOD0'
        data[0x3000:0x3004] = b'ASET'
        struct.pack_into('<QQ', data, 0x3018, 0x38, 0x4000)
        data[0x3038:0x303C] = b'Test'
        data[0x6098:0x609D] = b'0.1.1'
        self.assertEqual(verify(data)['name'], 'Test')
        self.assertEqual(verify(data)['version'], '0.1.1')
        struct.pack_into('<I', data, 0x28, 0) # overlapping read-only segment
        with self.assertRaises(ValueError): verify(data)


@unittest.skipUnless(shutil.which('pwsh') or shutil.which('powershell'), 'PowerShell cleanup tests run on Windows')
class ReleaseCleanupTests(unittest.TestCase):
    def setUp(self):
        parent = ROOT / 'build/host-tests'
        parent.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix='cleanup-', dir=parent)
        self.project = Path(self.temp.name).resolve()
        assert self.project.is_relative_to(parent.resolve())
        self.addCleanup(self.temp.cleanup)
        self.dist = self.project / 'dist'
        self.dist.mkdir()
        self.artifacts = {}
        for name in ['v0.1.2/switch/dolphin/dolphin.nro', 'dolphin-horizon-v0.1.2.zip',
                     'dolphin-horizon-v0.1.2-source.tar.gz']:
            path = self.dist / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b'new-release-fixture')
            self.artifacts['dist/' + name] = {'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}
        (self.dist / 'v0.1.2/RELEASE-VERIFICATION.json').write_text(
            json.dumps({'version': '0.1.2', 'zip_crc': 'PASS', 'artifacts': self.artifacts}))
        for base in ['v0.1.1/switch/dolphin', 'switch/dolphin']:
            path = self.dist / base
            path.mkdir(parents=True)
            (path / 'dolphin.nro').write_bytes(b'old-release-fixture')
            (path / 'BUILD-MANIFEST.json').write_text(json.dumps({
                'name': 'Dolphin for Switch',
                'files': {'dolphin.nro': hashlib.sha256(b'old-release-fixture').hexdigest()}}))
        for version in ['0.1', '0.1.1']:
            (self.dist / f'dolphin-horizon-v{version}.zip').write_bytes(b'old-zip')
            (self.dist / f'dolphin-horizon-v{version}-source.tar.gz').write_bytes(b'old-source')
        (self.dist / 'logs.zip').write_bytes(b'user-data')
        (self.dist / 'v0.1.3').mkdir()  # Never retire a newer version.

    def run_cleanup(self, *args):
        return subprocess.run([shutil.which('pwsh') or shutil.which('powershell'), '-NoProfile',
                               '-File', str(ROOT / 'scripts/cleanup-releases.ps1'),
                               '-ProjectRoot', str(self.project), '-KeepVersion', '0.1.2', *args],
                              capture_output=True, text=True)

    def test_dry_run_then_remove_only_older_generated_releases(self):
        result = self.run_cleanup()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue((self.dist / 'v0.1.1').exists())
        self.assertFalse((self.project / 'build/release-history').exists())
        result = self.run_cleanup('-Apply')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        for name in ['switch', 'v0.1.1', 'dolphin-horizon-v0.1.zip', 'dolphin-horizon-v0.1.1.zip']:
            self.assertFalse((self.dist / name).exists())
        for name in ['logs.zip', 'v0.1.3', 'v0.1.2', 'dolphin-horizon-v0.1.2.zip',
                     'dolphin-horizon-v0.1.2-source.tar.gz']:
            self.assertTrue((self.dist / name).exists())
        for version in ['0.1.0', '0.1.1']:
            history = self.project / 'build/release-history' / version
            self.assertEqual((history / 'source.tar.gz').read_bytes(), b'old-source')
            self.assertTrue((history / 'BUILD-MANIFEST.json').exists())

    def test_modified_replacement_prevents_all_cleanup(self):
        (self.dist / 'v0.1.2/switch/dolphin/dolphin.nro').write_bytes(b'changed')
        result = self.run_cleanup('-Apply')
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue((self.dist / 'v0.1.1').exists())
        self.assertTrue((self.dist / 'dolphin-horizon-v0.1.1.zip').exists())

    def test_script_directory_default_on_windows_powershell(self):
        script = self.project / 'scripts/cleanup-releases.ps1'
        script.parent.mkdir()
        shutil.copyfile(ROOT / 'scripts/cleanup-releases.ps1', script)
        # A pwsh 7 parent may export its own incompatible module search path.
        # Let Windows PowerShell construct its own standard module locations.
        environment = {key: value for key, value in os.environ.items() if key.lower() != 'psmodulepath'}
        result = subprocess.run([shutil.which('powershell'), '-NoProfile', '-ExecutionPolicy', 'Bypass',
                                 '-File', str(script), '-KeepVersion', '0.1.2'],
                                capture_output=True, text=True, env=environment)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue((self.dist / 'v0.1.1').exists())
        self.assertFalse((self.project / 'build/release-history').exists())

    def test_unrecognized_user_file_prevents_all_cleanup(self):
        extra = self.dist / 'v0.1.1/switch/dolphin/my-save.dat'
        extra.write_bytes(b'do-not-delete')
        result = self.run_cleanup('-Apply')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(extra.read_bytes(), b'do-not-delete')
        self.assertTrue((self.dist / 'dolphin-horizon-v0.1.1.zip').exists())


if __name__ == '__main__': unittest.main()

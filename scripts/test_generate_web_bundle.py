#!/usr/bin/env python3
"""Exercise the actual bundler with temporary frontend trees, not device data."""

import gzip
from pathlib import Path
import runpy
import subprocess
import sys
import tempfile
import types
import unittest
from unittest.mock import Mock, patch

import generate_web_bundle as bundle


class WebBundleTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        for name, data in {
            'interface/package.json': b'{"scripts":{"build":"fixture"}}',
            'interface/package-lock.json': b'{"lockfileVersion":3}',
            'interface/vite.config.js': b'export default {};',
            'interface/src/routes/+page.svelte': b'<p>fixture</p>',
            'interface/static/branding/logo.png': b'fixture-logo',
            'tools/compress_web_assets.sh': b'gzip -n -9 -k -f',
            'scripts/generate_web_bundle.py': b'generator fixture identity',
            'scripts/embed_web_assets.py': b'prehook fixture identity',
        }.items():
            self.write(name, data)
        self.assets = {route + '.html': ('<html>' + route + '</html>').encode()
                       for route in bundle.ROUTES}
        self.assets.update({
            '_app/version.json': b'{"version":"fixture"}',
            '_app/env.js': b'export const env={};',
            '_app/immutable/entry/app.fixture.js': b'export const app="original unique entry";',
            '_app/immutable/assets/app.fixture.css': b'body{color:#123456}',
            '_app/immutable/assets/font.fixture.woff2': b'fixture-font-bytes',
            'branding/logo.png': b'fixture-logo',
        })
        self.seed_outputs()

    def write(self, name, data):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        return path

    def seed_outputs(self):
        for name, data in self.assets.items():
            self.write('interface/build/' + name, data)
            if name.endswith(('.js', '.css', '.json')):
                self.write('interface/build/' + name + '.gz', gzip.compress(data, mtime=0))

    def stamp(self):
        # Vite/gzip are the only substituted boundaries. The production build,
        # fingerprint, validation, selection and generation owners run below.
        with patch.object(bundle.subprocess, 'run') as run:
            record = bundle.build(self.root)
        self.assertEqual(run.call_count, 2)
        return record

    def test_missing_build_stamp_fails_closed(self):
        with self.assertRaisesRegex(ValueError, 'stamp missing'):
            bundle.generate(self.root, self.root / 'generated.h')
        self.assertFalse((self.root / 'generated.h').exists())

    def test_missing_output_tree_fails_closed(self):
        self.stamp()
        (self.root / 'interface/build').rename(self.root / 'saved-build')
        with self.assertRaisesRegex(ValueError, 'missing or unsafe directory'):
            bundle.validate(self.root)

    def test_missing_route_or_app_entry_cannot_be_stamped(self):
        for name in ('profiles.html', '_app/immutable/entry/app.fixture.js.gz'):
            with self.subTest(name=name):
                path = self.root / 'interface/build' / name
                old = path.read_bytes()
                path.unlink()
                with self.assertRaisesRegex(ValueError, 'missing'):
                    self.stamp()
                self.assertFalse((self.root / bundle.STAMP).exists())
                path.write_bytes(old)

    def test_every_build_input_class_invalidates_stamp(self):
        for name in ('interface/src/routes/+page.svelte', 'interface/package.json',
                     'interface/package-lock.json', 'interface/vite.config.js',
                     'interface/static/branding/logo.png', 'tools/compress_web_assets.sh',
                     'scripts/generate_web_bundle.py', 'scripts/embed_web_assets.py'):
            with self.subTest(name=name):
                self.stamp()
                path = self.root / name
                old = path.read_bytes()
                path.write_bytes(old + b' changed')
                with self.assertRaisesRegex(ValueError, 'stale or modified|branding differs'):
                    bundle.validate(self.root)
                path.write_bytes(old)

    def test_new_input_is_detected_and_build_caches_are_excluded(self):
        self.stamp()
        for name in ('node_modules/cache', '.svelte-kit/generated', '.vite/cache'):
            self.write('interface/' + name, b'not frontend input')
        bundle.validate(self.root)
        self.write('interface/src/new.js', b'new input')
        with self.assertRaisesRegex(ValueError, 'stale or modified'):
            bundle.validate(self.root)

    def test_nested_source_directory_named_build_is_not_excluded(self):
        self.stamp()
        self.write('interface/src/lib/build/real-source.js', b'new source')
        with self.assertRaisesRegex(ValueError, 'stale or modified'):
            bundle.validate(self.root)

    def test_missing_or_changed_built_branding_cannot_be_stamped(self):
        logo = self.root / 'interface/build/branding/logo.png'
        logo.unlink()
        with self.assertRaisesRegex(ValueError, 'branding differs'):
            self.stamp()
        logo.write_bytes(b'wrong logo')
        with self.assertRaisesRegex(ValueError, 'branding differs'):
            self.stamp()

    def test_output_tampering_missing_files_and_extra_files_invalidate(self):
        self.stamp()
        path = self.root / 'interface/build/settings.html'
        original = path.read_bytes()
        path.write_bytes(original + b' altered')
        with self.assertRaisesRegex(ValueError, 'stale or modified'):
            bundle.validate(self.root)
        path.write_bytes(original)
        path = self.root / 'interface/build/_app/immutable/assets/font.fixture.woff2'
        original = path.read_bytes()
        path.unlink()
        with self.assertRaisesRegex(ValueError, 'stale or modified'):
            bundle.validate(self.root)
        path.write_bytes(original)
        self.write('interface/build/extra.txt', b'output tampering')
        with self.assertRaisesRegex(ValueError, 'stale or modified'):
            bundle.validate(self.root)

    def test_corrupt_or_mismatched_gzip_cannot_be_stamped(self):
        path = self.root / 'interface/build/_app/immutable/entry/app.fixture.js.gz'
        invalid_deflate = b'\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\x03\x07' + b'\x00' * 8
        for data in (b'not gzip', invalid_deflate, gzip.compress(b'other source', mtime=0)):
            with self.subTest(data=data):
                path.write_bytes(data)
                with self.assertRaisesRegex(ValueError, 'compressed asset'):
                    self.stamp()

    def test_malicious_and_unexpected_app_paths_are_rejected(self):
        for name in ('_app/immutable/chunks/evil".js', '_app/../settings.json',
                     '_app/private.json', '_app/immutable/chunks/evil\\name.js'):
            with self.subTest(name=name):
                outputs = dict(self.assets)
                outputs.update({key + '.gz': gzip.compress(data, mtime=0)
                                for key, data in self.assets.items() if key.endswith(('.js', '.css', '.json'))})
                outputs[name] = b'private'
                with self.assertRaises(ValueError):
                    bundle.select_assets(outputs)

    def test_symlink_input_output_and_destination_rejected(self):
        for name in ('interface/src/link', 'interface/build/link'):
            with self.subTest(name=name):
                link = self.root / name
                link.symlink_to(self.root / 'interface/package.json')
                with self.assertRaisesRegex(ValueError, 'symlinks'):
                    self.stamp()
                link.unlink()
        self.stamp()
        destination = self.root / 'generated.h'
        destination.symlink_to(self.root / 'interface/package.json')
        with self.assertRaisesRegex(ValueError, 'symlink'):
            bundle.generate(self.root, destination)

    def test_protected_paths_are_never_embedded(self):
        for name in ('audio/custom.mul', 'v1profiles/Road.json', 'v1devices.json',
                     'v1restore_transaction.json', 'v1simple_backup.json', 'custom.html'):
            self.write('interface/build/' + name, b'PRIVATE-DATA-SENTINEL')
        self.stamp()
        _, assets = bundle.validate(self.root)
        self.assertFalse(any(b'PRIVATE-DATA-SENTINEL' in data for data in assets.values()))
        output = self.root / 'generated.h'
        bundle.generate(self.root, output)
        self.assertNotIn('v1profiles', output.read_text())

    def test_selected_compression_matches_deployment(self):
        self.stamp()
        _, assets = bundle.validate(self.root)
        for name in ('/_app/version.json', '/_app/env.js'):
            self.assertIn(name, assets)
            self.assertIn(name + '.gz', assets)
        self.assertNotIn('/_app/immutable/entry/app.fixture.js', assets)
        self.assertIn('/_app/immutable/entry/app.fixture.js.gz', assets)
        self.assertIn('/_app/immutable/assets/font.fixture.woff2', assets)
        self.assertIn('/branding/logo.png', assets)
        self.assertNotIn('/index.html.gz', assets)

    def test_generation_is_deterministic_and_does_not_rewrite(self):
        self.stamp()
        output = self.root / 'generated.h'
        bundle.generate(self.root, output)
        first = output.read_bytes()
        first_mtime = output.stat().st_mtime_ns
        bundle.generate(self.root, output)
        self.assertEqual(first, output.read_bytes())
        self.assertEqual(first_mtime, output.stat().st_mtime_ns)
        second = self.root / 'other.h'
        bundle.generate(self.root, second)
        self.assertEqual(first, second.read_bytes())

    def test_changed_content_requires_rebuild_then_changes_header(self):
        self.stamp()
        output = self.root / 'generated.h'
        bundle.generate(self.root, output)
        original = output.read_bytes()
        self.write('interface/src/routes/+page.svelte', b'<p>new content</p>')
        self.assets['index.html'] = b'<html>new content</html>'
        self.seed_outputs()
        with self.assertRaisesRegex(ValueError, 'stale or modified'):
            bundle.generate(self.root, output)
        self.assertEqual(original, output.read_bytes())
        self.stamp()
        bundle.generate(self.root, output)
        self.assertNotEqual(original, output.read_bytes())

    def test_failed_build_revokes_previous_stamp(self):
        self.stamp()
        with patch.object(bundle.subprocess, 'run', side_effect=subprocess.CalledProcessError(1, 'vite')):
            with self.assertRaises(subprocess.CalledProcessError):
                bundle.build(self.root)
        self.assertFalse((self.root / bundle.STAMP).exists())

    def test_inputs_changing_during_build_cannot_be_stamped(self):
        def change(*args, **kwargs):
            self.write('interface/src/routes/+page.svelte', b'changed during compilation')
        with patch.object(bundle.subprocess, 'run', side_effect=change):
            with self.assertRaisesRegex(ValueError, 'inputs changed during build'):
                bundle.build(self.root)
        self.assertFalse((self.root / bundle.STAMP).exists())

    def test_invalid_stamp_rejected(self):
        self.write(bundle.STAMP, b'not json')
        with self.assertRaisesRegex(ValueError, 'invalid web build stamp'):
            bundle.validate(self.root)

    def test_firmware_verification_requires_current_fingerprint_paths_and_payloads(self):
        record = self.stamp()
        _, assets = bundle.validate(self.root)
        marker = bundle.fingerprint(record).encode() + b'\0'
        parts = [(name.encode() + b'\0', data) for name, data in assets.items()]
        full = marker + b''.join(name + data for name, data in parts)
        firmware = self.write('firmware.bin', full)
        bundle.verify_firmware(self.root, firmware)
        for corrupted in (full.replace(marker, b'old bundle'),
                          full.replace(b'/index.html\0', b'/other.html\0'),
                          full.replace(assets['/index.html'], b'<html>other</html>')):
            firmware.write_bytes(corrupted)
            with self.assertRaisesRegex(ValueError, 'firmware'):
                bundle.verify_firmware(self.root, firmware)

    def run_hook(self, targets, environment='waveshare-349'):
        fake_script = types.ModuleType('SCons.Script')
        fake_script.COMMAND_LINE_TARGETS = targets
        env = Mock()
        build_dir = self.root / '.pio/build' / environment
        env.subst.side_effect = lambda name: {'$PROJECT_DIR': str(self.root), '$BUILD_DIR': str(build_dir)}[name]
        env.Exit.side_effect = SystemExit
        script = Path(__file__).with_name('embed_web_assets.py')
        with patch.dict(sys.modules, {'SCons': types.ModuleType('SCons'), 'SCons.Script': fake_script}):
            runpy.run_path(str(script), init_globals={'Import': lambda name: None, 'env': env})
        return env, build_dir

    def test_pre_hook_uses_environment_build_path_and_registers_header(self):
        self.stamp()
        for environment in ('waveshare-349', 'esp32-s3-car-install'):
            with self.subTest(environment=environment):
                env, build_dir = self.run_hook([], environment)
                self.assertTrue((build_dir / 'include/embedded_web_assets.generated.h').is_file())
                env.AppendUnique.assert_called_once_with(CPPPATH=[str(build_dir / 'include')])
                env.Exit.assert_not_called()

    def test_clean_only_hook_needs_no_frontend_but_build_upload_still_validate(self):
        env, _ = self.run_hook(['clean'])
        env.subst.assert_not_called()
        env.AppendUnique.assert_not_called()
        for targets in ([], ['upload'], ['buildfs'], ['clean', 'upload']):
            with self.subTest(targets=targets):
                with self.assertRaises(SystemExit):
                    self.run_hook(targets)


if __name__ == '__main__':
    unittest.main()

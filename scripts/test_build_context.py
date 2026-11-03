#!/usr/bin/env python3
"""Independent geometry and selected-build-context regression cases."""

from __future__ import annotations

import importlib.util
import json
import os
import shutil
import subprocess
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

import build_context as context
import check_littlefs_image_compatibility as compatibility
import report_flash_package_size as sizes
import check_memory_headroom as memory
import check_release_image_info as image_info
import write_release_manifests as manifests

ROOT = Path(__file__).resolve().parents[1]

PREFIX = 'nvs,data,nvs,0x9000,0x8000,\napp,app,factory,0x20000,0xD80000,\n'
STORAGE = 'storage,data,spiffs,0xDA0000,0x250000,\n'
CASES = (
    ('canonical', PREFIX + STORAGE, (2424832,) * 4),
    ('duplicate_storage', PREFIX + 'storage,data,spiffs,0xDA0000,16,\nstorage,data,spiffs,0xDA0000,32,\n', (32, 16, 16, 16)),
    ('malformed_unrelated_before', 'broken\n' + PREFIX + STORAGE, ('ValueError', 2424832, 2424832, 2424832)),
    ('malformed_unrelated_after', PREFIX + STORAGE + 'broken\n', ('ValueError', 2424832, 2424832, 2424832)),
    ('bad_unrelated_offset', PREFIX + 'other,data,x,bad,32,\n' + STORAGE, ('ValueError', 2424832, 2424832, 2424832)),
    ('bad_storage_offset', PREFIX + 'storage,data,spiffs,bad,32,\n', ('ValueError', 32, 32, 32)),
    ('bad_storage_size', PREFIX + 'storage,data,spiffs,0xDA0000,bad,\n', ('ValueError',) * 4),
    ('malformed_storage_row', PREFIX + 'storage,data,spiffs,0xDA0000\n', ('ValueError', 'IndexError', 'IndexError', 'IndexError')),
    ('storage_only', STORAGE, ('ValueError', 2424832, 2424832, 2424832)),
    ('missing_storage', PREFIX, ('ValueError', 'RuntimeError', 'RuntimeError', 'RuntimeError')),
    ('missing_nvs', 'app,app,factory,0x20000,0xD80000,\n' + STORAGE, ('ValueError', 2424832, 2424832, 2424832)),
    ('overlapping_geometry', 'nvs,data,nvs,0x9000,0x8000,\napp,app,factory,0x10000,0xD80000,\nstorage,data,spiffs,0x10000,64,\n', (64,) * 4),
    ('negative_storage_size', PREFIX + 'storage,data,spiffs,0xDA0000,-16,\n', (-16,) * 4),
    ('k_suffix_is_not_numeric', PREFIX + 'storage,data,spiffs,0xDA0000,64K,\n', ('ValueError',) * 4),
    ('comments_and_extra_columns', '# comment\n\n' + PREFIX + ' storage ,data,spiffs,0xDA0000, 0x250000 ,ignored,extra\n', (2424832,) * 4),
)


class GeometryTests(unittest.TestCase):
    def test_existing_four_reader_contracts(self):
        littlefs = types.ModuleType("littlefs")
        littlefs.LittleFS = object
        errors = types.ModuleType("littlefs.errors")
        errors.LittleFSError = RuntimeError
        with patch.dict(sys.modules, {"littlefs": littlefs, "littlefs.errors": errors}):
            spec = importlib.util.spec_from_file_location("capacity_geometry", ROOT / "interface/scripts/littlefs-capacity.py")
            capacity = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(capacity)
        readers = (manifests.read_partitions, sizes.read_storage_partition_size,
                   compatibility.read_storage_partition_size, capacity.read_storage_partition_size)
        self.assertIs(manifests.read_partitions, context.read_partitions)
        for reader in readers[1:]:
            self.assertIs(reader, context.read_storage_partition_size)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "partitions.csv"
            for name, source, expected in CASES:
                path.write_text(source)
                for index, reader in enumerate(readers):
                    with self.subTest(case=name, reader=index):
                        try:
                            result = reader(path)
                            actual = result["storage"].size if index == 0 else result
                        except Exception as exc:
                            actual = type(exc).__name__
                        self.assertEqual(actual, expected[index])

    def test_first_storage_does_not_consume_later_rows(self):
        def rows(_):
            yield ["storage", "data", "spiffs", "ignored offset", "16"]
            raise AssertionError("later rows must not be consumed")
        with patch.object(context, "partition_rows", rows):
            self.assertEqual(context.read_storage_partition_size(Path("unused")), 16)


class SelectedContextTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="selected build ")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.core = self.root / "custom core"
        self.log = self.root / "calls.jsonl"
        self.python = self.root / "bin/selected-python"
        self.pio = self.root / "bin/selected-pio"
        (self.root / "scripts").mkdir()
        shutil.copyfile(ROOT / "scripts/build_context.py", self.root / "scripts/build_context.py")
        (self.root / "platformio.ini").write_text("configuration belongs to PlatformIO\n")
        self.write_tool(self.pio, f'''#!{sys.executable}
import json, os, pathlib, sys
with pathlib.Path({str(self.log)!r}).open('a') as log:
    log.write(json.dumps(['pio', *sys.argv[1:]]) + '\\n')
if os.environ.get('CONTEXT_FAIL_PIO'): raise SystemExit(41)
if sys.argv[1:] != ['system', 'info', '--json-output']: raise SystemExit('unexpected PIO effect')
print(json.dumps({{'python_exe': {{'value': {str(self.python)!r}}}, 'core_dir': {{'value': {str(self.core)!r}}}}}))
''')
        self.write_tool(self.python, f'''#!{sys.executable}
import json, os, pathlib, subprocess, sys
with pathlib.Path({str(self.log)!r}).open('a') as log:
    log.write(json.dumps(['python', *sys.argv[1:]]) + '\\n')
if os.environ.get('CONTEXT_FAIL_PYTHON'): raise SystemExit(42)
assert os.environ['PLATFORMIO_CORE_DIR'] == {str(self.core)!r}
environment = dict(os.environ, PYTHONPATH={str(self.root / 'fake-python')!r})
raise SystemExit(subprocess.call([{sys.executable!r}, '-S', *sys.argv[1:]], env=environment))
''')
        packages = self.root / "fake-python/platformio"
        for relative in ("", "project", "platform", "package", "package/manager"):
            path = packages / relative
            path.mkdir(parents=True, exist_ok=True)
            (path / "__init__.py").write_text("")
        (packages / "project/config.py").write_text(f'''
from pathlib import Path
import json, os
class ProjectConfig:
    @classmethod
    def get_instance(cls, path=None):
        if path is not None: assert Path(path) == Path({str(self.root / 'platformio.ini')!r})
        return cls()
    def get(self, section, key, default=None):
        if section == 'platformio':
            return {{'build_dir': {str(self.root / 'selected-builds')!r}}}.get(key, default)
        assert section in ('env:waveshare-349', 'env:esp32-s3-car-install')
        car = section.endswith('car-install')
        values = {{'platform': os.environ.get('CONTEXT_PLATFORM_SPEC', 'fixture/custom-platform@4.0.0'), 'platform_packages': json.loads(os.environ.get('CONTEXT_PACKAGES', '[]')), 'board': 'selected-board', 'board_build.partitions': 'geometry/car.csv' if car else 'geometry/default.csv',
                 'board_build.arduino.memory_type': 'opi_opi' if car else 'qio_opi',
                 'board_build.flash_mode': 'qio', 'board_build.f_flash': '80000000L', 'board_build.flash_size': '16MB',
                 'board_upload.flash_size': '16MB', 'board_upload.maximum_size': '14155776'}}
        if car: values.pop('board_upload.flash_size')
        return values.get(key, default)
''')
        (packages / "platform/factory.py").write_text(f'''
from pathlib import Path
class Board:
    def get(self, key, default=None): return '8MB' if key == 'upload.flash_size' else default
class Platform:
    def configure_project_packages(self, environment):
        self.configure_default_packages({{'board': 'selected-board'}}, [])
    def configure_default_packages(self, variables, targets):
        # Mirrors the installed platform hook: autoinstall=False does not skip it.
        Path({str(self.root / 'installer-called')!r}).write_text('installer/penv/framework mutation')
    def get_dir(self): return {str(self.core / 'platforms/selected-platform')!r}
    def get_package_dir(self, name): return str(Path({str(self.core / 'packages')!r}) / name)
    def board_config(self, board):
        assert board == 'selected-board'
        return Board()
class PlatformFactory:
    @classmethod
    def from_env(cls, environment, autoinstall):
        assert autoinstall is False
        assert environment in ('waveshare-349', 'esp32-s3-car-install')
        platform = Platform()
        platform.configure_project_packages(environment)
        return platform
''')
        (packages / "package/meta.py").write_text('''class PackageSpec:
    def __init__(self, value=None, *, name=None, requirements=None):
        # Core parses URI sources before inferring their name. PlatformBase
        # separates the declared name from @requirements before this API.
        self.name = value.rsplit('/', 1)[-1] if value and '://' in value else value.split('@', 1)[0].strip().split('/')[-1] if value else name
        self.requirements = value.split('@', 1)[1].strip() if value and '@' in value else requirements
''')
        (packages / "package/manager/platform.py").write_text(f'''
from pathlib import Path
from types import SimpleNamespace
class PlatformPackageManager:
    def get_package(self, spec):
        if spec == 'fixture/custom-platform@4.0.0':
            return SimpleNamespace(path={str(self.core / 'platforms/selected-platform')!r})
        return None
''')
        (packages / "platform/base.py").write_text(f'''
import json, os
from pathlib import Path
from types import SimpleNamespace
from platformio.package.meta import PackageSpec
from platformio.project.config import ProjectConfig
class PackageManager:
    def ensure_spec(self, item): return PackageSpec(item)
    def get_package(self, spec):
        assert spec.name == 'toolchain-xtensa-esp-elf'
        assert spec.requirements == '14.2.0+20260121'
        version = os.environ.get('CONTEXT_INSTALLED_VERSION', '14.2.0+20260121')
        if version == 'missing': return None
        return SimpleNamespace(path={str(self.core / 'packages/toolchain-xtensa-esp-elf')!r}, metadata=SimpleNamespace(version=version))
class Board:
    def get(self, key, default=None): return '8MB' if key == 'upload.flash_size' else default
class PlatformBase:
    def __init__(self, manifest):
        self.manifest = Path(manifest)
        assert self.manifest.is_file(), 'must select an installed manifest'
        self.packages = json.loads(self.manifest.read_text())['packages']
        self.pm = PackageManager()
        self.overrides = {{}}
    def configure_project_packages(self, environment):
        for item in ProjectConfig.get_instance().get('env:' + environment, 'platform_packages', []):
            name, _, version = item.partition('@')
            spec = self.pm.ensure_spec(name)
            self.overrides[spec.name] = version.strip() or '*'
        self.configure_default_packages({{'board': 'selected-board'}}, [])
    def configure_default_packages(self, variables, targets): pass
    def get_dir(self): return str(self.manifest.parent)
    def get_package_dir(self, name):
        if name in self.overrides:
            value = self.overrides[name]
            if value == 'missing' or (value.startswith('file://') and not Path(value[7:]).exists()): return None
            return value
        if name == 'toolchain-xtensa-esp-elf' and os.environ.get('CONTEXT_REMAP') == '1': return None
        return str(Path({str(self.core / 'packages')!r}) / name)
    def board_config(self, board):
        assert board == 'selected-board'
        return Board()
''')
        manifest = self.core / "platforms/selected-platform/platform.json"
        manifest.parent.mkdir(parents=True)
        manifest.write_text(json.dumps({"name": "custom-platform", "packages": {
            "toolchain-xtensa-esp-elf": {"version": "https://fixture.invalid/toolchain.zip", "package-version": "14.2.0+20260121"}
        }}))
        builder = self.core / "platforms/selected-platform/builder/main.py"
        builder.parent.mkdir(parents=True, exist_ok=True)
        builder.write_text('env["FS_BLOCK"] = int("0x1000", 16)')
        for memory, limit in (("qio_opi", 64), ("opi_opi", 32)):
            sdk = self.core / "packages/framework-arduinoespressif32-libs/esp32s3" / memory / "include/sdkconfig.h"
            sdk.parent.mkdir(parents=True)
            sdk.write_text(f"#define CONFIG_LITTLEFS_OBJ_NAME_LEN {limit}\n")

    @staticmethod
    def write_tool(path, source):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(source)
        path.chmod(0o755)

    def test_selected_interpreter_resolves_both_environments_and_custom_paths(self):
        for name, memory, limit, mode, partition in (
            ("waveshare-349", "qio_opi", 64, "dio", "default.csv"),
            ("esp32-s3-car-install", "opi_opi", 32, "dout", "car.csv"),
        ):
            with self.subTest(environment=name):
                result = context.resolve(self.root, name, str(self.pio))
                self.assertEqual(result.tools.python, str(self.python))
                self.assertEqual(result.tools.core_dir, str(self.core))
                self.assertEqual(result.build_dir, str(self.root / "selected-builds" / name))
                self.assertEqual(result.partitions, str(self.root / "geometry" / partition))
                self.assertEqual(result.memory_type, memory)
                self.assertEqual(result.filesystem_name_max(), limit)
                self.assertEqual(result.filesystem_block_size(), 4096)
                self.assertEqual(result.flash, context.FlashPolicy(mode, "80m", "16MB"))
                self.assertEqual(context.BuildContext.from_json(result.to_json()), result)
        calls = [json.loads(line) for line in self.log.read_text().splitlines()]
        self.assertEqual([call[0] for call in calls], ['pio', 'python', 'pio', 'python'])

    def test_context_resolution_does_not_run_platform_package_install_hooks(self):
        context.resolve(self.root, "waveshare-349", str(self.pio))
        self.assertFalse((self.root / "installer-called").exists(),
                         "reading context must not dispatch the installed platform's configure_default_packages hook")

    def test_direct_platform_directory_and_explicit_package_overrides(self):
        direct = self.root / "direct platform"
        shutil.copytree(self.core / "platforms/selected-platform", direct)
        overrides = ["framework-arduinoespressif32@" + str(self.root / "custom framework"),
                     "toolchain-xtensa-esp-elf@" + str(self.root / "custom toolchain")]
        with patch.dict(os.environ, CONTEXT_PLATFORM_SPEC=str(direct), CONTEXT_PACKAGES=json.dumps(overrides), CONTEXT_REMAP="1"):
            result = context.resolve(self.root, "waveshare-349", str(self.pio))
        self.assertEqual(result.platform_dir, str(direct))
        self.assertEqual(result.arduino_dir, str(self.root / "custom framework"))
        self.assertEqual(result.toolchain_dir, str(self.root / "custom toolchain"))
        self.assertFalse((self.root / "installer-called").exists())

    def test_default_toolchain_local_remap_requires_exact_installed_version_and_no_override(self):
        with patch.dict(os.environ, CONTEXT_REMAP="1"):
            result = context.resolve(self.root, "waveshare-349", str(self.pio))
            self.assertEqual(result.toolchain_dir, str(self.core / "packages/toolchain-xtensa-esp-elf"))
            for version in ("14.1.0", "missing"):
                with self.subTest(version=version), patch.dict(os.environ, CONTEXT_INSTALLED_VERSION=version):
                    with self.assertRaisesRegex(RuntimeError, "selected platform package is unavailable"):
                        context.resolve(self.root, "waveshare-349", str(self.pio))
            with patch.dict(os.environ, CONTEXT_PACKAGES=json.dumps(["toolchain-xtensa-esp-elf@missing"])):
                with self.assertRaisesRegex(RuntimeError, "selected platform package is unavailable"):
                    context.resolve(self.root, "waveshare-349", str(self.pio))
        self.assertFalse((self.root / "installer-called").exists())

    def test_missing_selected_platform_does_not_fallback_to_another_installation(self):
        with patch.dict(os.environ, CONTEXT_PLATFORM_SPEC="fixture/missing@1.0.0"):
            with self.assertRaisesRegex(RuntimeError, "selected platform is not installed"):
                context.resolve(self.root, "waveshare-349", str(self.pio))
        self.assertFalse((self.root / "installer-called").exists())

    def test_missing_explicit_toolchain_uri_never_uses_default_remap(self):
        override = 'toolchain-xtensa-esp-elf @ file://' + str(self.root / 'missing toolchain')
        with patch.dict(os.environ, CONTEXT_REMAP='1', CONTEXT_PACKAGES=json.dumps([override])):
            with self.assertRaisesRegex(RuntimeError, 'selected platform package is unavailable'):
                context.resolve(self.root, 'waveshare-349', str(self.pio))
        self.assertFalse((self.root / 'installer-called').exists())

    def test_flash_policy_needs_only_selected_core_configuration_before_platform_install(self):
        shutil.rmtree(self.root / "fake-python/platformio/platform")
        shutil.rmtree(self.core)
        result = context.resolve_flash_policy(self.root, "esp32-s3-car-install", str(self.pio))
        self.assertEqual(result, context.FlashPolicy("dout", "80m", "16MB"))

    def test_failed_explicit_tools_never_try_another_interpreter(self):
        with self.assertRaises(FileNotFoundError):
            context.resolve(self.root, "waveshare-349", str(self.root / "missing-pio"))
        self.assertFalse(self.log.exists())
        with patch.dict(os.environ, {"CONTEXT_FAIL_PIO": "1"}):
            with self.assertRaisesRegex(RuntimeError, '41'):
                context.resolve(self.root, "waveshare-349", str(self.pio))
        self.assertEqual(len(self.log.read_text().splitlines()), 1)
        self.log.unlink()
        with patch.dict(os.environ, {"CONTEXT_FAIL_PYTHON": "1"}):
            with self.assertRaisesRegex(RuntimeError, '42'):
                context.resolve(self.root, "waveshare-349", str(self.pio))
        self.assertEqual([json.loads(line)[0] for line in self.log.read_text().splitlines()], ['pio', 'python'])

    def test_scons_uses_resolved_board_and_platform_without_tool_discovery(self):
        selected = context.resolve(self.root, "waveshare-349", str(self.pio))
        values = {"build.partitions": "geometry/default.csv", "build.arduino.memory_type": "qio_opi",
                  "build.flash_mode": "qio", "build.f_flash": "80000000L", "build.flash_size": "16MB",
                  "upload.maximum_size": "14155776"}
        board = types.SimpleNamespace(get=lambda key, default=None: "8MB" if key == "upload.flash_size" else values.get(key, default))
        platform = types.SimpleNamespace(get_dir=lambda: selected.platform_dir,
            get_package_dir=lambda name: str(self.core / "packages" / name))
        substitutions = {"$PROJECT_DIR": str(self.root), "$BUILD_DIR": str(self.root / "resolved-scons-output"),
                         "$PYTHONEXE": str(self.python), "$PROJECT_CORE_DIR": str(self.core)}
        class Env(dict):
            def subst(self, key): return substitutions[key]
            def BoardConfig(self): return board
            def PioPlatform(self): return platform
            def GetProjectOption(self, key, default=None): return values.get(key.removeprefix("board_"), default)
        with patch.object(context, 'selected_tools', side_effect=AssertionError('must use resolved SCons inputs')):
            actual = context.from_scons(Env(PIOENV="waveshare-349"))
        self.assertEqual(actual.flash, context.FlashPolicy("dio", "80m", "16MB"))
        self.assertEqual(actual.sdkconfig, selected.sdkconfig)
        self.assertEqual(actual.build_dir, str(self.root / "resolved-scons-output"))

    def test_memory_main_builds_before_resolving_newly_prepared_packages(self):
        calls = []
        build_dir = self.root / "produced output"
        def build(command):
            self.assertEqual(command, ["selected-pio", "run", "-e", "esp32-s3-car-install"])
            calls.append("build")
            return ""
        def resolve(*_):
            calls.append("resolve")
            if calls != ["build", "resolve"]:
                raise RuntimeError("selected packages are not installed yet")
            return types.SimpleNamespace(build_dir=str(build_dir), maximum_size="4096")
        pool = {"used_bytes": 32, "limit_bytes": 64, "headroom_bytes": 32}
        with patch.object(memory, "run", side_effect=build), patch.object(memory, "resolve", side_effect=resolve), \
                patch.object(memory, "parse_memory_configuration", return_value={"dram0_0_seg": 1024}), \
                patch.object(memory, "parse_size_sections", return_value={".dram0.data": 32, ".flash.text": 64}), \
                patch.object(memory, "load_esp_idf_size_memory", return_value={"iram": pool, "diram": pool}):
            result = memory.main(["--pio", "selected-pio", "--env", "esp32-s3-car-install", "--report-dir", str(self.root / "reports")])
        self.assertEqual(result, 0)
        self.assertEqual(calls, ["build", "resolve"])
        report = json.loads((self.root / "reports/esp32-s3-car-install.json").read_text())
        self.assertEqual(report["memory"]["flash"], {"used_bytes": 64, "limit_bytes": 4096, "headroom_bytes": 4032})

    def test_memory_main_no_build_and_failed_build_do_not_trigger_extra_work(self):
        for args in (["--no-build"], ["--build-log", str(self.root / "missing.log")], []):
            with self.subTest(args=args), patch.object(memory, "run", side_effect=RuntimeError("build failed")) as build, \
                    patch.object(memory, "resolve", side_effect=RuntimeError("packages absent")) as resolve:
                self.assertEqual(memory.main(args), 1)
                self.assertEqual(build.call_count, 1 if not args else 0)
                self.assertEqual(resolve.call_count, 1 if args == ["--no-build"] else 0)

    def test_memory_tools_stay_in_selected_context_after_failure(self):
        selected = context.resolve(self.root, "waveshare-349", str(self.pio))
        with self.assertRaises(FileNotFoundError):
            memory.find_xtensa_size(selected)
        executable = Path(selected.toolchain_dir) / "bin/xtensa-esp32s3-elf-size"
        self.write_tool(executable, '#!/bin/sh\nprintf ".flash.text 321 0\\n"\n')
        self.assertEqual(memory.parse_size_sections(self.root / "firmware.elf", selected), {".flash.text": 321})
        self.log.unlink()
        with patch.dict(os.environ, {"CONTEXT_FAIL_PYTHON": "1"}):
            with self.assertRaisesRegex(RuntimeError, '42'):
                memory.load_esp_idf_size_memory(self.root / "firmware.map", selected)
        calls = [json.loads(line) for line in self.log.read_text().splitlines()]
        self.assertEqual(len(calls), 1)
        self.assertEqual(calls[0][1:3], ['-m', 'esp_idf_size'])

    def test_explicit_image_policy_checks_real_header_bytes_without_platformio(self):
        image = self.root / "merged-firmware.bin"
        data = bytearray(b'\xff' * (0x20000 + 4))
        for offset in (0, 0x20000):
            data[offset:offset + 4] = bytes((0xE9, 1, 0x02, 0x4F))
        image.write_bytes(data)
        expected = {"Flash mode": "DIO", "Flash freq": "80m", "Flash size": "16MB"}
        self.assertEqual(image_info.check_embedded_image_headers(image, expected), [])
        data[0x20002] = 0x00
        image.write_bytes(data)
        self.assertTrue(any('app image header' in error for error in image_info.check_embedded_image_headers(image, expected)))
        data[0x20002] = 0x02
        data[3] = 0x3F
        image.write_bytes(data)
        self.assertTrue(any('bootloader image header' in error for error in image_info.check_embedded_image_headers(image, expected)))

    def test_production_builder_propagates_selected_environment_and_paths(self):
        shutil.copyfile(ROOT / "scripts/build_production_artifacts.sh", self.root / "scripts/build_production_artifacts.sh")
        (self.root / "scripts/platformio_ca_bundle.sh").write_text(":\n")
        (self.root / "interface/node_modules").mkdir(parents=True)
        pio_source = self.pio.read_text().replace(
            "if sys.argv[1:] != ['system', 'info', '--json-output']: raise SystemExit('unexpected PIO effect')",
            "if sys.argv[1:2] in (['run'], ['pkg']): raise SystemExit(0)\nif sys.argv[1:] != ['system', 'info', '--json-output']: raise SystemExit('unexpected PIO effect')")
        self.pio.write_text(pio_source)
        self.write_tool(self.root / "bin/python3", f'''#!{sys.executable}
import json, pathlib, subprocess, sys
with pathlib.Path({str(self.log)!r}).open('a') as log:
    log.write(json.dumps(['host-python', *sys.argv[1:]]) + '\\n')
if sys.argv[1].endswith('build_context.py'):
    raise SystemExit(subprocess.call([{sys.executable!r}, *sys.argv[1:]]))
allowed = {{'check_platformio_core_version.py', 'check_audio_asset_manifest.py', 'generate_web_bundle.py',
           'check_memory_headroom.py', 'report_flash_package_size.py', 'check_littlefs_image_compatibility.py'}}
assert pathlib.Path(sys.argv[1]).name in allowed
''')
        self.write_tool(self.root / "bin/npm", f'''#!{sys.executable}
import json, pathlib, sys
with pathlib.Path({str(self.log)!r}).open('a') as log:
    log.write(json.dumps(['npm', *sys.argv[1:]]) + '\\n')
assert sys.argv[1:] in (['run', 'build'], ['run', 'deploy:built'])
''')
        env = dict(os.environ, PATH=str(self.root / "bin") + os.pathsep + os.environ['PATH'],
                   PIO_CMD=str(self.pio), PIO_ENV='esp32-s3-car-install', PLATFORMIO_RUN_JOBS='2')
        result = subprocess.run(['bash', str(self.root / 'scripts/build_production_artifacts.sh')],
                                env=env, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        calls = [json.loads(line) for line in self.log.read_text().splitlines()]
        pio_commands = [call[1:] for call in calls if call[0] == 'pio' and call[1] in ('run', 'pkg')]
        self.assertEqual(pio_commands, [['pkg', 'install', '-e', 'esp32-s3-car-install'],
            ['run', '-e', 'esp32-s3-car-install', '-j', '2', '-t', 'clean'],
            ['run', '-e', 'esp32-s3-car-install', '-j', '2'],
            ['run', '-e', 'esp32-s3-car-install', '-j', '2', '-t', 'buildfs']])
        report = next(call for call in calls if call[0] == 'host-python' and call[1].endswith('report_flash_package_size.py'))
        self.assertEqual(report[report.index('--build-dir') + 1], str(self.root / 'selected-builds/esp32-s3-car-install'))
        self.assertEqual(report[report.index('--partition-table') + 1], str(self.root / 'geometry/car.csv'))
        self.assertEqual(report[report.index('--max-firmware-bytes') + 1], '5570560')
        self.assertEqual(report[report.index('--expect-littlefs-bytes') + 1], '2424832')
        compatibility = next(call for call in calls if call[0] == 'host-python' and call[1].endswith('check_littlefs_image_compatibility.py'))
        self.assertEqual(compatibility[compatibility.index('--pio') + 1], str(self.pio))
        self.assertEqual(compatibility[compatibility.index('--env') + 1], 'esp32-s3-car-install')
        self.assertEqual(compatibility[compatibility.index('--candidate') + 1], str(self.root / 'selected-builds/esp32-s3-car-install/littlefs.bin'))

    def test_malformed_tool_info_is_not_replaced_with_host_defaults(self):
        for output in ('[]', '{}', '{"python_exe":null}', '{"python_exe":{"value":""}}'):
            with patch.object(context, '_output', return_value=output):
                with self.assertRaises(ValueError):
                    context.selected_tools(self.root, str(self.pio))


if __name__ == "__main__":
    unittest.main(verbosity=2)

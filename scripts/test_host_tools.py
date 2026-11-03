#!/usr/bin/env python3
"""Exercise bootstrap reads and real consumers with isolated tool/OS fixtures."""

from __future__ import annotations

import ast
import copy
import json
import os
from pathlib import Path
import shutil
import shlex
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import check_linux_validation_environment as environment
import host_tools
from test_linux_validation_environment import valid_snapshot

ROOT = Path(__file__).resolve().parents[1]


class HostToolsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="host tools ")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        for name in ("config/host_tools.json", "scripts/host_tools.py", "scripts/bootstrap_linux_validation.sh",
                     "scripts/check_platformio_core_version.py", "scripts/check_esptool_version.py",
                     "scripts/platformio_ca_bundle.sh", "scripts/run_clean_linux_ci.sh"):
            target = self.root / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / name, target)
        self.manifest = self.root / "config/host_tools.json"
        self.data = json.loads(self.manifest.read_text())

    def save(self) -> None:
        self.manifest.write_text(json.dumps(self.data))

    def cli(self, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run([sys.executable, "-I", "-S", str(self.root / "scripts/host_tools.py"), *args],
                              env={"PATH": ""}, capture_output=True, text=True)

    def test_base_python_reads_before_any_qualified_tools_exist(self) -> None:
        # Python 3.8 syntax and no site packages; PATH contains no Node/PIO/tools.
        ast.parse((self.root / "scripts/host_tools.py").read_text(), feature_version=(3, 8))
        result = self.cli("workflow", "linux")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "python=3.12\nnode=22.23.3\n")
        self.assertEqual(self.cli("workflow", "macos").stdout,
                         "macos_developer_dir=/Applications/Xcode_16.4.app/Contents/Developer\n")
        self.assertEqual(self.cli("packages", "release").stdout.splitlines(),
                         ["pioarduino==6.2.0", "esptool==5.4.0", "Pillow>=10.4,<13", "littlefs-python==0.18.0"])
        self.assertEqual(self.cli("packages", "ci").stdout.splitlines(),
                         ["pioarduino==6.2.0", "esptool==5.4.0", "Pillow>=10.4,<13", "littlefs-python==0.18.0",
                          "ruff==0.16.0", "cryptography>=41", "numpy>=2,<3"])
        self.assertEqual(self.cli("requires", "release", "swift").returncode, 1)
        self.assertEqual(self.cli("requires", "ci", "swift").returncode, 0)

    def test_malformed_or_missing_manifest_fails_before_output(self) -> None:
        original = copy.deepcopy(self.data)
        changes = [lambda d: d.update(schema=True), lambda d: d.update(python=312),
                   lambda d: d["node"].update(version="22.23.3\ninjected=value"),
                   lambda d: d["swift"].update(ubuntu_archive_sha256="wrong"),
                   lambda d: d["profiles"]["ci"].append("unrecognized"),
                   lambda d: d.update(extra=True)]
        for change in changes:
            self.data = copy.deepcopy(original)
            change(self.data)
            self.save()
            result = self.cli("workflow", "linux")
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(result.stdout, "")
        self.manifest.write_text('{"schema":1,"schema":1}')
        self.assertIn("duplicate", self.cli("get", "python").stderr)
        self.manifest.unlink()
        self.assertNotEqual(self.cli("get", "python").returncode, 0)

    def test_changed_declaration_reaches_setup_version_probe_and_diagnostic(self) -> None:
        self.data["node"]["version"] = "22.24.7"
        self.data["python_packages"]["pioarduino"] = "==6.3.1"
        self.save()
        self.assertIn("node=22.24.7", self.cli("workflow", "linux").stdout)
        self.assertIn("pioarduino==6.3.1", self.cli("packages", "release").stdout)
        self.assertEqual(self.cli("get", "pio_venv_name").stdout, "pio-core-6.3.1\n")
        observed = self.root / "observed-pio"
        observed.write_text('#!/bin/sh\nprintf "PlatformIO Core, version 6.2.0\\n"\n')
        observed.chmod(0o755)
        result = subprocess.run([sys.executable, str(self.root / "scripts/check_platformio_core_version.py"),
                                 "--pio", str(observed)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 1)
        self.assertIn("required release pin 6.3.1", result.stderr)
        self.assertIn("pip install 'pioarduino==6.3.1'", result.stderr)
        self.assertIn(".artifacts/pio-core-6.3.1/bin/pio", result.stderr)
        errors = environment.validate_snapshot(valid_snapshot(), host_tools.load(self.manifest))
        self.assertIn("node_version is 22.23.3; required 22.24.7", errors)
        self.assertIn("pioarduino_version is 6.2.0; required 6.3.1", errors)

    def test_changed_npm_requirement_rejects_independent_executable_probe(self) -> None:
        binary = self.root / "bin"
        binary.mkdir()
        npm = binary / "npm"
        npm.write_text('#!/bin/sh\nprintf "10.9.9\\n"\n')
        npm.chmod(0o755)
        self.data["node"]["npm"] = "10.9.10"
        self.save()
        with patch.dict(os.environ, {"PATH": str(binary)}):
            observed = environment.command_output(["npm", "--version"])
        self.assertEqual(observed, "10.9.9")
        errors = environment.validate_snapshot(valid_snapshot(npm_version=observed), host_tools.load(self.manifest))
        self.assertIn("npm_version is 10.9.9; required 10.9.10", errors)

    def test_profile_membership_preserves_smaller_release_requirements(self) -> None:
        tools = host_tools.load(self.manifest)
        self.assertFalse(environment.validate_snapshot(valid_snapshot("release", ruff_version="", swift_version="",
            shellcheck_version="", ffmpeg_version="", libpcre3_available=False), tools))
        self.data["profiles"]["release"].append("ruff")
        self.save()
        changed = host_tools.load(self.manifest)
        self.assertIn("ruff==0.16.0", changed.packages("release"))
        self.assertIn("ruff_version is missing; required 0.16.0",
                      environment.validate_snapshot(valid_snapshot("release", ruff_version=""), changed))

    def bootstrap(self, profile: str, observed_python: str = "3.12", observed_node: str | None = None, checksum_fails: bool = False) -> tuple[subprocess.CompletedProcess[str], list[list[str]]]:
        # Substitute only the host OS file and executable effects. The complete
        # production bootstrap body and declaration reader execute unchanged.
        release_file = self.root / "os-release"
        release_file.write_text('ID=ubuntu\nVERSION_ID=24.04\n')
        bootstrap = self.root / "scripts/bootstrap_linux_validation.sh"
        bootstrap.write_text(bootstrap.read_text().replace("/etc/os-release", shlex.quote(str(release_file))))
        (self.root / "interface").mkdir(exist_ok=True)
        binary = self.root / "bin"
        binary.mkdir(exist_ok=True)
        log = self.root / "calls.jsonl"
        fake = f'''#!{sys.executable}
import json, os, pathlib, shutil, subprocess, sys
name = pathlib.Path(sys.argv[0]).name
args = sys.argv[1:]
with pathlib.Path({str(log)!r}).open('a') as output:
    output.write(json.dumps([name, *args]) + '\\n')
if name in ('python', 'python3'):
    if args[0].endswith('host_tools.py'):
        raise SystemExit(subprocess.call([{sys.executable!r}, '-I', '-S', *args]))
    if args[0] == '-c': print({observed_python!r})
    elif args[:2] == ['-m', 'venv']:
        target = pathlib.Path(args[-1]) / 'bin/python'
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(sys.argv[0], target)
        target.chmod(0o755)
    elif args[:2] == ['-m', 'pip']: pass
    elif args[0].endswith('check_linux_validation_environment.py'): pass
    else: raise SystemExit('unexpected Python effect')
elif name == 'uname': print('Linux')
elif name == 'node': print('v' + {(observed_node or self.data['node']['version'])!r})
elif name == 'swiftc': print('Swift version 0.0.0')
elif name == 'sudo': raise SystemExit(subprocess.call(args))
elif name == 'sha256sum':
    with pathlib.Path({str(log)!r}).open('a') as output:
        output.write(json.dumps(['digest', sys.stdin.read().split()[0]]) + '\\n')
    raise SystemExit(1 if {checksum_fails!r} else 0)
'''
        for name in ("python3", "uname", "node", "swiftc", "apt-get", "curl", "sha256sum", "tar", "install", "sudo", "npm"):
            path = binary / name
            path.write_text(fake)
            path.chmod(0o755)
        env = dict(os.environ, PATH=str(binary) + ":/usr/bin:/bin", PYTHON_CMD=str(binary / "python3"),
                   V1_PIO_VENV=str(self.root / "pio-env"), V1_SWIFT_ROOT=str(self.root / "swift-env"))
        result = subprocess.run(["/bin/bash", str(bootstrap), profile], env=env, capture_output=True, text=True)
        calls = [json.loads(line) for line in log.read_text().splitlines()] if log.exists() else []
        return result, calls

    def test_complete_ci_bootstrap_consumes_declared_versions_and_hashes(self) -> None:
        self.data["node"]["version"] = "22.24.7"
        self.data["python_packages"]["pioarduino"] = "==6.3.1"
        self.data["swift"]["version"] = "6.3.4"
        self.save()
        result, calls = self.bootstrap("ci")
        self.assertEqual(result.returncode, 0, result.stderr)
        pip = [call for call in calls if call[1:4] == ["-m", "pip", "install"]]
        self.assertEqual(len(pip), 1)
        self.assertEqual(pip[0][4:], ["pioarduino==6.3.1", "esptool==5.4.0", "Pillow>=10.4,<13",
                                     "littlefs-python==0.18.0", "ruff==0.16.0", "cryptography>=41", "numpy>=2,<3"])
        self.assertTrue(any(call[0] == "curl" and any("swift-6.3.4-release" in arg for arg in call) for call in calls))
        self.assertIn(["digest", "da8272a5fddccd65b1529ed0e52e04526e2eadd4237d58d6220efeb973c6cd19"], calls)
        self.assertTrue(any(call[0] == "apt-get" and "libpcre3" in call and "ffmpeg" in call for call in calls))
        self.assertTrue(any("check_linux_validation_environment.py" in call[1] and call[2] == "ci"
                            for call in calls if len(call) > 2))

    def test_wrong_observed_python_or_node_fails_before_install(self) -> None:
        for python_version, node_version, diagnostic in (("3.11", "22.23.3", "Python 3.12"),
                                                         ("3.12", "22.24.0", "Node.js 22.23.3")):
            with self.subTest(diagnostic=diagnostic):
                result, calls = self.bootstrap("ci", python_version, node_version)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(diagnostic, result.stderr)
                self.assertFalse(any(call[0] in {"apt-get", "curl", "npm"} or "pip" in call for call in calls))

    def test_download_checksum_failure_stops_before_extract_and_install(self) -> None:
        result, calls = self.bootstrap("ci", checksum_fails=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue(any(call[0] == "sha256sum" for call in calls))
        self.assertFalse(any(call[0] in {"tar", "install", "npm"} or "pip" in call for call in calls))

    def test_bad_manifest_stops_bootstrap_before_tool_effects(self) -> None:
        self.data["schema"] = True
        self.save()
        result, calls = self.bootstrap("release")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unsupported host-tool schema", result.stderr)
        self.assertEqual(len(calls), 1)
        self.assertEqual(calls[0][0], "python3")

    def test_clean_runner_passes_declared_image_node_and_hash_to_engine(self) -> None:
        self.data["node"]["version"] = "22.24.7"
        self.data["node"]["linux_x64_sha256"] = "a" * 64
        self.data["linux"]["image"] = "docker.io/library/ubuntu@sha256:" + "b" * 64
        self.save()
        binary = self.root / "bin"
        binary.mkdir()
        log = self.root / "engine.json"
        fake = f'''#!{sys.executable}
import json, pathlib, subprocess, sys
name = pathlib.Path(sys.argv[0]).name
args = sys.argv[1:]
if name == 'python3' and args[0].endswith('host_tools.py'):
    raise SystemExit(subprocess.call([{sys.executable!r}, '-I', '-S', *args]))
if name == 'git':
    print('a' * 40 if args[-1] == 'HEAD' else 'https://example.invalid/repo')
if name == 'docker' and args[0] == 'run':
    pathlib.Path({str(log)!r}).write_text(json.dumps(args))
'''
        for name in ("python3", "git", "docker"):
            path = binary / name
            path.write_text(fake)
            path.chmod(0o755)
        result = subprocess.run(["/bin/bash", str(self.root / "scripts/run_clean_linux_ci.sh"),
                                 "--engine", "docker", "--preflight-only"],
                                env=dict(os.environ, PATH=str(binary) + ":/usr/bin:/bin"),
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        args = json.loads(log.read_text())
        self.assertIn("V1_NODE_VERSION=22.24.7", args)
        self.assertIn("V1_NODE_SHA256=" + "a" * 64, args)
        self.assertIn("docker.io/library/ubuntu@sha256:" + "b" * 64, args)
        self.assertIn("RUN_FULL_GATE=false", args)
        self.assertIn('sha256sum --check --strict', args[-1])
        self.assertIn('python3 scripts/host_tools.py get swift', args[-1])

    def test_complete_release_bootstrap_omits_ci_tools(self) -> None:
        result, calls = self.bootstrap("release")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(any(call[0] in {"apt-get", "curl", "swiftc", "install"} for call in calls))
        pip = next(call for call in calls if call[1:4] == ["-m", "pip", "install"])
        self.assertEqual(pip[4:], ["pioarduino==6.2.0", "esptool==5.4.0", "Pillow>=10.4,<13", "littlefs-python==0.18.0"])
        self.assertIn(["npm", "ci"], calls)

    def test_ca_discovery_reads_optional_fallback_with_only_base_python(self) -> None:
        binary = self.root / "bin"
        binary.mkdir()
        certificate = self.root / "fixture-ca.pem"
        certificate.write_text("fixture certificate")
        wrapper = binary / "python3"
        wrapper.write_text(f'''#!{sys.executable}
import subprocess, sys
if sys.argv[1:] == ['-']:
    sys.stdin.read()
    print({str(certificate)!r})
else:
    raise SystemExit(subprocess.call([{sys.executable!r}, '-I', '-S', *sys.argv[1:]]))
''')
        wrapper.chmod(0o755)
        env = dict(os.environ, PATH=str(binary) + ":/usr/bin:/bin", PIO_CMD="missing-pio")
        for key in ("REQUESTS_CA_BUNDLE", "SSL_CERT_FILE", "PLATFORMIO_SKIP_CA_BOOTSTRAP"):
            env.pop(key, None)
        command = 'source "$1"; printf "%s\\n" "$REQUESTS_CA_BUNDLE"'
        result = subprocess.run(["/bin/bash", "-e", "-c", command, "fixture",
                                  str(self.root / "scripts/platformio_ca_bundle.sh")],
                                 env=env, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), str(certificate))
        self.assertFalse((self.root / ".artifacts/pio-core-6.2.0").exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)

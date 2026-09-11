#!/usr/bin/env python3
"""Exercise device-run output ownership without opening a physical device."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import textwrap
import unittest


ROOT = Path(__file__).resolve().parents[1]


class DeviceRunOutputTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        for relative in (
            "scripts/run_device_tests.sh",
            "scripts/platformio_ca_bundle.sh",
            "tools/extract_device_metrics.py",
        ):
            destination = self.root / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / relative, destination)
        self.port = self.root / "port-fixture"
        self.port.touch()
        self.calls = self.root / "pio-calls"
        self.pio = self.root / "pio-fixture"
        self.pio.write_text(textwrap.dedent("""\
            #!/usr/bin/env python3
            import json, os, sys
            from pathlib import Path
            with Path(os.environ['PIO_CALLS']).open('a') as handle:
                handle.write('test\\n')
            if os.environ.get('PIO_FAIL_BEFORE_REPORT') == '1':
                print('Fixture failure before report creation', file=sys.stderr)
                sys.exit(1)
            args = sys.argv[1:]
            name = args[args.index('-f') + 1]
            if os.environ.get('PIO_ZERO_TEST_SUITE') == name:
                report = {'test_suites': [{
                    'env_name': 'device', 'test_name': name, 'status': 'SKIPPED',
                    'testcase_nums': 0, 'failure_nums': 0, 'error_nums': 0,
                    'skipped_nums': 0, 'duration': 0.0, 'test_cases': [],
                }]}
                Path(args[args.index('--json-output-path') + 1]).write_text(json.dumps(report))
                Path(args[args.index('--junit-output-path') + 1]).write_text('<testsuites/>')
                sys.exit(0)
            report = {'test_suites': [{
                'env_name': 'device', 'test_name': name, 'status': 'PASSED',
                'testcase_nums': 1, 'failure_nums': 0, 'error_nums': 0,
                'skipped_nums': 0, 'duration': 0.01,
                'test_cases': [{'name': 'current_assertion', 'status': 'PASSED'}],
            }]}
            Path(args[args.index('--json-output-path') + 1]).write_text(json.dumps(report))
            Path(args[args.index('--junit-output-path') + 1]).write_text('<testsuites/>')
        """))
        self.pio.chmod(0o755)

    def run_device(self, out, fail=False, zero_test_suite=""):
        environment = os.environ.copy()
        environment.update(
            PIO_CMD=str(self.pio), PIO_CALLS=str(self.calls),
            PIO_FAIL_BEFORE_REPORT="1" if fail else "0",
            PIO_ZERO_TEST_SUITE=zero_test_suite,
            DEVICE_PORT=str(self.port), DEVICE_GIT_SHA="test-revision",
            PLATFORMIO_SKIP_CA_BOOTSTRAP="1", DEVICE_FAIL_CLOSED_TRANSPORT="0",
        )
        return subprocess.run(
            ["bash", str(self.root / "scripts/run_device_tests.sh"),
             "--quick", "--cooldown-seconds", "0", "--out-dir", str(out)],
            env=environment, capture_output=True, text=True, timeout=20,
        )

    def test_new_and_empty_output_directories_accept_current_reports(self):
        for precreate in (False, True):
            with self.subTest(precreate=precreate):
                out = self.root / f"output-{precreate}"
                if precreate:
                    out.mkdir()
                result = self.run_device(out)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                manifest = json.loads((out / "manifest.json").read_text())
                self.assertEqual(manifest["result"], "PASS")
                self.assertEqual(len(manifest["suite_results"]), 2)

    def test_reused_passing_output_is_refused_and_preserved_before_running(self):
        out = self.root / "output"
        first = self.run_device(out)
        self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
        before = {p.name: p.read_bytes() for p in out.iterdir()}
        calls_before = self.calls.read_bytes()
        second = self.run_device(out, fail=True)
        self.assertNotEqual(second.returncode, 0)
        self.assertIn("must be empty", second.stderr)
        self.assertEqual(before, {p.name: p.read_bytes() for p in out.iterdir()})
        self.assertEqual(calls_before, self.calls.read_bytes())

    def test_hidden_existing_evidence_is_also_preserved(self):
        out = self.root / "output"
        out.mkdir()
        evidence = out / ".retained"
        evidence.write_bytes(b"original evidence")
        result = self.run_device(out)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("must be empty", result.stderr)
        self.assertEqual(list(out.iterdir()), [evidence])
        self.assertEqual(evidence.read_bytes(), b"original evidence")
        self.assertFalse(self.calls.exists())

    def test_fresh_early_failure_remains_failed(self):
        out = self.root / "output"
        result = self.run_device(out, fail=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(json.loads((out / "manifest.json").read_text())["result"], "FAIL")
        self.assertFalse((out / "test_device_boot.json").exists())

    def test_selected_suite_with_zero_executed_tests_fails_closed(self):
        out = self.root / "output"
        result = self.run_device(out, zero_test_suite="test_device_heap")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("did not execute any tests", result.stdout)
        self.assertIn("produced no executed test evidence", result.stderr)
        manifest = json.loads((out / "manifest.json").read_text())
        self.assertEqual(manifest["result"], "FAIL")
        self.assertEqual(
            [(row["suite"], row["status"]) for row in manifest["suite_results"]],
            [("test_device_boot", "PASS"), ("test_device_heap", "FAIL")],
        )


if __name__ == "__main__":
    unittest.main()

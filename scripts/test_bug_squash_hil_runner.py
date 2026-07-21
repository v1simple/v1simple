#!/usr/bin/env python3
"""Regression tests for the fail-closed bug-squash HIL runner."""

from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import tempfile
from unittest import mock

import run_bug_squash_hil as hil_runner


ROOT = Path(__file__).resolve().parents[1]
RUNNER = ROOT / "scripts" / "run_bug_squash_hil.py"
EXPECTED_SUITES = (
    "test_device_boot",
    "test_device_heap",
    "test_device_psram",
    "test_device_freertos",
    "test_device_event_bus",
    "test_device_nvs",
    "test_device_battery",
    "test_device_coexistence",
)


def assert_true(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def write_executable(path: Path, content: str) -> None:
    path.write_text(content, encoding="utf-8")
    path.chmod(0o755)


def initialize_repository(path: Path) -> str:
    subprocess.run(["git", "init", "-q"], cwd=path, check=True)
    (path / "README.md").write_text("runner fixture\n", encoding="utf-8")
    subprocess.run(["git", "add", "README.md"], cwd=path, check=True)
    subprocess.run(
        [
            "git",
            "-c",
            "user.name=Runner Test",
            "-c",
            "user.email=runner@example.invalid",
            "commit",
            "-q",
            "-m",
            "fixture",
        ],
        cwd=path,
        check=True,
    )
    return subprocess.run(
        ["git", "rev-parse", "HEAD"],
        cwd=path,
        capture_output=True,
        text=True,
        check=True,
    ).stdout.strip()


def prepare_fixture(root: Path) -> dict[str, Path | str]:
    repository = root / "repository"
    repository.mkdir()
    target_sha = initialize_repository(repository)
    fake_port = root / "tty-secret-fixture"
    fake_port.write_text("", encoding="utf-8")
    template = root / "template.json"
    inventory = root / "inventory.json"
    ports = root / "ports.json"
    template.write_text(
        json.dumps({"schema_version": 1, "description": "test", "boards": []}),
        encoding="utf-8",
    )
    inventory.write_text(
        json.dumps(
            {
                "schema_version": 1,
                "boards": [
                    {
                        "alias": "release",
                        "capabilities": ["device-tests", "serial"],
                        "usb_serial": "SECRET-USB-IDENTITY",
                    }
                ],
            }
        ),
        encoding="utf-8",
    )
    ports.write_text(
        json.dumps(
            [
                {
                    "port": str(fake_port),
                    "serial_number": "SECRET-USB-IDENTITY",
                }
            ]
        ),
        encoding="utf-8",
    )

    device_runner = root / "fake-device-runner.py"
    write_executable(
        device_runner,
        """#!/usr/bin/env python3
import json
import os
from pathlib import Path
import sys

out_dir = Path(sys.argv[sys.argv.index('--out-dir') + 1])
out_dir.mkdir(parents=True, exist_ok=True)
suites = %r
status = os.environ.get('FAKE_SUITE_STATUS', 'PASS')
result = os.environ.get('FAKE_MANIFEST_RESULT', 'NO_BASELINE')
run_id = 'fixture-device-run'
suite_rows = [{
    'suite': suite,
    'status': status,
    'json': str(out_dir / f'{suite}.json'),
    'xml': str(out_dir / f'{suite}.xml'),
    'log': str(out_dir / f'{suite}.log'),
    'metric_count': '1',
} for suite in suites]
if os.environ.get('FAKE_DUPLICATE_SUITE') == '1':
    suite_rows[-1] = dict(suite_rows[0])
if os.environ.get('FAKE_MALFORMED_SUITE') == '1':
    suite_rows[-1]['suite'] = ['not', 'a', 'string']
payload = {
    'schema_version': 1,
    'run_id': run_id,
    'git_sha': os.environ['DEVICE_GIT_SHA'],
    'board_id': os.environ['DEVICE_BOARD_ID'],
    'result': result,
    'base_result': 'PASS',
    'metrics_file': 'metrics.ndjson',
    'scoring_file': 'scoring.json',
    'tracks': list(suites),
    'suite_results': suite_rows,
}
manifest_text = json.dumps(payload)
if os.environ.get('FAKE_DUPLICATE_MANIFEST_KEY') == '1':
    manifest_text = manifest_text.replace('"result":', '"result": "PASS", "result":', 1)
(out_dir / 'manifest.json').write_text(manifest_text + '\\n', encoding='utf-8')
for suite in suites:
    suite_payload = {
        'test_suites': [{
            'env_name': 'device',
            'test_suite_name': 'wrong-suite' if os.environ.get('FAKE_WRONG_SUITE_ID') == '1' else suite,
            'status': 'PASSED',
            'testcase_nums': 1,
            'pass_nums': 1,
            'failure_nums': 0,
            'error_nums': 0,
        }]
    }
    (out_dir / f'{suite}.json').write_text(json.dumps(suite_payload) + '\\n', encoding='utf-8')
    (out_dir / f'{suite}.xml').write_text(
        f'<testsuites><testsuite name="device:{suite}" tests="1" failures="0" errors="0"/></testsuites>\\n',
        encoding='utf-8',
    )
    (out_dir / f'{suite}.log').write_text('fixture\\n', encoding='utf-8')
for name in ('device.log', 'summary.md'):
    (out_dir / name).write_text('fixture\\n', encoding='utf-8')
metrics = [{
    'schema_version': 1,
    'run_id': run_id,
    'git_sha': os.environ['DEVICE_GIT_SHA'],
    'run_kind': 'device_suite',
    'suite_or_profile': suite,
    'metric': 'fixture-count',
    'sample': 'value',
    'value': 1.0,
    'unit': 'count',
    'tags': {},
} for suite in suites]
metric_text = '\\n'.join(json.dumps(metric) for metric in metrics) + '\\n'
if os.environ.get('FAKE_BAD_METRICS') == '1':
    metric_text = 'fixture\\n'
(out_dir / 'metrics.ndjson').write_text(metric_text, encoding='utf-8')
(out_dir / 'scoring.json').write_text(json.dumps({
    'result': result,
    'manifest': {'git_sha': os.environ['DEVICE_GIT_SHA'], 'run_id': run_id},
}) + '\\n', encoding='utf-8')
index_lines = ['suite\\tstatus\\tjson\\txml\\tlog\\tmetric_count']
index_lines.extend(
    f'{suite}\\tPASS\\t{out_dir / (suite + ".json")}\\t{out_dir / (suite + ".xml")}\\t{out_dir / ("wrong.log" if os.environ.get("FAKE_WRONG_INDEX_PATH") == "1" else suite + ".log")}\\t1'
    for suite in suites
)
(out_dir / 'suite_index.tsv').write_text('\\n'.join(index_lines) + '\\n', encoding='utf-8')
if os.environ.get('FAKE_MUTATE_TRACKED') == '1':
    Path('README.md').write_text('mutated during device run\\n', encoding='utf-8')
if os.environ.get('FAKE_CREATE_UNTRACKED') == '1':
    Path('untracked_source.cpp').write_text('mutation\\n', encoding='utf-8')
raise SystemExit(int(os.environ.get('FAKE_DEVICE_EXIT', '0')))
"""
        % (EXPECTED_SUITES,),
    )
    fake_pio = root / "fake-pio.py"
    write_executable(
        fake_pio,
        """#!/usr/bin/env python3
import os
from pathlib import Path
marker = os.environ.get('FAKE_RESTORE_MARKER')
if marker:
    Path(marker).write_text('restored\\n', encoding='utf-8')
raise SystemExit(int(os.environ.get('FAKE_RESTORE_EXIT', '0')))
""",
    )
    return {
        "repository": repository,
        "target_sha": target_sha,
        "port": fake_port,
        "template": template,
        "inventory": inventory,
        "ports": ports,
        "device_runner": device_runner,
        "pio": fake_pio,
    }


def run_fixture(
    fixture: dict[str, Path | str],
    root: Path,
    *,
    suite_status: str = "PASS",
    device_exit: int = 0,
    restore_exit: int = 0,
    manifest_result: str = "NO_BASELINE",
    duplicate_suite: bool = False,
    malformed_suite: bool = False,
    duplicate_manifest_key: bool = False,
    mutate_tracked: bool = False,
    create_untracked: bool = False,
    missing_device_command: bool = False,
    wrong_suite_identity: bool = False,
    wrong_index_path: bool = False,
    bad_metrics: bool = False,
    git_override_repository: Path | None = None,
) -> tuple[subprocess.CompletedProcess[str], Path, Path]:
    out_dir = root / "out"
    restore_marker = root / "restored.txt"
    environment = os.environ.copy()
    environment.update(
        {
            "FAKE_SUITE_STATUS": suite_status,
            "FAKE_DEVICE_EXIT": str(device_exit),
            "FAKE_RESTORE_EXIT": str(restore_exit),
            "FAKE_RESTORE_MARKER": str(restore_marker),
            "FAKE_MANIFEST_RESULT": manifest_result,
            "FAKE_DUPLICATE_SUITE": "1" if duplicate_suite else "0",
            "FAKE_MALFORMED_SUITE": "1" if malformed_suite else "0",
            "FAKE_DUPLICATE_MANIFEST_KEY": "1" if duplicate_manifest_key else "0",
            "FAKE_MUTATE_TRACKED": "1" if mutate_tracked else "0",
            "FAKE_CREATE_UNTRACKED": "1" if create_untracked else "0",
            "FAKE_WRONG_SUITE_ID": "1" if wrong_suite_identity else "0",
            "FAKE_WRONG_INDEX_PATH": "1" if wrong_index_path else "0",
            "FAKE_BAD_METRICS": "1" if bad_metrics else "0",
            "V1SIMPLE_HIL_TEST_HOOKS": "1",
        }
    )
    if git_override_repository is not None:
        environment["GIT_DIR"] = str(git_override_repository / ".git")
        environment["GIT_WORK_TREE"] = str(git_override_repository)
    command = [
        "python3",
        "-B",
        str(RUNNER),
        "--run-device-suite",
        "--board",
        "release",
        "--repo-root",
        str(fixture["repository"]),
        "--template",
        str(fixture["template"]),
        "--inventory",
        str(fixture["inventory"]),
        "--ports-json",
        str(fixture["ports"]),
        "--device-runner",
        str(root / "missing-device-runner")
        if missing_device_command
        else str(fixture["device_runner"]),
        "--pio-command",
        str(fixture["pio"]),
        "--out-dir",
        str(out_dir),
    ]
    completed = subprocess.run(
        command,
        cwd=ROOT,
        env=environment,
        capture_output=True,
        text=True,
        check=False,
    )
    return completed, out_dir, restore_marker


def test_success_is_full_sha_bound_sanitized_and_restored() -> None:
    with tempfile.TemporaryDirectory() as raw:
        root = Path(raw)
        fixture = prepare_fixture(root)
        completed, out_dir, marker = run_fixture(fixture, root)
        assert_true(completed.returncode == 0, completed.stdout + completed.stderr)
        result = json.loads((out_dir / "result.json").read_text(encoding="utf-8"))
        assert_true(result["result"] == "TEST_PASS", str(result))
        assert_true(result["authoritative"] is False, str(result))
        assert_true(result["target_sha"] == fixture["target_sha"], str(result))
        assert_true(len(result["target_sha"]) == 40, str(result))
        assert_true(result["production_restored"] is True, str(result))
        assert_true(marker.exists(), "production restoration was not attempted")
        public_output = completed.stdout + completed.stderr + json.dumps(result)
        assert_true("SECRET-USB-IDENTITY" not in public_output, public_output)
        assert_true(str(fixture["port"]) not in public_output, public_output)
        hashes = result["device_artifact_sha256"]
        assert_true(len(hashes) == 30, str(hashes))
        assert_true(
            "manifest.json" in hashes and "test_device_boot.xml" in hashes,
            str(hashes),
        )


def test_existing_output_and_duplicate_suite_rows_fail_closed() -> None:
    with tempfile.TemporaryDirectory() as raw:
        root = Path(raw)
        fixture = prepare_fixture(root)
        completed, out_dir, _ = run_fixture(fixture, root)
        assert_true(completed.returncode == 0, completed.stdout + completed.stderr)
        repeated, _, _ = run_fixture(fixture, root)
        assert_true(repeated.returncode != 0, "stale output directory must be rejected")
        payload = json.loads(repeated.stdout)
        assert_true(payload["error"]["code"] == "output_not_empty", str(payload))

    with tempfile.TemporaryDirectory() as raw:
        root = Path(raw)
        fixture = prepare_fixture(root)
        completed, out_dir, marker = run_fixture(
            fixture, root, duplicate_suite=True
        )
        assert_true(completed.returncode != 0, "duplicate suite rows must fail")
        result = json.loads((out_dir / "result.json").read_text(encoding="utf-8"))
        assert_true(result["failure_code"] == "device_manifest_incomplete", str(result))
        assert_true(marker.exists(), "production restoration must follow manifest rejection")


def test_nonpass_transport_status_fails_closed_and_restores() -> None:
    with tempfile.TemporaryDirectory() as raw:
        root = Path(raw)
        fixture = prepare_fixture(root)
        completed, out_dir, marker = run_fixture(
            fixture, root, suite_status="INFRA_ERROR"
        )
        assert_true(completed.returncode != 0, "infra status must fail the wrapper")
        result = json.loads((out_dir / "result.json").read_text(encoding="utf-8"))
        assert_true(result["failure_code"] == "device_transport_failed", str(result))
        assert_true(marker.exists(), "production restoration must follow validation failure")


def test_nonzero_device_or_restore_exit_fails() -> None:
    with tempfile.TemporaryDirectory() as raw:
        root = Path(raw)
        fixture = prepare_fixture(root)
        completed, out_dir, marker = run_fixture(fixture, root, device_exit=7)
        assert_true(completed.returncode != 0, "device command failure must fail")
        result = json.loads((out_dir / "result.json").read_text(encoding="utf-8"))
        assert_true(result["failure_code"] == "device_suite_failed", str(result))
        assert_true(marker.exists(), "production restoration must follow device failure")

    with tempfile.TemporaryDirectory() as raw:
        root = Path(raw)
        fixture = prepare_fixture(root)
        completed, out_dir, _ = run_fixture(fixture, root, restore_exit=9)
        assert_true(completed.returncode != 0, "production restore failure must fail")
        result = json.loads((out_dir / "result.json").read_text(encoding="utf-8"))
        assert_true(result["failure_code"] == "production_restore_failed", str(result))


def test_git_mutation_fails_and_command_errors_still_emit_results() -> None:
    for mutation in ({"mutate_tracked": True}, {"create_untracked": True}):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            fixture = prepare_fixture(root)
            completed, out_dir, marker = run_fixture(fixture, root, **mutation)
            assert_true(completed.returncode != 0, "worktree mutation must fail")
            result = json.loads((out_dir / "result.json").read_text(encoding="utf-8"))
            assert_true(result["failure_code"] == "target_mutated", str(result))
            assert_true(
                not marker.exists(),
                "a mutated target must never be rebuilt or flashed during restoration",
            )

    with tempfile.TemporaryDirectory() as raw:
        root = Path(raw)
        fixture = prepare_fixture(root)
        completed, out_dir, marker = run_fixture(
            fixture, root, missing_device_command=True
        )
        assert_true(completed.returncode != 0, "missing device command must fail")
        result = json.loads((out_dir / "result.json").read_text(encoding="utf-8"))
        assert_true(result["failure_code"] == "command_unavailable", str(result))
        assert_true(marker.exists(), "production restoration must follow start failure")
        assert_true(str(root) not in completed.stdout + completed.stderr, "local path leaked")

    with tempfile.TemporaryDirectory() as raw:
        root = Path(raw)
        fixture = prepare_fixture(root)
        completed, out_dir, marker = run_fixture(
            fixture, root, malformed_suite=True
        )
        assert_true(completed.returncode != 0, "malformed suite identity must fail")
        result = json.loads((out_dir / "result.json").read_text(encoding="utf-8"))
        assert_true(result["failure_code"] == "device_manifest_invalid", str(result))
        assert_true(marker.exists(), "production restoration must follow malformed evidence")
        assert_true("Traceback" not in completed.stderr, completed.stderr)

    with tempfile.TemporaryDirectory() as raw:
        root = Path(raw)
        fixture = prepare_fixture(root)
        completed, out_dir, marker = run_fixture(
            fixture, root, duplicate_manifest_key=True
        )
        assert_true(completed.returncode != 0, "duplicate manifest keys must fail")
        result = json.loads((out_dir / "result.json").read_text(encoding="utf-8"))
        assert_true(result["failure_code"] == "artifact_invalid", str(result))
        assert_true(marker.exists(), "production restoration must follow duplicate JSON")

    for mutation in (
        {"wrong_suite_identity": True},
        {"wrong_index_path": True},
        {"bad_metrics": True},
    ):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            fixture = prepare_fixture(root)
            completed, out_dir, marker = run_fixture(fixture, root, **mutation)
            assert_true(completed.returncode != 0, "cross-report identity mutation must fail")
            result = json.loads((out_dir / "result.json").read_text(encoding="utf-8"))
            assert_true(result["failure_code"] == "device_report_invalid", str(result))
            assert_true(marker.exists(), "production restoration must follow report rejection")


def test_unimplemented_case_cannot_false_pass() -> None:
    completed = subprocess.run(
        ["python3", "-B", str(RUNNER), "--case", "BSC-02", "--board", "release"],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    assert_true(completed.returncode == 3, completed.stdout + completed.stderr)
    payload = json.loads(completed.stdout)
    assert_true(payload["error"]["code"] == "case_driver_unavailable", str(payload))


def test_authoritative_mode_rejects_tool_path_overrides() -> None:
    completed = subprocess.run(
        [
            "python3",
            "-B",
            str(RUNNER),
            "--run-device-suite",
            "--board",
            "release",
            "--repo-root",
            "/tmp/not-the-repository",
        ],
        cwd=ROOT,
        env={key: value for key, value in os.environ.items() if key != "V1SIMPLE_HIL_TEST_HOOKS"},
        capture_output=True,
        text=True,
        check=False,
    )
    assert_true(completed.returncode != 0, "authoritative path override must fail")
    payload = json.loads(completed.stdout)
    assert_true(payload["error"]["code"] == "untrusted_override", str(payload))


def test_git_and_child_environment_overrides_are_ignored() -> None:
    with tempfile.TemporaryDirectory() as raw:
        root = Path(raw)
        fixture = prepare_fixture(root)
        alternate = root / "alternate-repository"
        alternate.mkdir()
        initialize_repository(alternate)
        repository = fixture["repository"]
        assert isinstance(repository, Path)
        (repository / "README.md").write_text("dirty target\n", encoding="utf-8")
        completed, _, _ = run_fixture(
            fixture,
            root,
            git_override_repository=alternate,
        )
        assert_true(completed.returncode != 0, "GIT_* override must not hide a dirty target")
        payload = json.loads(completed.stdout)
        assert_true(payload["error"]["code"] == "dirty_target", str(payload))

    injected = {
        "BASH_ENV": "/tmp/fake-bash-env",
        "DYLD_INSERT_LIBRARIES": "/tmp/fake-loader.dylib",
        "GIT_DIR": "/tmp/fake-git",
        "PLATFORMIO_BUILD_FLAGS": "-D FAULT_INJECTION=1",
        "PYTHONPATH": "/tmp/fake-python",
    }
    with mock.patch.dict(os.environ, injected, clear=False):
        environment = hil_runner.authoritative_child_environment(Path("/trusted/bin/pio"))
    for key in injected:
        assert_true(key not in environment, f"authoritative environment retained {key}")

    with tempfile.TemporaryDirectory() as raw:
        fake_bin = Path(raw)
        write_executable(fake_bin / "pio", "#!/bin/sh\nexit 0\n")
        environment = {
            key: value
            for key, value in os.environ.items()
            if key != "V1SIMPLE_HIL_TEST_HOOKS"
        }
        environment["PATH"] = f"{fake_bin}{os.pathsep}{environment.get('PATH', '')}"
        completed = subprocess.run(
            [
                "python3",
                "-B",
                str(RUNNER),
                "--run-device-suite",
                "--board",
                "release",
            ],
            cwd=ROOT,
            env=environment,
            capture_output=True,
            text=True,
            check=False,
        )
        assert_true(completed.returncode != 0, "PATH-injected PlatformIO must fail")
        payload = json.loads(completed.stdout)
        assert_true(payload["error"]["code"] == "untrusted_platformio", str(payload))

    completed = subprocess.run(
        [
            "python3",
            "-B",
            str(RUNNER),
            "--run-device-suite",
            "--board",
            "release",
            "--ports-json",
            "/tmp/caller-ports.json",
        ],
        cwd=ROOT,
        env={key: value for key, value in os.environ.items() if key != "V1SIMPLE_HIL_TEST_HOOKS"},
        capture_output=True,
        text=True,
        check=False,
    )
    assert_true(completed.returncode != 0, "authoritative saved ports must fail")
    payload = json.loads(completed.stdout)
    assert_true(payload["error"]["code"] == "untrusted_override", str(payload))


def main() -> int:
    test_success_is_full_sha_bound_sanitized_and_restored()
    test_existing_output_and_duplicate_suite_rows_fail_closed()
    test_nonpass_transport_status_fails_closed_and_restores()
    test_nonzero_device_or_restore_exit_fails()
    test_git_mutation_fails_and_command_errors_still_emit_results()
    test_unimplemented_case_cannot_false_pass()
    test_authoritative_mode_rejects_tool_path_overrides()
    test_git_and_child_environment_overrides_are_ignored()
    print("bug-squash HIL runner regression tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

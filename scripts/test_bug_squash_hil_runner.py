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
                        "capabilities": [
                            "car-mode",
                            "device-tests",
                            "firmware-execution",
                            "persistence",
                            "serial",
                            "v1-connectivity",
                        ],
                        "usb_serial": "SECRET-USB-IDENTITY",
                    },
                    {
                        "alias": "rig",
                        "capabilities": [
                            "artifact-capture",
                            "bond-peer",
                            "ignition-control",
                            "obd-peer",
                            "power-control",
                            "power-button",
                            "sd-media",
                            "utc-time-source",
                            "v1-peer",
                            "vbus-isolation",
                        ],
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
    bsc03_adapter = root / "fake-bsc03-adapter.py"
    write_executable(
        bsc03_adapter,
        """#!/usr/bin/env python3
from datetime import datetime, timedelta, timezone
import hashlib
import json
import os
import sys

def argument(name):
    return sys.argv[sys.argv.index(name) + 1]

run_index = int(argument('--run-index'))
attempt_id = argument('--attempt-id')
if os.environ.get('FAKE_BSC03_FAIL_ON_RUN') == str(run_index):
    raise SystemExit(7)

def digest(value):
    return hashlib.sha256(value.encode('utf-8')).hexdigest()

now = datetime.now(timezone.utc)
started = now - timedelta(seconds=20)
event_ms = [0, 1, 8000, 11000, 12000]
if os.environ.get('FAKE_BSC03_EARLY_CUT') == '1':
    event_ms[3] = 9000
state_run = 1 if os.environ.get('FAKE_BSC03_REUSE_STATE') == '1' else run_index
state_commitments = {}
for state_class in ('settings', 'bond', 'obd', 'v1-device'):
    value = digest(f'{state_class}-run-{state_run}')
    state_commitments[state_class] = {
        'before_sha256': value,
        'after_sha256': value,
    }
admission_times = {
    'settings': 6000,
    'bond': 7000,
    'obd': 7500,
    'v1-device': 8000,
}
if os.environ.get('FAKE_BSC03_LATE_CLASS') == '1':
    admission_times['v1-device'] = 10001
payload = {
    'schema_version': 1,
    'case_id': argument('--case'),
    'session_id': argument('--session-id'),
    'attempt_id': attempt_id,
    'run_index': run_index,
    'target_sha': argument('--target-sha'),
    'dut_alias': argument('--dut-alias'),
    'rig_alias': argument('--rig-alias'),
    'execution_mode': 'simulated',
    'hardware_observed': False,
    'started_at_utc': started.isoformat(timespec='seconds').replace('+00:00', 'Z'),
    'completed_at_utc': now.isoformat(timespec='seconds').replace('+00:00', 'Z'),
    'preconditions': {
        'vbus_isolated': os.environ.get('FAKE_BSC03_VBUS') != '0',
        'power_rig_qualified': True,
        'power_rig_evidence_sha256': digest('power-rig-evidence'),
        'sd_media_present': True,
        'v1_peer_ready': True,
        'obd_peer_ready': True,
        'bond_peer_ready': True,
        'production_firmware_target_sha': argument('--target-sha'),
    },
    'events': [
        {'id': event_id, 'sequence': sequence, 'elapsed_ms': elapsed}
        for sequence, (event_id, elapsed) in enumerate(zip((
            'mutate-four-persistence-classes',
            'wait-for-persistence-admission',
            'persistence-admitted',
            'isolated-ignition-cut',
            'ignition-restore',
        ), event_ms), start=1)
    ],
    'admissions': {
        state_class: {
            'admitted_elapsed_ms': admission_times[state_class],
            'state_commitment_sha256': state_commitments[state_class]['before_sha256'],
        }
        for state_class in ('settings', 'bond', 'obd', 'v1-device')
    },
    'state_commitments': state_commitments,
    'mutation_commitment_sha256': digest(f'mutation-{state_run}'),
    'hard_cut_commitment_sha256': digest(f'hard-cut-{run_index}-{attempt_id}'),
    'boot_commitments': {
        'before_sha256': digest(f'boot-before-{run_index}-{attempt_id}'),
        'after_sha256': digest(f'boot-after-{run_index}-{attempt_id}'),
    },
    'resets': {
        'expected_kind': 'ignition-hard-cut',
        'planned': 1,
        'observed': 1,
        'unexpected': 0,
    },
    'performance': {'loop_max_us': 200000, 'sample_count': 8},
    'facts': {
        'persistence-admission-ms': 8000,
        'settings-state-survived': True,
        'bond-state-survived': True,
        'obd-state-survived': True,
        'v1-device-state-survived': True,
        'peers-reconnected-without-pairing': True,
        'loop-slo-preserved': True,
        'early-cut-durability-not-claimed': True,
    },
}
sys.stdout.write(json.dumps(payload))
""",
    )
    bsc11_adapter = root / "fake-bsc11-adapter.py"
    write_executable(
        bsc11_adapter,
        """#!/usr/bin/env python3
from datetime import datetime, timedelta, timezone
import hashlib
import json
import os
import sys

def argument(name):
    return sys.argv[sys.argv.index(name) + 1]

def digest(value):
    return hashlib.sha256(value.encode('utf-8')).hexdigest()

def commitment(payload):
    canonical = json.dumps(payload, ensure_ascii=False, separators=(',', ':'), sort_keys=True)
    return hashlib.sha256(b'v1simple.bsc11.case-record.v1\\0' + canonical.encode('utf-8')).hexdigest()

now = datetime.now(timezone.utc)
started = now - timedelta(seconds=72)
event_ms = [0, 500, 1000, 62001, 68001, 69000, 70000]
if os.environ.get('FAKE_BSC11_SHORT') == '1':
    event_ms = [0, 500, 1000, 61000, 67000, 68000, 69000]
attempt_id = argument('--attempt-id')
services = {}
for service in ('alp', 'ble', 'display', 'logging', 'wifi'):
    services[service] = {
        'continuous': os.environ.get('FAKE_BSC11_SERVICE_LOSS') != service,
        'sample_count': 20,
        'first_observed_elapsed_ms': event_ms[2],
        'during_hold_elapsed_ms': event_ms[3] + 3000,
        'last_observed_elapsed_ms': event_ms[4] + 500,
        'maximum_gap_ms': 4000,
        'evidence_sha256': digest(f'{service}-continuity'),
    }
service_mutation = os.environ.get('FAKE_BSC11_SERVICE_MUTATION')
if service_mutation == 'missing-first':
    del services['alp']['first_observed_elapsed_ms']
elif service_mutation == 'late-first':
    services['alp']['first_observed_elapsed_ms'] = event_ms[3]
elif service_mutation == 'absent-during':
    del services['alp']['during_hold_elapsed_ms']
elif service_mutation == 'out-of-window-during':
    services['alp']['during_hold_elapsed_ms'] = event_ms[4] + 1
elif service_mutation == 'early-last':
    services['alp']['last_observed_elapsed_ms'] = event_ms[4]
elif service_mutation == 'excessive-gap':
    services['alp']['maximum_gap_ms'] = 5001
forbidden = {
    field: 0
    for field in (
        'auto-power-timer-fired',
        'shutdown-preparation-entered',
        'goodbye-frame-presented',
        'clean-shutdown-marker-written',
        'power-latch-action',
        'deep-sleep-entered',
    )
}
forbidden_key = os.environ.get('FAKE_BSC11_FORBIDDEN')
if forbidden_key:
    forbidden[forbidden_key] = 1
payload = {
    'schema_version': 1,
    'case_id': argument('--case'),
    'session_id': argument('--session-id'),
    'attempt_id': attempt_id,
    'target_sha': argument('--target-sha'),
    'dut_alias': argument('--dut-alias'),
    'rig_alias': argument('--rig-alias'),
    'execution_mode': 'simulated',
    'hardware_observed': False,
    'started_at_utc': started.isoformat(timespec='seconds').replace('+00:00', 'Z'),
    'completed_at_utc': now.isoformat(timespec='seconds').replace('+00:00', 'Z'),
    'firmware': {
        'environment': 'waveshare-349' if os.environ.get('FAKE_BSC11_WRONG_FIRMWARE') == '1' else 'esp32-s3-car-install',
        'target_sha': argument('--target-sha'),
        'binary_sha256': digest('car-production-binary'),
        'car_mode_define': True,
    },
    'preconditions': {
        'vbus_isolated': True,
        'ignition_present': True,
        'real_v1_peer': True,
        'auto_power_off_minutes': 1,
        'ignition_rig_evidence_sha256': digest('ignition-rig'),
    },
    'events': [
        {'id': event_id, 'sequence': sequence, 'elapsed_ms': elapsed}
        for sequence, (event_id, elapsed) in enumerate(zip((
            'car-ignition-established',
            'real-v1-received',
            'real-v1-disconnected',
            'auto-power-window-exceeded',
            'long-pwr-hold-completed',
            'ignition-removed',
            'ignition-power-down',
        ), event_ms), start=1)
    ],
    'long_press': {
        'started_elapsed_ms': event_ms[3],
        'completed_elapsed_ms': event_ms[4],
        'duration_ms': 6000,
        'inert': True,
        'evidence_sha256': digest('long-press'),
    },
    'forbidden_activity': forbidden,
    'services': services,
    'power': {
        'ignition_present_through_observation': True,
        'expected_power_event_kind': 'ignition-removal',
        'observed_power_events': 1,
        'unexpected_resets_before_removal': 0,
        'power_downs_before_removal': 0,
        'ignition_removal_elapsed_ms': event_ms[5],
        'power_down_elapsed_ms': event_ms[6],
        'power_down_source': 'ignition-removal',
        'vbus_present_at_power_down': os.environ.get('FAKE_BSC11_USB_POWER') == '1',
        'evidence_sha256': digest('power-transition'),
    },
    'capture_commitments': {
        'display_video_sha256': digest('display-video'),
        'ignition_timeline_sha256': digest('ignition-timeline'),
        'serial_log_sha256': digest('serial-log'),
        'service_timeline_sha256': digest('service-timeline'),
        'v1_exchange_sha256': digest('v1-exchange'),
    },
}
if os.environ.get('FAKE_BSC11_REUSE') == '1':
    payload['capture_commitments']['serial_log_sha256'] = payload['capture_commitments']['display_video_sha256']
payload['evidence_binding_sha256'] = commitment(payload)
if os.environ.get('FAKE_BSC11_TAMPER') == '1':
    payload['capture_commitments']['serial_log_sha256'] = digest('tampered-serial-log')
sys.stdout.write(json.dumps(payload))
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
        "bsc03_adapter": bsc03_adapter,
        "bsc11_adapter": bsc11_adapter,
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


def run_bsc03_fixture(
    fixture: dict[str, Path | str],
    root: Path,
    *,
    out_dir: Path | None = None,
    runs: int = 3,
    include_acknowledgements: bool = True,
    resume: bool = False,
    recover_incomplete: bool = False,
    missing_inventory: bool = False,
    fail_on_run: int | None = None,
    vbus_verified: bool = True,
    early_cut: bool = False,
    reuse_state: bool = False,
    late_class_admission: bool = False,
) -> tuple[subprocess.CompletedProcess[str], Path]:
    if out_dir is None:
        out_dir = root / "bsc03-out"
    environment = os.environ.copy()
    environment.update(
        {
            "V1SIMPLE_HIL_TEST_HOOKS": "1",
            "FAKE_BSC03_FAIL_ON_RUN": str(fail_on_run) if fail_on_run is not None else "",
            "FAKE_BSC03_VBUS": "1" if vbus_verified else "0",
            "FAKE_BSC03_EARLY_CUT": "1" if early_cut else "0",
            "FAKE_BSC03_REUSE_STATE": "1" if reuse_state else "0",
            "FAKE_BSC03_LATE_CLASS": "1" if late_class_admission else "0",
        }
    )
    command = [
        "python3",
        "-B",
        str(RUNNER),
        "--case",
        "BSC-03",
        "--board",
        "release",
        "--rig",
        "rig",
        "--runs",
        str(runs),
        "--repo-root",
        str(fixture["repository"]),
        "--template",
        str(fixture["template"]),
        "--inventory",
        str(root / "missing-inventory.json")
        if missing_inventory
        else str(fixture["inventory"]),
        "--ports-json",
        str(fixture["ports"]),
        "--pio-command",
        str(fixture["pio"]),
        "--case-adapter",
        str(fixture["bsc03_adapter"]),
        "--out-dir",
        str(out_dir),
    ]
    if include_acknowledgements:
        command.extend(
            (
                "--ack-vbus-isolated",
                "--ack-destructive-hard-cuts",
                "--ack-early-cut-not-qualified",
            )
        )
    if resume:
        command.append("--resume")
    if recover_incomplete:
        command.append("--ack-incomplete-run-recovered")
    completed = subprocess.run(
        command,
        cwd=ROOT,
        env=environment,
        capture_output=True,
        text=True,
        check=False,
    )
    return completed, out_dir


def run_bsc11_fixture(
    fixture: dict[str, Path | str],
    root: Path,
    *,
    include_acknowledgement: bool = True,
    runs: int = 1,
    short_observation: bool = False,
    forbidden_activity: str = "",
    service_loss: str = "",
    service_mutation: str = "",
    usb_power_down: bool = False,
    wrong_firmware: bool = False,
    reused_evidence: bool = False,
    tampered_evidence: bool = False,
) -> tuple[subprocess.CompletedProcess[str], Path]:
    out_dir = root / "bsc11-out"
    environment = os.environ.copy()
    environment.update(
        {
            "V1SIMPLE_HIL_TEST_HOOKS": "1",
            "FAKE_BSC11_SHORT": "1" if short_observation else "0",
            "FAKE_BSC11_FORBIDDEN": forbidden_activity,
            "FAKE_BSC11_SERVICE_LOSS": service_loss,
            "FAKE_BSC11_SERVICE_MUTATION": service_mutation,
            "FAKE_BSC11_USB_POWER": "1" if usb_power_down else "0",
            "FAKE_BSC11_WRONG_FIRMWARE": "1" if wrong_firmware else "0",
            "FAKE_BSC11_REUSE": "1" if reused_evidence else "0",
            "FAKE_BSC11_TAMPER": "1" if tampered_evidence else "0",
        }
    )
    command = [
        "python3",
        "-B",
        str(RUNNER),
        "--case",
        "BSC-11",
        "--board",
        "release",
        "--rig",
        "rig",
        "--runs",
        str(runs),
        "--repo-root",
        str(fixture["repository"]),
        "--template",
        str(fixture["template"]),
        "--inventory",
        str(fixture["inventory"]),
        "--ports-json",
        str(fixture["ports"]),
        "--pio-command",
        str(fixture["pio"]),
        "--case-adapter",
        str(fixture["bsc11_adapter"]),
        "--out-dir",
        str(out_dir),
    ]
    if include_acknowledgement:
        command.append("--ack-vbus-isolated")
    completed = subprocess.run(
        command,
        cwd=ROOT,
        env=environment,
        capture_output=True,
        text=True,
        check=False,
    )
    return completed, out_dir


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
        binding_path = out_dir / "raw" / "board-resolution-binding.json"
        attestation_path = out_dir / "resolver-attestation.json"
        binding = json.loads(binding_path.read_text(encoding="utf-8"))
        attestation = json.loads(attestation_path.read_text(encoding="utf-8"))
        assert_true(
            binding["inventory_record"]["connection"]["usb_serial"]
            == "SECRET-USB-IDENTITY",
            "private inventory identity is retained only in the ignored raw binding",
        )
        assert_true(attestation["schema_version"] == 2, str(attestation))
        assert_true(
            attestation
            == hil_runner.qualification.build_board_inventory_attestation(
                binding,
                observed_at_utc=attestation["observed_at_utc"],
            ),
            "published attestation exactly commits to the selected private inventory",
        )
        attestation_text = json.dumps(attestation)
        assert_true("SECRET-USB-IDENTITY" not in attestation_text, attestation_text)
        assert_true(str(fixture["port"]) not in attestation_text, attestation_text)
        hashes = result["device_artifact_sha256"]
        assert_true(len(hashes) == 30, str(hashes))
        assert_true(
            "manifest.json" in hashes and "test_device_boot.xml" in hashes,
            str(hashes),
        )
        assert_true(
            result["artifact_sha256"]["board_resolution_binding"]
            == hil_runner.sha256_file(binding_path),
            "result binds the private board-resolution record without publishing it",
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


def test_bsc03_simulation_is_three_run_bound_and_explicitly_nonqualifying() -> None:
    with tempfile.TemporaryDirectory() as raw:
        root = Path(raw)
        fixture = prepare_fixture(root)
        completed, out_dir = run_bsc03_fixture(fixture, root)
        assert_true(completed.returncode == 0, completed.stdout + completed.stderr)
        result = json.loads(
            (out_dir / "collection_result.json").read_text(encoding="utf-8")
        )
        assert_true(result["result"] == "TEST_PASS", str(result))
        assert_true(result["execution_mode"] == "simulated", str(result))
        assert_true(result["hardware_observed"] is False, str(result))
        assert_true(result["authoritative"] is False, str(result))
        assert_true(result["physical_collection_completed"] is False, str(result))
        assert_true(result["non_qualifying"] is True, str(result))
        assert_true(result["qualification_status"] == "BLOCKED", str(result))
        assert_true(result["runs_completed"] == 3, str(result))
        assert_true(result["early_cut_durability_claimed"] is False, str(result))
        assert_true(
            "case-source-provenance-not-authenticated" in result["qualification_blockers"],
            str(result),
        )
        records = [
            json.loads((out_dir / row["artifact"]).read_text(encoding="utf-8"))
            for row in result["run_artifacts"]
        ]
        assert_true(
            [record["run_index"] for record in records] == [1, 2, 3],
            str(records),
        )
        assert_true(
            all(
                record["execution_mode"] == "simulated"
                and record["hardware_observed"] is False
                for record in records
            ),
            str(records),
        )
        public_output = completed.stdout + completed.stderr + json.dumps(result)
        assert_true("SECRET-USB-IDENTITY" not in public_output, public_output)
        assert_true(str(fixture["port"]) not in public_output, public_output)


def test_bsc03_rejects_missing_preconditions_timing_and_reused_runs() -> None:
    cases = (
        ({"include_acknowledgements": False}, "operator_preconditions_incomplete"),
        ({"missing_inventory": True}, "local_inventory_missing"),
        ({"vbus_verified": False}, "case_record_invalid"),
        ({"early_cut": True}, "case_record_invalid"),
        ({"late_class_admission": True}, "case_record_invalid"),
        ({"reuse_state": True}, "case_runs_reused"),
        ({"runs": 2}, "invalid_runs"),
    )
    for options, expected_code in cases:
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            fixture = prepare_fixture(root)
            completed, out_dir = run_bsc03_fixture(fixture, root, **options)
            assert_true(completed.returncode != 0, f"{options} unexpectedly passed")
            payload = json.loads(completed.stdout)
            assert_true(payload["error"]["code"] == expected_code, str(payload))
            assert_true(
                not (out_dir / "collection_result.json").exists(),
                f"{options} produced a qualifying-looking result",
            )
            assert_true("SECRET-USB-IDENTITY" not in completed.stdout, completed.stdout)


def test_bsc03_resume_requires_recovery_and_never_counts_interrupted_attempt() -> None:
    with tempfile.TemporaryDirectory() as raw:
        root = Path(raw)
        fixture = prepare_fixture(root)
        failed, out_dir = run_bsc03_fixture(fixture, root, fail_on_run=2)
        assert_true(failed.returncode != 0, "interrupted adapter must fail")
        failure = json.loads(failed.stdout)
        assert_true(failure["error"]["code"] == "case_adapter_failed", str(failure))
        checkpoint = json.loads((out_dir / "checkpoint.json").read_text(encoding="utf-8"))
        assert_true(len(checkpoint["completed_attempts"]) == 1, str(checkpoint))
        interrupted_id = checkpoint["active_attempt"]["attempt_id"]

        unsafe, _ = run_bsc03_fixture(fixture, root, out_dir=out_dir, resume=True)
        assert_true(unsafe.returncode != 0, "resume without recovery must fail")
        unsafe_payload = json.loads(unsafe.stdout)
        assert_true(
            unsafe_payload["error"]["code"] == "incomplete_run_recovery_required",
            str(unsafe_payload),
        )

        resumed, _ = run_bsc03_fixture(
            fixture,
            root,
            out_dir=out_dir,
            resume=True,
            recover_incomplete=True,
        )
        assert_true(resumed.returncode == 0, resumed.stdout + resumed.stderr)
        result = json.loads(
            (out_dir / "collection_result.json").read_text(encoding="utf-8")
        )
        checkpoint = json.loads((out_dir / "checkpoint.json").read_text(encoding="utf-8"))
        completed_ids = {row["attempt_id"] for row in checkpoint["completed_attempts"]}
        assert_true(result["result"] == "TEST_PASS", str(result))
        assert_true(result["runs_completed"] == 3, str(result))
        assert_true(interrupted_id in checkpoint["abandoned_attempt_ids"], str(checkpoint))
        assert_true(interrupted_id not in completed_ids, str(checkpoint))


def test_bsc11_simulation_is_one_run_bound_hashed_and_nonqualifying() -> None:
    with tempfile.TemporaryDirectory() as raw:
        root = Path(raw)
        fixture = prepare_fixture(root)
        completed, out_dir = run_bsc11_fixture(fixture, root)
        assert_true(completed.returncode == 0, completed.stdout + completed.stderr)
        result = json.loads(
            (out_dir / "collection_result.json").read_text(encoding="utf-8")
        )
        assert_true(result["result"] == "TEST_PASS", str(result))
        assert_true(result["execution_mode"] == "simulated", str(result))
        assert_true(result["authoritative"] is False, str(result))
        assert_true(result["hardware_observed"] is False, str(result))
        assert_true(result["physical_collection_completed"] is False, str(result))
        assert_true(result["non_qualifying"] is True, str(result))
        assert_true(result["qualification_status"] == "BLOCKED", str(result))
        assert_true(result["runs_required"] == 1, str(result))
        assert_true(result["runs_completed"] == 1, str(result))
        assert_true(
            result["production_target"]["environment"] == "esp32-s3-car-install",
            str(result),
        )
        assert_true(
            result["production_target"]["target_sha"] == fixture["target_sha"],
            str(result),
        )
        assert_true(
            result["configured_auto_power_off_minutes"] == 1
            and result["minimum_observation_ms"] == 60_000,
            str(result),
        )
        assert_true(
            all(
                len(value) == 64
                for value in (
                    result["session_sha256"],
                    result["attempt_sha256"],
                    result["evidence_binding_sha256"],
                    *result["artifact_sha256"].values(),
                )
            ),
            str(result),
        )
        assert_true(
            not (out_dir / "qualification_result.json").exists(),
            "mock collection must never emit qualification evidence",
        )
        public_output = completed.stdout + completed.stderr + json.dumps(result)
        assert_true("SECRET-USB-IDENTITY" not in public_output, public_output)
        assert_true(str(fixture["port"]) not in public_output, public_output)
        assert_true("release" not in public_output and '"rig"' not in public_output, public_output)


def test_bsc11_rejects_short_wrong_target_and_missing_operator_boundary() -> None:
    cases = (
        ({"short_observation": True}, "case_record_invalid"),
        ({"wrong_firmware": True}, "case_record_invalid"),
        ({"include_acknowledgement": False}, "operator_preconditions_incomplete"),
        ({"runs": 2}, "invalid_runs"),
    )
    for options, expected_code in cases:
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            fixture = prepare_fixture(root)
            completed, out_dir = run_bsc11_fixture(fixture, root, **options)
            assert_true(completed.returncode != 0, f"{options} unexpectedly passed")
            payload = json.loads(completed.stdout)
            assert_true(payload["error"]["code"] == expected_code, str(payload))
            assert_true(
                not (out_dir / "collection_result.json").exists(),
                f"{options} produced a collection result",
            )
            assert_true(
                not (out_dir / "qualification_result.json").exists(),
                f"{options} produced qualification evidence",
            )


def test_bsc11_rejects_every_portable_shutdown_action_and_service_loss() -> None:
    for forbidden in (
        "auto-power-timer-fired",
        "shutdown-preparation-entered",
        "goodbye-frame-presented",
        "clean-shutdown-marker-written",
        "power-latch-action",
        "deep-sleep-entered",
    ):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            fixture = prepare_fixture(root)
            completed, out_dir = run_bsc11_fixture(
                fixture, root, forbidden_activity=forbidden
            )
            assert_true(completed.returncode != 0, f"{forbidden} unexpectedly passed")
            payload = json.loads(completed.stdout)
            assert_true(payload["error"]["code"] == "case_record_invalid", str(payload))
            assert_true(not (out_dir / "collection_result.json").exists(), forbidden)

    for service in ("alp", "ble", "display", "logging", "wifi"):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            fixture = prepare_fixture(root)
            completed, out_dir = run_bsc11_fixture(
                fixture, root, service_loss=service
            )
            assert_true(completed.returncode != 0, f"{service} loss unexpectedly passed")
            payload = json.loads(completed.stdout)
            assert_true(payload["error"]["code"] == "case_record_invalid", str(payload))
            assert_true(not (out_dir / "collection_result.json").exists(), service)


def test_bsc11_rejects_service_evidence_that_does_not_span_the_hold() -> None:
    for mutation in (
        "missing-first",
        "late-first",
        "absent-during",
        "out-of-window-during",
        "early-last",
        "excessive-gap",
    ):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            fixture = prepare_fixture(root)
            completed, out_dir = run_bsc11_fixture(
                fixture,
                root,
                service_mutation=mutation,
            )
            assert_true(completed.returncode != 0, f"{mutation} unexpectedly passed")
            payload = json.loads(completed.stdout)
            assert_true(payload["error"]["code"] == "case_record_invalid", str(payload))
            assert_true(not (out_dir / "collection_result.json").exists(), mutation)
            assert_true(not (out_dir / "qualification_result.json").exists(), mutation)


def test_bsc11_rejects_usb_powerdown_reused_and_tampered_evidence() -> None:
    for options in (
        {"usb_power_down": True},
        {"reused_evidence": True},
        {"tampered_evidence": True},
    ):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            fixture = prepare_fixture(root)
            completed, out_dir = run_bsc11_fixture(fixture, root, **options)
            assert_true(completed.returncode != 0, f"{options} unexpectedly passed")
            payload = json.loads(completed.stdout)
            assert_true(payload["error"]["code"] == "case_record_invalid", str(payload))
            if options == {"reused_evidence": True}:
                assert_true(
                    payload["error"]["message"]
                    == "BSC-11 evidence roles reused the same capture",
                    str(payload),
                )
            assert_true(not (out_dir / "collection_result.json").exists(), str(options))
            assert_true(not (out_dir / "qualification_result.json").exists(), str(options))


def test_bsc11_physical_mode_remains_blocked_without_tracked_adapter() -> None:
    completed = subprocess.run(
        [
            "python3",
            "-B",
            str(RUNNER),
            "--case",
            "BSC-11",
            "--board",
            "release",
            "--rig",
            "rig",
            "--ack-vbus-isolated",
        ],
        cwd=ROOT,
        env={
            key: value
            for key, value in os.environ.items()
            if key != "V1SIMPLE_HIL_TEST_HOOKS"
        },
        capture_output=True,
        text=True,
        check=False,
    )
    assert_true(completed.returncode != 0, "physical BSC-11 must remain blocked")
    payload = json.loads(completed.stdout)
    assert_true(payload["error"]["code"] == "case_rig_adapter_unavailable", str(payload))


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
    test_bsc03_simulation_is_three_run_bound_and_explicitly_nonqualifying()
    test_bsc03_rejects_missing_preconditions_timing_and_reused_runs()
    test_bsc03_resume_requires_recovery_and_never_counts_interrupted_attempt()
    test_bsc11_simulation_is_one_run_bound_hashed_and_nonqualifying()
    test_bsc11_rejects_short_wrong_target_and_missing_operator_boundary()
    test_bsc11_rejects_every_portable_shutdown_action_and_service_loss()
    test_bsc11_rejects_service_evidence_that_does_not_span_the_hold()
    test_bsc11_rejects_usb_powerdown_reused_and_tampered_evidence()
    test_bsc11_physical_mode_remains_blocked_without_tracked_adapter()
    test_authoritative_mode_rejects_tool_path_overrides()
    test_git_and_child_environment_overrides_are_ignored()
    print("bug-squash HIL runner regression tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

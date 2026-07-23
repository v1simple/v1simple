#!/usr/bin/env python3
"""Regression tests for the fail-closed bug-squash HIL runner."""

from __future__ import annotations

import ast
import copy
import hashlib
import json
import os
from pathlib import Path
import re
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


def read_cpp_integer_constant(path: Path, name: str) -> int:
    source = path.read_text(encoding="utf-8")
    match = re.search(rf"\b{re.escape(name)}\s*=\s*([^;]+);", source)
    if match is None:
        raise AssertionError(f"missing C++ constant {name} in {path.name}")

    def evaluate(node: ast.AST) -> int:
        if isinstance(node, ast.Constant) and type(node.value) is int:
            return node.value
        if isinstance(node, ast.BinOp) and isinstance(node.op, ast.Add):
            return evaluate(node.left) + evaluate(node.right)
        if isinstance(node, ast.BinOp) and isinstance(node.op, ast.Mult):
            return evaluate(node.left) * evaluate(node.right)
        raise AssertionError(f"unsupported C++ constant expression for {name}")

    return evaluate(ast.parse(match.group(1).strip(), mode="eval").body)


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
                            "battery-monitor",
                            "car-mode",
                            "device-tests",
                            "display",
                            "firmware-execution",
                            "maintenance-mode",
                            "obd-connectivity",
                            "persistence",
                            "power-button",
                            "proxy-connectivity",
                            "serial",
                            "touchscreen",
                            "v1-connectivity",
                            "wifi-client",
                        ],
                        "usb_serial": "SECRET-USB-IDENTITY",
                    },
                    {
                        "alias": "rig",
                        "capabilities": [
                            "artifact-capture",
                            "ap-traffic",
                            "battery-source",
                            "bond-peer",
                            "browser-client",
                            "display-capture",
                            "http-response-control",
                            "ignition-control",
                            "lan-client",
                            "logic-analyzer",
                            "obd-peer",
                            "power-control",
                            "power-button",
                            "proxy-client",
                            "programmable-v1-peer",
                            "reset-control",
                            "sd-media",
                            "sram-pressure-control",
                            "utc-time-source",
                            "usb-source",
                            "v1-peer",
                            "vbus-isolation",
                            "wake-input-control",
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
    bsc02_adapter = root / "fake-bsc02-adapter.py"
    write_executable(
        bsc02_adapter,
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
    return hashlib.sha256(b'v1simple.bsc02.case-record.v1\\0' + canonical.encode('utf-8')).hexdigest()

role = argument('--role')
now = datetime.now(timezone.utc)
started = now - timedelta(seconds=50)
is_fault = role == 'fault-collection'
environment = 'waveshare-349-hil' if is_fault else 'waveshare-349'
hil_active = is_fault
if os.environ.get('FAKE_BSC02_WRONG_FIRMWARE') == '1':
    environment = 'waveshare-349' if is_fault else 'waveshare-349-hil'
if os.environ.get('FAKE_BSC02_WRONG_HIL_MARKER') == '1':
    hil_active = not hil_active

fault_collection = None
production_replay = None
if is_fault:
    pressure_low_heap_started = 10020
    pressure_stop = (
        11000
        if os.environ.get('FAKE_BSC02_SHORT_GUARD') == '1'
        else pressure_low_heap_started + 1500
    )
    pressure_first_retry = pressure_stop + 3000
    pressure_later_retry = pressure_first_retry + 30000
    ap_events = [
        {'id': 'ready', 'sequence': 1, 'elapsed_ms': 0, 'reason': 'fresh_ap_admission', 'arm_sequence': 1, 'ready_sequence': 1, 'generation': 1, 'phase': 1},
        {'id': 'fired', 'sequence': 2, 'elapsed_ms': 10, 'reason': 'softap_admission_suppressed', 'arm_sequence': 1, 'ready_sequence': 1, 'generation': 1, 'phase': 1},
        {'id': 'terminal', 'sequence': 3, 'elapsed_ms': 20, 'reason': 'released_after_suppression', 'arm_sequence': 1, 'ready_sequence': 1, 'generation': 1, 'phase': 1},
    ]
    pressure_events = [
        {'id': 'ready', 'sequence': 1, 'elapsed_ms': 10000, 'reason': 'pressure_task_admission', 'arm_sequence': 2, 'ready_sequence': 2, 'generation': 2, 'phase': 2},
        {'id': 'fired', 'sequence': 2, 'elapsed_ms': 10010, 'reason': 'pressure_task_start', 'arm_sequence': 2, 'ready_sequence': 2, 'generation': 2, 'phase': 2},
        {'id': 'competing_observed', 'sequence': 3, 'elapsed_ms': pressure_stop, 'reason': 'wifi_heap_guard_stop', 'arm_sequence': 2, 'ready_sequence': 2, 'generation': 2, 'phase': 2},
        {'id': 'terminal', 'sequence': 4, 'elapsed_ms': pressure_stop + 10, 'reason': 'released', 'arm_sequence': 2, 'ready_sequence': 2, 'generation': 2, 'phase': 2},
    ]
    if os.environ.get('FAKE_BSC02_IDENTITY_DRIFT') == '1':
        pressure_events[2]['ready_sequence'] = 99
    if os.environ.get('FAKE_BSC02_NO_RELEASE') == '1':
        pressure_events[-1]['reason'] = 'pressure_task_start_failed'
    first_retry = 1010 if os.environ.get('FAKE_BSC02_RAPID_RETRY') == '1' else 3010
    later_retry = 23020 if os.environ.get('FAKE_BSC02_RAPID_LATER_RETRY') == '1' else pressure_later_retry
    first_http = 10000 if os.environ.get('FAKE_BSC02_NO_HTTP') == '1' else 3500
    free_floor = 14500 if os.environ.get('FAKE_BSC02_LOW_FREE') == '1' else 15360
    largest_floor = 6500 if os.environ.get('FAKE_BSC02_LOW_BLOCK') == '1' else 7168
    fault_collection = {
        'ap_start': {
            'fault_id': 'wifi-ap-start-fail-once',
            'setup_ap_configured': True,
            'softap_called': False,
            'false_ap_active': os.environ.get('FAKE_BSC02_FALSE_AP') == '1',
            'false_ap_reachable': os.environ.get('FAKE_BSC02_FALSE_AP') == '1',
            'events': ap_events,
        },
        'pressure': {
            'fault_id': 'wifi-internal-sram-hold',
            'allocation_cap_bytes': 65536,
            'allocated_bytes': 49152,
            'task_overhead_bytes': 9000 if os.environ.get('FAKE_BSC02_OVERHEAD') == '1' else 4096,
            'auto_release_ms': 5000,
            'heap_guard_stop_observed': os.environ.get('FAKE_BSC02_NO_HEAP_STOP') != '1',
            'events': pressure_events,
        },
        'recovery': {
            'initial_failure_elapsed_ms': 10,
            'first_retry_elapsed_ms': first_retry,
            'first_http_success_elapsed_ms': first_http,
            'pressure_low_heap_started_elapsed_ms': pressure_low_heap_started,
            'pressure_stop_elapsed_ms': pressure_stop,
            'pressure_first_retry_elapsed_ms': pressure_first_retry,
            'pressure_first_retry_outcome': 'cooldown-rejected',
            'pressure_later_retry_elapsed_ms': later_retry,
            'pressure_http_success_elapsed_ms': later_retry + 480,
            'maintenance_mode_continuous': os.environ.get('FAKE_BSC02_NO_MAINTENANCE') != '1',
            'unexpected_resets': 0,
        },
        'heap': {
            'configured_free_floor_bytes': 16384,
            'configured_largest_block_floor_bytes': 8192,
            'safety_free_floor_bytes': 14848,
            'safety_largest_block_floor_bytes': 6656,
            'absolute_minimum_free_bytes': 14336,
            'absolute_minimum_largest_block_bytes': 6144,
            'minimum_free_bytes': free_floor,
            'minimum_largest_block_bytes': largest_floor,
            'samples': [
                {'phase': 'before', 'free_bytes': 100000, 'largest_block_bytes': 65536},
                {'phase': 'pressured', 'free_bytes': free_floor, 'largest_block_bytes': largest_floor},
                {'phase': 'released', 'free_bytes': 90000, 'largest_block_bytes': 60000},
            ],
        },
    }
else:
    production_replay = {
        'maintenance_mode_continuous': os.environ.get('FAKE_BSC02_NO_MAINTENANCE') != '1',
        'http_status_recovered': os.environ.get('FAKE_BSC02_NO_HTTP') != '1',
        'fault_events_seen': 1 if os.environ.get('FAKE_BSC02_REPLAY_FAULT') == '1' else 0,
        'unexpected_resets': 0,
    }

payload = {
    'schema_version': 1,
    'case_id': argument('--case'),
    'role': role,
    'session_id': argument('--session-id'),
    'attempt_id': argument('--attempt-id'),
    'target_sha': argument('--target-sha'),
    'dut_alias': argument('--dut-alias'),
    'rig_alias': argument('--rig-alias'),
    'execution_mode': 'simulated',
    'hardware_observed': False,
    'started_at_utc': started.isoformat(timespec='seconds').replace('+00:00', 'Z'),
    'completed_at_utc': now.isoformat(timespec='seconds').replace('+00:00', 'Z'),
    'firmware': {
        'environment': environment,
        'target_sha': argument('--target-sha'),
        'binary_sha256': digest(f'{role}-binary'),
        'build_manifest_sha256': digest(f'{role}-build-manifest'),
        'hil_fault_control_active': hil_active,
    },
    'preconditions': {
        'maintenance_mode': True,
        'http_probe_ready': True,
        'unexpected_resets_before_start': 0,
    },
    'fault_collection': fault_collection,
    'production_replay': production_replay,
    'capture_commitments': {
        'build_evidence_sha256': digest(f'{role}-build-evidence'),
        'heap_timeline_sha256': digest(f'{role}-heap-timeline'),
        'http_timeline_sha256': digest(f'{role}-http-timeline'),
        'lifecycle_timeline_sha256': digest(f'{role}-lifecycle-timeline'),
        'serial_log_sha256': digest(f'{role}-serial-log'),
    },
}
if os.environ.get('FAKE_BSC02_REUSE') == '1':
    payload['capture_commitments']['serial_log_sha256'] = payload['capture_commitments']['http_timeline_sha256']
payload['evidence_binding_sha256'] = commitment(payload)
if os.environ.get('FAKE_BSC02_TAMPER') == '1':
    payload['capture_commitments']['serial_log_sha256'] = digest('tampered-serial')
sys.stdout.write(json.dumps(payload))
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
    bsc04_adapter = root / "fake-bsc04-adapter.py"
    write_executable(
        bsc04_adapter,
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
    return hashlib.sha256(b'v1simple.bsc04.case-record.v1\\0' + canonical.encode('utf-8')).hexdigest()

role = argument('--role')
is_fault = role == 'fault-collection'
now = datetime.now(timezone.utc)
started = now - timedelta(seconds=50)
event_ids = [
    'late-v1-power-enabled',
    'late-v1-connected',
    'v1-settling-entered',
    'verify-push-suppressed',
    'hard-deadline-exit',
    'obd-sequencing-started',
    'proxy-sequencing-started',
] if is_fault else [
    'late-v1-power-enabled',
    'late-v1-connected',
    'v1-settling-entered',
    'verify-push-accepted',
    'obd-sequencing-started',
    'proxy-sequencing-started',
]
event_ms = [31000, 31020, 31020, 34000, 41050, 42000, 43000] if is_fault else [
    31000, 31020, 31020, 34000, 35000, 36000
]
loop_sequences = [100, 101, 101, 200, 300, 400, 500] if is_fault else [100, 101, 101, 200, 300, 400]
events = [
    {
        'id': event_id,
        'sequence': sequence,
        'elapsed_ms': elapsed,
        'loop_sequence': loop_sequence,
        'result': 'pass',
    }
    for sequence, (event_id, elapsed, loop_sequence) in enumerate(
        zip(event_ids, event_ms, loop_sequences), start=1
    )
]
if os.environ.get('FAKE_BSC04_MISSING_EVENT') == '1':
    events.pop()
if os.environ.get('FAKE_BSC04_NOT_SAME_LOOP') == '1':
    events[2]['loop_sequence'] += 1

facts = {
    'late_connection_delay_ms': 31020,
    'entry_state': 'STEADY',
    'same_loop_reentry': True,
    'settle_exit_elapsed_ms': 10030 if is_fault else 2980,
    'verify_push_match_observed': True,
    'verify_push_suppressed': is_fault,
    'hard_deadline_used': is_fault,
    'v1_connected_through_exit': True,
    'obd_started_without_v1_power_cycle': True,
    'proxy_started_without_v1_power_cycle': True,
    'unexpected_v1_disconnects': 0,
    'unexpected_resets': 0,
    'hil_fault_control_active': is_fault,
}
if os.environ.get('FAKE_BSC04_EARLY_CONNECTION') == '1':
    facts['late_connection_delay_ms'] = 29999
if os.environ.get('FAKE_BSC04_BAD_DEADLINE') == '1':
    facts['settle_exit_elapsed_ms'] = 9999
if os.environ.get('FAKE_BSC04_DISCONNECT') == '1':
    facts['unexpected_v1_disconnects'] = 1
if os.environ.get('FAKE_BSC04_BAD_DOWNSTREAM') == '1':
    facts['proxy_started_without_v1_power_cycle'] = False

if is_fault:
    lifecycle = [{
        'id': event_id,
        'sequence': sequence,
        'elapsed_ms': 34000 + sequence,
        'arm_sequence': 7,
        'ready_sequence': 3,
        'generation': 1,
        'phase': 1,
        'coordinator_state': 'V1_SETTLING',
        'v1_connected': True,
        'raw_verify_push_edge': True,
        'forwarded_verify_push_edge': event_id == 'ready',
    } for sequence, event_id in enumerate(('ready', 'fired', 'released'), start=1)]
    if os.environ.get('FAKE_BSC04_BAD_LIFECYCLE') == '1':
        lifecycle[1]['coordinator_state'] = 'STEADY'
    environment = 'waveshare-349-hil'
    hil_active = True
else:
    lifecycle = []
    if os.environ.get('FAKE_BSC04_REPLAY_FAULT') == '1':
        lifecycle = [{'id': 'fired'}]
    environment = 'waveshare-349'
    hil_active = False

if os.environ.get('FAKE_BSC04_WRONG_FIRMWARE') == '1':
    environment = 'waveshare-349' if is_fault else 'waveshare-349-hil'
if os.environ.get('FAKE_BSC04_WRONG_HIL') == '1':
    hil_active = not hil_active

payload = {
    'schema_version': 1,
    'case_id': argument('--case'),
    'role': role,
    'session_id': argument('--session-id'),
    'attempt_id': argument('--attempt-id'),
    'target_sha': argument('--target-sha'),
    'dut_alias': argument('--dut-alias'),
    'rig_alias': argument('--rig-alias'),
    'execution_mode': 'simulated',
    'hardware_observed': False,
    'started_at_utc': started.isoformat(timespec='seconds').replace('+00:00', 'Z'),
    'completed_at_utc': now.isoformat(timespec='seconds').replace('+00:00', 'Z'),
    'firmware': {
        'environment': environment,
        'target_sha': argument('--target-sha'),
        'binary_sha256': digest(f'{role}-binary'),
        'hil_fault_control_active': hil_active,
    },
    'events': events,
    'facts': facts,
    'fault_lifecycle': lifecycle,
    'capture_commitments': {
        'build_evidence_sha256': digest(f'{role}-build-evidence'),
        'coordinator_timeline_sha256': digest(f'{role}-coordinator-timeline'),
        'perf_csv_sha256': digest(f'{role}-perf-csv'),
        'serial_log_sha256': digest(f'{role}-serial-log'),
        'v1_exchange_sha256': digest(f'{role}-v1-exchange'),
    },
}
if os.environ.get('FAKE_BSC04_REUSE') == '1':
    payload['capture_commitments']['serial_log_sha256'] = payload['capture_commitments']['perf_csv_sha256']
if os.environ.get('FAKE_BSC04_BOOL_SCHEMA') == '1':
    payload['schema_version'] = True
if os.environ.get('FAKE_BSC04_INTEGER_HARDWARE') == '1':
    payload['hardware_observed'] = 0
payload['evidence_binding_sha256'] = commitment(payload)
if os.environ.get('FAKE_BSC04_TAMPER') == '1':
    payload['capture_commitments']['serial_log_sha256'] = digest('tampered-serial')
sys.stdout.write(json.dumps(payload))
""",
    )
    bsc05_adapter = root / "fake-bsc05-adapter.py"
    write_executable(
        bsc05_adapter,
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
    return hashlib.sha256(b'v1simple.bsc05.case-record.v1\\0' + canonical.encode('utf-8')).hexdigest()

role_id = argument('--role-id')
production_replay = role_id == 'alert-generation-production-replay'
is_fault = not production_replay
run_index = int(argument('--run-index'))
mutation = os.environ.get('FAKE_BSC05_MUTATION', '')
case_descriptor = json.loads(argument('--case-descriptor-json'))
role_descriptor = case_descriptor['production_replay' if production_replay else 'scenario']
if mutation == 'descriptor':
    case_descriptor['required_dut_capabilities'].append('invented-capability')
if mutation == 'wrong-fault-id':
    role_descriptor['fault_ids'] = ['substituted-notification-fault']
if mutation == 'missing-fault-id':
    role_descriptor['fault_ids'] = []
if mutation == 'replay-fault-id':
    role_descriptor['fault_ids'] = ['v1-notification-delay-once']

elapsed = [100, 200, 500] if production_replay else [100, 200, 400, 500, 700]
stimuli = [
    {'id': stimulus_id, 'sequence': sequence, 'elapsed_ms': elapsed[sequence - 1]}
    for sequence, stimulus_id in enumerate(role_descriptor['stimulus_ids'], start=1)
]
if mutation == 'stimulus-order':
    stimuli[0]['id'], stimuli[1]['id'] = stimuli[1]['id'], stimuli[0]['id']

old_generation = run_index * 10 + 1
new_generation = run_index * 10 + 2
if mutation == 'reuse-generations':
    old_generation = 11
    new_generation = 12
timeline = {
    'old_generation': old_generation,
    'new_generation': new_generation,
    'fragment_started_elapsed_ms': 100,
    'disconnect_elapsed_ms': 200,
    'old_session_closed_elapsed_ms': 250,
    'new_session_opened_elapsed_ms': 350,
    'fresh_alert_elapsed_ms': 500 if production_replay else 700,
    'fresh_alert_rendered_elapsed_ms': 550 if production_replay else 750,
    'fresh_alert_persisted_elapsed_ms': 560 if production_replay else 760,
    'fresh_alert_faded_elapsed_ms': 700 if production_replay else 900,
    'unexpected_generation_admissions': 0,
}
if not production_replay:
    timeline.update({
        'old_callback_release_elapsed_ms': 400,
        'old_callback_rejected_elapsed_ms': 450,
        'display_only_packet_elapsed_ms': 500,
        'logical_display_idle_elapsed_ms': 550,
        'physical_display_idle_elapsed_ms': 560,
    })
if mutation == 'timeline-order':
    timeline['old_session_closed_elapsed_ms'] = 800
if mutation == 'generation':
    timeline['new_generation'] = old_generation

barriers = []
if not production_replay:
    barriers = [{
        'id': 'old-callback-held',
        'sequence': 1,
        'ready_elapsed_ms': 150,
        'released_elapsed_ms': 425,
        'old_generation': old_generation,
        'new_generation': timeline['new_generation'],
        'timed_out': mutation == 'barrier-timeout',
    }]
elif mutation == 'replay-barrier':
    barriers = [{'id': 'old-callback-held'}]

fault_lifecycle = []
if is_fault:
    lifecycle_times = (150, 160, 425)
    lifecycle_reasons = (
        'notification_copied_without_callback_pointer',
        'old_generation_notification_delayed',
        'new_session_rejected_old_generation_copy',
    )
    fault_lifecycle = [
        {
            'id': event_id,
            'sequence': sequence,
            'elapsed_ms': lifecycle_times[sequence - 1],
            'reason': lifecycle_reasons[sequence - 1],
            'case_id': 'BSC-05',
            'fault_id': 'v1-notification-delay-once',
            'arm_sequence': 7,
            'ready_sequence': 3,
            'generation': old_generation,
            'phase': 1,
            'old_generation': old_generation,
            'new_generation': new_generation if event_id == 'released' else 0,
            'characteristic_class': 'display',
            'old_session_closed_elapsed_ms': 250 if event_id == 'released' else 0,
            'new_session_opened_elapsed_ms': 350 if event_id == 'released' else 0,
            'wrong_generation_rejected': event_id == 'released',
        }
        for sequence, event_id in enumerate(
            ('ready', 'fired', 'released'), start=1
        )
    ]
    if mutation == 'missing-hil-event':
        fault_lifecycle.pop(1)
    if mutation == 'reordered-hil-events':
        fault_lifecycle[0], fault_lifecycle[1] = (
            fault_lifecycle[1], fault_lifecycle[0]
        )
    if mutation == 'substituted-hil-event':
        fault_lifecycle[1]['fault_id'] = 'substituted-notification-fault'
    if mutation == 'hil-arm':
        fault_lifecycle[1]['arm_sequence'] = 8
    if mutation == 'hil-generation':
        fault_lifecycle[1]['generation'] = new_generation
    if mutation == 'hil-timing':
        fault_lifecycle[2]['elapsed_ms'] = 300
elif mutation == 'replay-hil-event':
    fault_lifecycle = [{'id': 'ready'}]

if mutation == 'shifted-beyond-duration':
    shift_ms = 60000
    for stimulus in stimuli:
        stimulus['elapsed_ms'] += shift_ms
    for field in tuple(timeline):
        if field.endswith('_elapsed_ms'):
            timeline[field] += shift_ms
    for barrier in barriers:
        barrier['ready_elapsed_ms'] += shift_ms
        barrier['released_elapsed_ms'] += shift_ms
    for event in fault_lifecycle:
        event['elapsed_ms'] += shift_ms
        if event['old_session_closed_elapsed_ms']:
            event['old_session_closed_elapsed_ms'] += shift_ms
        if event['new_session_opened_elapsed_ms']:
            event['new_session_opened_elapsed_ms'] += shift_ms
if mutation == 'negative-elapsed':
    stimuli[0]['elapsed_ms'] = -1
    timeline['fragment_started_elapsed_ms'] = -1

facts = {descriptor['id']: True for descriptor in role_descriptor['facts']}
if mutation == 'fact':
    facts[next(iter(facts))] = False
resets = {
    'expected_kind': 'none',
    'planned': False if mutation == 'bool-reset' else 0,
    'observed': 0,
    'unexpected': 1 if mutation == 'unexpected-reset' else 0,
}

now = datetime.now(timezone.utc)
started = now - timedelta(seconds=30)
capture_run = '' if mutation == 'reuse-captures' else f'-run-{run_index}'
capture_fields = (
    'build_evidence_sha256',
    'display_video_sha256',
    'framebuffer_sha256',
    'packet_generation_sha256',
    'serial_log_sha256',
)
binary_label = f'{role_id}-binary'
if mutation == 'mixed-firmware' and run_index == 2:
    binary_label += '-different'
environment = 'waveshare-349-hil' if is_fault else 'waveshare-349'
hil_active = is_fault
if mutation == 'fault-production-build':
    environment = 'waveshare-349'
if mutation == 'hil-inactive':
    hil_active = False
if mutation == 'replay-hil-build':
    environment = 'waveshare-349-hil'
    hil_active = True
payload = {
    'schema_version': True if mutation == 'bool-schema' else 1,
    'case_id': argument('--case'),
    'role_id': 'substituted-role' if mutation == 'role' else role_id,
    'session_id': argument('--session-id'),
    'attempt_id': argument('--attempt-id'),
    'run_index': run_index,
    'target_sha': argument('--target-sha'),
    'dut_alias': argument('--dut-alias'),
    'rig_alias': argument('--rig-alias'),
    'execution_mode': 'simulated',
    'hardware_observed': 0 if mutation == 'integer-hardware' else False,
    'started_at_utc': started.isoformat(timespec='seconds').replace('+00:00', 'Z'),
    'completed_at_utc': now.isoformat(timespec='seconds').replace('+00:00', 'Z'),
    'case_descriptor': case_descriptor,
    'case_descriptor_sha256': (
        '0' * 64 if mutation == 'descriptor-digest' else argument('--case-descriptor-sha256')
    ),
    'firmware': {
        'environment': environment,
        'target_sha': argument('--target-sha'),
        'binary_sha256': digest(binary_label),
        'hil_fault_control_active': hil_active,
        'build_kind': role_descriptor['build_kind'],
    },
    'stimuli': stimuli,
    'generation_timeline': timeline,
    'barriers': barriers,
    'fault_lifecycle': fault_lifecycle,
    'resets': resets,
    'facts': facts,
    'capture_commitments': {
        field: digest(f'{role_id}-{field}{capture_run}') for field in capture_fields
    },
}
if mutation == 'capture-role-reuse':
    payload['capture_commitments']['serial_log_sha256'] = payload['capture_commitments']['framebuffer_sha256']
payload['evidence_binding_sha256'] = commitment(payload)
if mutation == 'tamper':
    payload['generation_timeline']['fresh_alert_faded_elapsed_ms'] += 1
sys.stdout.write(json.dumps(payload))
""",
    )
    bsc06_adapter = root / "fake-bsc06-adapter.py"
    write_executable(
        bsc06_adapter,
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

def canonical_commitment(domain, payload):
    canonical = json.dumps(payload, ensure_ascii=True, separators=(',', ':'), sort_keys=True)
    return hashlib.sha256(domain.encode('ascii') + b'\\0' + canonical.encode('utf-8')).hexdigest()

def record_commitment(payload):
    return canonical_commitment('v1simple.bsc06.case-record.v1', payload)

fault_role = {
    'role_id': 'obd-transport-race-fault',
    'schema': 'case-observation-v1',
    'build_kind': 'hil-fault',
    'stimulus_ids': [
        'start-obd-polling', 'race-proxy-attach', 'race-adapter-loss',
        'race-forget-device', 'race-shutdown', 'restore-obd-peer',
    ],
    'fault_ids': ['transport-ready-barrier'],
    'barrier_ids': ['connect-window-ready', 'cancel-window-ready'],
    'vbus_isolation_required': False,
    'reset_contract': {'expected_kind': 'none', 'expected_count': 0, 'unexpected_count': 0},
    'facts': [
        {'id': 'post-cancel-gatt-observed', 'type': 'boolean', 'expected': False},
        {'id': 'link-down-before-handle-retire', 'type': 'boolean', 'expected': True},
        {'id': 'negative-ack-delay-ms', 'type': 'integer', 'minimum': 0, 'maximum': 5000},
        {'id': 'clean-reconnect-count', 'type': 'integer', 'minimum': 1, 'maximum': 1},
        {'id': 'heap-corruption-observed', 'type': 'boolean', 'expected': False},
        {'id': 'barrier-generation-matched', 'type': 'boolean', 'expected': True},
    ],
}
production_role = {
    'role_id': 'obd-transport-production-replay',
    'schema': 'case-observation-v1',
    'build_kind': 'production',
    'stimulus_ids': [
        'start-obd-polling', 'race-proxy-attach', 'race-adapter-loss', 'restore-obd-peer',
    ],
    'fault_ids': [],
    'barrier_ids': [],
    'vbus_isolation_required': False,
    'reset_contract': {'expected_kind': 'none', 'expected_count': 0, 'unexpected_count': 0},
    'facts': [
        {'id': 'transport-ownership-preserved', 'type': 'boolean', 'expected': True},
        {'id': 'clean-reconnect-succeeded', 'type': 'boolean', 'expected': True},
        {'id': 'hil-fault-control-active', 'type': 'boolean', 'expected': False},
    ],
}
case_descriptor = {
    'id': 'BSC-06',
    'minimum_runs': 3,
    'fault_build_required': True,
    'production_replay_required': True,
    'required_dut_capabilities': [
        'firmware-execution', 'obd-connectivity', 'proxy-connectivity', 'serial',
    ],
    'required_rig_capabilities': [
        'artifact-capture', 'obd-peer', 'proxy-client', 'utc-time-source', 'v1-peer',
    ],
    'scenario': fault_role,
    'production_replay': production_role,
}
role_id = argument('--role-id')
is_fault = role_id == fault_role['role_id']
descriptor = dict(fault_role if is_fault else production_role)
run_index = int(argument('--run-index'))
mutation = os.environ.get('FAKE_BSC06_MUTATION', '')
now = datetime.now(timezone.utc)
started = now - timedelta(seconds=30)
stimuli = [
    {'id': event_id, 'sequence': sequence, 'elapsed_ms': sequence * 100, 'result': 'pass'}
    for sequence, event_id in enumerate(descriptor['stimulus_ids'], start=1)
]
if mutation == 'stimulus-order':
    stimuli[1], stimuli[2] = stimuli[2], stimuli[1]
if mutation == 'stimulus-timing':
    stimuli[2]['elapsed_ms'] = stimuli[1]['elapsed_ms']

race_specs = [
    ('proxy-attach', 'race-proxy-attach', 'cancellation'),
    ('adapter-loss', 'race-adapter-loss', 'link-down'),
    ('forget-device', 'race-forget-device', 'cancellation'),
    ('shutdown', 'race-shutdown', 'cancellation'),
]
if not is_fault:
    race_specs = race_specs[:2]
windows = []
for sequence, (race_id, stimulus_id, release_cause) in enumerate(race_specs, start=1):
    generation_run = 1 if mutation == 'reuse-generations' else run_index
    generation = generation_run * 100 + sequence
    start_ms = descriptor['stimulus_ids'].index(stimulus_id) * 100 + 100
    cancel_ms = start_ms + 10
    completion_ms = start_ms + 40
    epoch_before = run_index * 100 + sequence
    epoch_after = epoch_before if release_cause == 'link-down' else epoch_before + 1
    hil_events = []
    if is_fault:
        device_ready_ms = run_index * 100000 + sequence * 100
        common_event = {
            'case_id': 'BSC-06',
            'fault_id': 'obd-transport-operation-barrier-once',
            'arm_sequence': run_index * 10 + sequence,
            'ready_sequence': run_index * 10 + sequence,
            'generation': generation,
            'phase': 1,
            'request_id': run_index * 1000 + sequence,
            'dispatch_epoch': epoch_before,
            'operation': 'write',
            'runtime_state': 'polling',
            'ready_timestamp_ms': device_ready_ms,
        }
        hil_events = [
            {
                **common_event,
                'hil_event': 'ready',
                'reason': 'polling_write_after_epoch_claim',
                'sequence': 1,
                'elapsed_ms': start_ms + 1,
            },
            {
                **common_event,
                'hil_event': 'fired',
                'reason': 'transport_owner_barrier_active',
                'sequence': 2,
                'elapsed_ms': start_ms + 2,
            },
            {
                **common_event,
                'hil_event': 'released',
                'reason': (
                    'matching_link_down_suppressed_write'
                    if release_cause == 'link-down'
                    else 'newer_cancellation_epoch_suppressed_write'
                ),
                'sequence': 3,
                'elapsed_ms': cancel_ms + 1,
                'cancellation_epoch': epoch_after,
                'link_down_generation': generation if release_cause == 'link-down' else 0,
                'completion_timestamp_ms': device_ready_ms + 1,
                'operation_suppressed': True,
                'controller_release_recorded': True,
            },
        ]
    barriers = []
    if is_fault:
        barriers = [
            {
                'id': 'connect-window-ready',
                'sequence': 1,
                'ready_elapsed_ms': start_ms + 1,
                'released_elapsed_ms': start_ms + 2,
            },
            {
                'id': 'cancel-window-ready',
                'sequence': 2,
                'ready_elapsed_ms': start_ms + 2,
                'released_elapsed_ms': cancel_ms + 1,
            },
        ]
    window = {
        'race_id': race_id,
        'sequence': sequence,
        'stimulus_id': stimulus_id,
        'release_cause': release_cause,
        'start_elapsed_ms': start_ms,
        'cancellation_elapsed_ms': cancel_ms,
        'last_gatt_operation_elapsed_ms': start_ms + 5,
        'negative_ack_elapsed_ms': start_ms + 20,
        'link_down_elapsed_ms': start_ms + 25,
        'handle_retired_elapsed_ms': start_ms + 30,
        'completion_elapsed_ms': completion_ms,
        'captured_generation': generation,
        'cancellation_epoch_before': epoch_before,
        'cancellation_epoch_after': epoch_after,
        'callback_link_down_generation': generation,
        'callback_confirmed_link_down': True,
        'post_cancel_gatt_observed': False,
        'negative_ack_delay_ms': 10,
        'reconnect_count': 1,
        'heap_corruption_observed': False,
        'barrier_ready': is_fault,
        'barrier_released': is_fault,
        'barriers': barriers,
        'hil_events': hil_events,
    }
    windows.append(window)

if mutation == 'role':
    role_id = 'substituted-role'
if mutation == 'descriptor':
    descriptor['role_id'] = 'substituted-role'
if mutation == 'window-order':
    windows[0], windows[1] = windows[1], windows[0]
if mutation == 'wrong-generation':
    windows[0]['callback_link_down_generation'] += 1
if mutation == 'post-cancel-operation':
    windows[0]['last_gatt_operation_elapsed_ms'] = windows[0]['cancellation_elapsed_ms'] + 1
    windows[0]['post_cancel_gatt_observed'] = True
if mutation == 'negative-last-gatt':
    windows[0]['last_gatt_operation_elapsed_ms'] = -1
if mutation == 'last-gatt-before-window':
    windows[0]['last_gatt_operation_elapsed_ms'] = windows[0]['start_elapsed_ms'] - 1
if mutation == 'overlapping-windows':
    windows[0]['completion_elapsed_ms'] = windows[1]['start_elapsed_ms']
if mutation == 'reconnect-cardinality':
    windows[0]['reconnect_count'] = 2
if mutation == 'barrier-missing':
    windows[0]['barrier_released'] = False
if mutation == 'barrier-order' and is_fault:
    windows[0]['barriers'][0], windows[0]['barriers'][1] = (
        windows[0]['barriers'][1], windows[0]['barriers'][0]
    )
if mutation == 'barrier-timing' and is_fault:
    windows[0]['barriers'][1]['released_elapsed_ms'] = windows[0]['completion_elapsed_ms'] + 1
if mutation == 'fault-identity' and is_fault:
    windows[0]['hil_events'][0]['fault_id'] = 'substituted-fault'
if mutation == 'fault-logical-id' and is_fault:
    windows[0]['hil_events'][0]['fault_id'] = 'transport-ready-barrier'
if mutation == 'fault-reason' and is_fault:
    windows[0]['hil_events'][0]['reason'] = 'invented-ready-reason'
if mutation == 'fault-completion-schema' and is_fault:
    windows[0]['hil_events'][0]['cancellation_epoch'] = windows[0]['cancellation_epoch_before']
if mutation == 'fault-completion-timestamp' and is_fault:
    windows[0]['hil_events'][2]['completion_timestamp_ms'] += 1001
if mutation == 'fault-order' and is_fault:
    windows[0]['hil_events'][1], windows[0]['hil_events'][2] = (
        windows[0]['hil_events'][2], windows[0]['hil_events'][1]
    )
if mutation == 'fault-timing' and is_fault:
    windows[0]['hil_events'][2]['elapsed_ms'] = windows[0]['completion_elapsed_ms'] + 1
if mutation == 'cancellation-epoch-unchanged' and is_fault:
    windows[0]['cancellation_epoch_after'] = windows[0]['cancellation_epoch_before']
    windows[0]['hil_events'][2]['cancellation_epoch'] = windows[0]['cancellation_epoch_before']
if mutation == 'cancellation-link-generation' and is_fault:
    windows[0]['hil_events'][2]['link_down_generation'] = windows[0]['captured_generation']
if mutation == 'link-down-epoch-advanced' and is_fault:
    windows[1]['cancellation_epoch_after'] += 1
    windows[1]['hil_events'][2]['cancellation_epoch'] = windows[1]['cancellation_epoch_after']
if mutation == 'link-down-generation-zero' and is_fault:
    windows[1]['hil_events'][2]['link_down_generation'] = 0
if mutation.startswith('event-operation-bool-') and is_fault:
    value = {'zero': 0, 'one': 1, 'string': 'true'}[mutation.rsplit('-', 1)[1]]
    windows[0]['hil_events'][2]['operation_suppressed'] = value
if mutation.startswith('event-controller-bool-') and is_fault:
    value = {'zero': 0, 'one': 1, 'string': 'true'}[mutation.rsplit('-', 1)[1]]
    windows[0]['hil_events'][2]['controller_release_recorded'] = value
if mutation == 'production-hil' and not is_fault:
    windows[0]['barrier_ready'] = True
    windows[0]['hil_events'] = [{'id': 'fired'}]

facts = (
    {
        'post-cancel-gatt-observed': False,
        'link-down-before-handle-retire': True,
        'negative-ack-delay-ms': 10,
        'clean-reconnect-count': 1,
        'heap-corruption-observed': False,
        'barrier-generation-matched': True,
    }
    if is_fault
    else {
        'transport-ownership-preserved': True,
        'clean-reconnect-succeeded': True,
        'hil-fault-control-active': False,
    }
)
if mutation == 'aggregate-reconnect':
    facts['clean-reconnect-count' if is_fault else 'clean-reconnect-succeeded'] = 2 if is_fault else False

environment = 'waveshare-349-hil' if is_fault else 'waveshare-349'
build_kind = 'hil-fault' if is_fault else 'production'
hil_active = is_fault
if mutation == 'wrong-firmware':
    environment = 'waveshare-349' if is_fault else 'waveshare-349-hil'
if mutation == 'wrong-build-role':
    build_kind = 'production' if is_fault else 'hil-fault'
if mutation == 'wrong-hil':
    hil_active = not hil_active
if mutation.startswith('firmware-hil-bool-'):
    hil_active = {'zero': 0, 'one': 1, 'string': 'true'}[mutation.rsplit('-', 1)[1]]
capture_run = '' if mutation == 'stale-run-capture-binding' else f'-run-{run_index}'
capture_fields = (
    'build_evidence_sha256', 'control_queue_timeline_sha256',
    'obd_race_timeline_sha256', 'panic_summary_sha256', 'serial_log_sha256',
)
payload = {
    'schema_version': 1,
    'case_id': argument('--case'),
    'role_id': role_id,
    'session_id': argument('--session-id'),
    'attempt_id': argument('--attempt-id'),
    'run_index': run_index,
    'target_sha': argument('--target-sha'),
    'dut_alias': argument('--dut-alias'),
    'rig_alias': argument('--rig-alias'),
    'execution_mode': 'simulated',
    'hardware_observed': False,
    'started_at_utc': started.isoformat(timespec='seconds').replace('+00:00', 'Z'),
    'completed_at_utc': now.isoformat(timespec='seconds').replace('+00:00', 'Z'),
    'case_descriptor_sha256': canonical_commitment('v1simple.bsc06.case-descriptor.v1', case_descriptor),
    'descriptor': descriptor,
    'firmware': {
        'environment': environment,
        'build_kind': build_kind,
        'target_sha': argument('--target-sha'),
        'binary_sha256': digest(f'{role_id}-binary'),
        'hil_fault_control_active': hil_active,
    },
    'stimuli': stimuli,
    'race_windows': windows,
    'reset_observation': {
        'expected_kind': descriptor['reset_contract']['expected_kind'],
        'expected_count': descriptor['reset_contract']['expected_count'],
        'unexpected_count': descriptor['reset_contract']['unexpected_count'],
        'panic_observed': False,
        'watchdog_reset_observed': False,
        'load_prohibited_observed': False,
    },
    'facts': facts,
    'capture_commitments': {
        field: digest(f'{role_id}-{field}{capture_run}') for field in capture_fields
    },
}
if mutation == 'descriptor-digest':
    payload['case_descriptor_sha256'] = 'f' * 64
if mutation == 'role-capture-reuse':
    payload['capture_commitments']['serial_log_sha256'] = payload['capture_commitments']['panic_summary_sha256']
if mutation == 'utc-duration':
    payload['completed_at_utc'] = payload['started_at_utc']
if mutation == 'reset-expected':
    payload['reset_observation']['expected_count'] = 1
if mutation == 'reset-unexpected':
    payload['reset_observation']['unexpected_count'] = 1
if mutation == 'reset-panic':
    payload['reset_observation']['panic_observed'] = True
if mutation == 'reset-watchdog':
    payload['reset_observation']['watchdog_reset_observed'] = True
if mutation == 'reset-load-prohibited':
    payload['reset_observation']['load_prohibited_observed'] = True
if mutation == 'reset-expected-bool':
    payload['reset_observation']['expected_count'] = False
if mutation == 'reset-unexpected-string':
    payload['reset_observation']['unexpected_count'] = '0'
if mutation == 'reset-panic-int':
    payload['reset_observation']['panic_observed'] = 0
if mutation == 'reset-watchdog-int':
    payload['reset_observation']['watchdog_reset_observed'] = 0
if mutation == 'reset-load-prohibited-int':
    payload['reset_observation']['load_prohibited_observed'] = 0
if mutation == 'bool-schema':
    payload['schema_version'] = True
if mutation == 'integer-hardware':
    payload['hardware_observed'] = 0
payload['evidence_binding_sha256'] = record_commitment(payload)
if mutation == 'stale-evidence-binding':
    payload['capture_commitments']['serial_log_sha256'] = digest('tampered')
sys.stdout.write(json.dumps(payload))
""",
    )

    bsc13_adapter = root / "fake-bsc13-adapter.py"
    write_executable(
        bsc13_adapter,
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
    return hashlib.sha256(b'v1simple.bsc13.case-record.v1\\0' + canonical.encode('utf-8')).hexdigest()

role = argument('--role')
run_index = int(argument('--run-index'))
is_fault = role == 'fault-collection'
now = datetime.now(timezone.utc)
started = now - timedelta(seconds=30)
stimulus_ids = [
    'begin-obd-connect',
    'preempt-with-proxy',
    'preempt-with-disable',
    'remove-preemption',
]
if is_fault:
    facts_descriptors = [
        {'id': 'unowned-link-disconnected', 'type': 'boolean', 'expected': True},
        {'id': 'callback-confirmed-link-down', 'type': 'boolean', 'expected': True},
        {'id': 'coordinator-reached-idle', 'type': 'boolean', 'expected': True},
        {'id': 'phantom-connected-status-observed', 'type': 'boolean', 'expected': False},
        {'id': 'resume-scan-count', 'type': 'integer', 'minimum': 1, 'maximum': 1},
        {'id': 'successful-reconnect-count', 'type': 'integer', 'minimum': 1, 'maximum': 1},
        {'id': 'barrier-generation-matched', 'type': 'boolean', 'expected': True},
    ]
    descriptor = {
        'role_id': 'obd-connect-edge-preemption-fault',
        'schema': 'case-observation-v1',
        'build_kind': 'hil-fault',
        'stimulus_ids': stimulus_ids,
        'fault_ids': ['obd-connect-edge-barrier'],
        'barrier_ids': ['physical-link-before-session', 'preemption-release'],
        'vbus_isolation_required': False,
        'reset_contract': {'expected_kind': 'none', 'expected_count': 0, 'unexpected_count': 0},
        'facts': facts_descriptors,
    }
    facts = {
        'unowned-link-disconnected': True,
        'callback-confirmed-link-down': True,
        'coordinator-reached-idle': True,
        'phantom-connected-status-observed': False,
        'resume-scan-count': 1,
        'successful-reconnect-count': 1,
        'barrier-generation-matched': True,
    }
    environment = 'waveshare-349-hil'
    hil_active = True
else:
    facts_descriptors = [
        {'id': 'orphan-link-observed', 'type': 'boolean', 'expected': False},
        {'id': 'phantom-connected-status-observed', 'type': 'boolean', 'expected': False},
        {'id': 'single-reconnect-succeeded', 'type': 'boolean', 'expected': True},
        {'id': 'hil-fault-control-active', 'type': 'boolean', 'expected': False},
    ]
    descriptor = {
        'role_id': 'obd-connect-edge-production-replay',
        'schema': 'case-observation-v1',
        'build_kind': 'production',
        'stimulus_ids': stimulus_ids,
        'fault_ids': [],
        'barrier_ids': [],
        'vbus_isolation_required': False,
        'reset_contract': {'expected_kind': 'none', 'expected_count': 0, 'unexpected_count': 0},
        'facts': facts_descriptors,
    }
    facts = {
        'orphan-link-observed': False,
        'phantom-connected-status-observed': False,
        'single-reconnect-succeeded': True,
        'hil-fault-control-active': False,
    }
    environment = 'waveshare-349'
    hil_active = False

if os.environ.get('FAKE_BSC13_WRONG_DESCRIPTOR') == '1':
    descriptor['role_id'] = 'substituted-role'
if os.environ.get('FAKE_BSC13_WRONG_FIRMWARE') == '1':
    environment = 'waveshare-349' if is_fault else 'waveshare-349-hil'
if os.environ.get('FAKE_BSC13_WRONG_HIL') == '1':
    hil_active = not hil_active
if os.environ.get('FAKE_BSC13_PHANTOM') == '1':
    facts['phantom-connected-status-observed'] = True

windows = []
for sequence, critical_role in enumerate(('proxy-takeover', 'qualification-disable'), start=1):
    generation_run = 1 if os.environ.get('FAKE_BSC13_REUSE_GENERATIONS') == '1' else run_index
    generation = generation_run * 10 + sequence
    start_ms = sequence * 1000
    completion_ms = start_ms + 300
    hil_events = []
    if is_fault:
        hil_events = [
            {
                'id': event_id,
                'sequence': event_sequence,
                'elapsed_ms': start_ms + event_sequence * 50,
                'arm_sequence': run_index * 10 + sequence,
                'ready_sequence': run_index * 10 + sequence,
                'generation': generation,
                'phase': 1,
            }
            for event_sequence, event_id in enumerate(('ready', 'fired', 'released'), start=1)
        ]
    window = {
        'role': critical_role,
        'sequence': sequence,
        'start_elapsed_ms': start_ms,
        'completion_elapsed_ms': completion_ms,
        'captured_generation': generation,
        'cancellation_epoch_before': run_index * 10 + sequence,
        'cancellation_epoch_after': run_index * 10 + sequence + 1,
        'callback_link_down_generation': generation,
        'callback_confirmed_link_down': True,
        'session_ownership_adopted': False,
        'barrier_ready': is_fault,
        'barrier_released': is_fault,
        'coordinator_reached_idle': True,
        'coordinator_idle_elapsed_ms': 200,
        'phantom_connected_status_observed': False,
        'resume_scan_count': 1,
        'successful_reconnect_count': 1,
        'hil_events': hil_events,
    }
    windows.append(window)

if os.environ.get('FAKE_BSC13_MISSING_WINDOW') == '1':
    windows.pop()
if os.environ.get('FAKE_BSC13_WRONG_WINDOW_ROLE') == '1':
    windows[1]['role'] = 'proxy-takeover'
if os.environ.get('FAKE_BSC13_BAD_GENERATION') == '1':
    windows[0]['callback_link_down_generation'] += 1
if os.environ.get('FAKE_BSC13_PHANTOM') == '1':
    windows[0]['phantom_connected_status_observed'] = True
if os.environ.get('FAKE_BSC13_BAD_RESUME') == '1':
    windows[0]['resume_scan_count'] = 2
if os.environ.get('FAKE_BSC13_MISSING_RELEASE') == '1' and is_fault:
    windows[0]['barrier_released'] = False
if os.environ.get('FAKE_BSC13_REPLAY_HIL') == '1' and not is_fault:
    windows[0]['hil_events'] = [{'id': 'fired'}]

capture_fields = (
    'build_evidence_sha256',
    'coordinator_timeline_sha256',
    'obd_exchange_sha256',
    'proxy_exchange_sha256',
    'serial_log_sha256',
)
capture_run = '' if os.environ.get('FAKE_BSC13_REUSE_CAPTURES') == '1' else f'-run-{run_index}'
payload = {
    'schema_version': 1,
    'case_id': argument('--case'),
    'role': role,
    'session_id': argument('--session-id'),
    'attempt_id': argument('--attempt-id'),
    'run_index': run_index,
    'target_sha': argument('--target-sha'),
    'dut_alias': argument('--dut-alias'),
    'rig_alias': argument('--rig-alias'),
    'execution_mode': 'simulated',
    'hardware_observed': False,
    'started_at_utc': started.isoformat(timespec='seconds').replace('+00:00', 'Z'),
    'completed_at_utc': now.isoformat(timespec='seconds').replace('+00:00', 'Z'),
    'descriptor': descriptor,
    'firmware': {
        'environment': environment,
        'target_sha': argument('--target-sha'),
        'binary_sha256': digest(f'{role}-binary'),
        'hil_fault_control_active': hil_active,
    },
    'critical_windows': windows,
    'facts': facts,
    'capture_commitments': {
        field: digest(f'{role}-{field}{capture_run}') for field in capture_fields
    },
}
if os.environ.get('FAKE_BSC13_BOOL_SCHEMA') == '1':
    payload['schema_version'] = True
if os.environ.get('FAKE_BSC13_INTEGER_HARDWARE') == '1':
    payload['hardware_observed'] = 0
payload['evidence_binding_sha256'] = commitment(payload)
if os.environ.get('FAKE_BSC13_TAMPER') == '1':
    payload['capture_commitments']['serial_log_sha256'] = digest('tampered-serial')
sys.stdout.write(json.dumps(payload))
""",
    )
    bsc07_adapter = root / "fake-bsc07-adapter.py"
    write_executable(
        bsc07_adapter,
        """#!/usr/bin/env python3
from datetime import datetime, timedelta, timezone
import hashlib
import json
import os
from pathlib import Path
import sys

def argument(name):
    return sys.argv[sys.argv.index(name) + 1]

def digest(value):
    return hashlib.sha256(value.encode('utf-8')).hexdigest()

def commitment(payload):
    canonical = json.dumps(payload, ensure_ascii=True, separators=(',', ':'), sort_keys=True)
    return hashlib.sha256(b'v1simple.bsc07.case-record.v1\\0' + canonical.encode('utf-8')).hexdigest()

mutation = os.environ.get('FAKE_BSC07_MUTATION', '')
now = datetime.now(timezone.utc)
started = now - timedelta(seconds=20)
case_descriptor = {
    'id': 'BSC-07',
    'minimum_runs': 1,
    'fault_build_required': False,
    'production_replay_required': False,
    'required_dut_capabilities': [
        'battery-monitor', 'firmware-execution', 'maintenance-mode', 'power-button', 'serial',
    ],
    'required_rig_capabilities': [
        'ap-traffic', 'artifact-capture', 'power-control', 'utc-time-source', 'vbus-isolation',
    ],
    'scenario': {
        'role_id': 'maintenance-power-safety',
        'schema': 'case-observation-v1',
        'build_kind': 'production',
        'stimulus_ids': [
            'maintenance-boot', 'apply-ap-load', 'change-battery-voltage',
            'hold-power-button', 'apply-critical-voltage',
        ],
        'fault_ids': [],
        'barrier_ids': ['critical-voltage-grace'],
        'vbus_isolation_required': True,
        'reset_contract': {
            'expected_kind': 'intentional-shutdown', 'expected_count': 1, 'unexpected_count': 0,
        },
        'facts': [
            {'id': 'voltage-refresh-delay-ms', 'type': 'integer', 'minimum': 0, 'maximum': 10000},
            {'id': 'power-button-handled-under-ap-load', 'type': 'boolean', 'expected': True},
            {'id': 'critical-shutdown-grace-ms', 'type': 'integer', 'minimum': 4500, 'maximum': 6500},
            {'id': 'critical-warning-observed', 'type': 'boolean', 'expected': True},
            {'id': 'ui-responsive-until-shutdown', 'type': 'boolean', 'expected': True},
            {'id': 'loop-stall-observed', 'type': 'boolean', 'expected': False},
        ],
    },
    'production_replay': None,
}
stimuli = [
    {'id': stimulus_id, 'sequence': sequence, 'elapsed_ms': elapsed_ms, 'result': 'pass'}
    for sequence, (stimulus_id, elapsed_ms) in enumerate(zip(
        case_descriptor['scenario']['stimulus_ids'], (1000, 2000, 3000, 4000, 7000)
    ), start=1)
]
observations = {
    'voltage_refresh': {'changed_elapsed_ms': 3000, 'refreshed_elapsed_ms': 3500},
    'ap_traffic': {
        'started_elapsed_ms': 2000,
        'last_success_before_hold_elapsed_ms': 3900,
        'first_success_after_hold_elapsed_ms': 6100,
        'continuous': True,
    },
    'power_button': {
        'hold_started_elapsed_ms': 4000,
        'hold_released_elapsed_ms': 6000,
        'handled_elapsed_ms': 6000,
    },
    'critical_shutdown': {
        'applied_elapsed_ms': 7000,
        'warning_elapsed_ms': 7500,
        'shutdown_elapsed_ms': 12500,
    },
    'health': {
        'ui_last_healthy_elapsed_ms': 12000,
        'battery_last_healthy_elapsed_ms': 12000,
        'loop_last_healthy_elapsed_ms': 12000,
        'watchdog_last_healthy_elapsed_ms': 12000,
    },
    'power': {
        'vbus_isolated': True,
        'external_power_removed': True,
        'power_removed_elapsed_ms': 12500,
    },
}
barriers = [{
    'id': 'critical-voltage-grace',
    'sequence': 1,
    'ready_elapsed_ms': 7500,
    'released_elapsed_ms': 12500,
    'timed_out': False,
}]
reset_observation = {
    'expected_kind': 'intentional-shutdown',
    'planned_count': 1,
    'observed_count': 1,
    'unexpected_count': 0,
    'panic_observed': False,
    'watchdog_reset_observed': False,
    'shutdown_elapsed_ms': 12500,
    'observed_elapsed_ms': 12500,
    'reason': 'intentional-critical-voltage-shutdown',
}
facts = {
    'voltage-refresh-delay-ms': 500,
    'power-button-handled-under-ap-load': True,
    'critical-shutdown-grace-ms': 5000,
    'critical-warning-observed': True,
    'ui-responsive-until-shutdown': True,
    'loop-stall-observed': False,
}
environment = 'waveshare-349'
build_kind = 'production'
hil_active = False

if mutation == 'descriptor':
    case_descriptor['required_dut_capabilities'].append('invented-capability')
if mutation == 'stimulus-order':
    stimuli[1], stimuli[2] = stimuli[2], stimuli[1]
if mutation == 'stimulus-timing':
    stimuli[2]['elapsed_ms'] = stimuli[1]['elapsed_ms']
if mutation == 'stimulus-missing':
    stimuli.pop()
if mutation == 'voltage-slow':
    observations['voltage_refresh']['refreshed_elapsed_ms'] = 14000
if mutation == 'hold-short':
    observations['power_button']['hold_released_elapsed_ms'] = 5999
if mutation == 'traffic-not-continuous':
    observations['ap_traffic']['continuous'] = False
if mutation == 'traffic-int':
    observations['ap_traffic']['continuous'] = 1
if mutation == 'traffic-before-hold-missing':
    observations['ap_traffic']['last_success_before_hold_elapsed_ms'] = 1000
if mutation == 'traffic-after-hold-missing':
    observations['ap_traffic']['first_success_after_hold_elapsed_ms'] = 13000
if mutation == 'button-not-handled':
    facts['power-button-handled-under-ap-load'] = False
if mutation == 'grace-short':
    observations['critical_shutdown']['shutdown_elapsed_ms'] = 11999
    observations['power']['power_removed_elapsed_ms'] = 11999
    barriers[0]['released_elapsed_ms'] = 11999
    facts['critical-shutdown-grace-ms'] = 4499
if mutation == 'grace-long':
    observations['critical_shutdown']['shutdown_elapsed_ms'] = 14001
    observations['power']['power_removed_elapsed_ms'] = 14001
    barriers[0]['released_elapsed_ms'] = 14001
    facts['critical-shutdown-grace-ms'] = 6501
if mutation == 'warning-missing':
    facts['critical-warning-observed'] = False
if mutation == 'warning-int':
    facts['critical-warning-observed'] = 1
if mutation == 'health-stale':
    observations['health']['battery_last_healthy_elapsed_ms'] = 11000
if mutation == 'health-after-shutdown':
    observations['health']['ui_last_healthy_elapsed_ms'] = 13000
if mutation == 'vbus-not-isolated':
    observations['power']['vbus_isolated'] = False
if mutation == 'vbus-int':
    observations['power']['vbus_isolated'] = 1
if mutation == 'power-not-removed':
    observations['power']['external_power_removed'] = False
if mutation == 'power-removal-time':
    observations['power']['power_removed_elapsed_ms'] = 13000
if mutation == 'barrier-missing':
    barriers.clear()
if mutation == 'barrier-timing':
    barriers[0]['ready_elapsed_ms'] = 7499
if mutation == 'barrier-timeout':
    barriers[0]['timed_out'] = True
if mutation == 'reset-kind':
    reset_observation['expected_kind'] = 'none'
if mutation == 'reset-count':
    reset_observation['observed_count'] = 0
if mutation == 'unexpected-reset':
    reset_observation['unexpected_count'] = 1
if mutation == 'panic':
    reset_observation['panic_observed'] = True
if mutation == 'watchdog':
    reset_observation['watchdog_reset_observed'] = True
if mutation == 'reset-bool-count':
    reset_observation['observed_count'] = True
if mutation == 'panic-int':
    reset_observation['panic_observed'] = 0
if mutation == 'watchdog-int':
    reset_observation['watchdog_reset_observed'] = 0
if mutation == 'wrong-firmware':
    environment = 'waveshare-349-hil'
if mutation == 'wrong-build':
    build_kind = 'hil-fault'
if mutation == 'wrong-hil':
    hil_active = True

raw_directory = Path(argument('--raw-artifact-dir'))
raw_payloads = {
    'ap-traffic.json': json.dumps(observations['ap_traffic'], sort_keys=True).encode('utf-8'),
    'firmware-build.json': json.dumps({
        'environment': environment,
        'build_kind': build_kind,
        'target_sha': argument('--target-sha'),
        'hil_fault_control_active': hil_active,
    }, sort_keys=True).encode('utf-8'),
    'power-timeline.json': json.dumps({
        'voltage_refresh': observations['voltage_refresh'],
        'power_button': observations['power_button'],
        'critical_shutdown': observations['critical_shutdown'],
        'power': observations['power'],
    }, sort_keys=True).encode('utf-8'),
    'reset-summary.json': json.dumps(reset_observation, sort_keys=True).encode('utf-8'),
    'serial.log': json.dumps({
        'health': observations['health'],
        'reset': reset_observation,
    }, sort_keys=True).encode('utf-8'),
    'ui-health.json': json.dumps(observations['health'], sort_keys=True).encode('utf-8'),
}
if mutation == 'raw-missing':
    raw_payloads.pop('power-timeline.json')
if mutation == 'raw-extra':
    raw_payloads['extra.json'] = b'not-declared'
for filename, content in raw_payloads.items():
    (raw_directory / filename).write_bytes(content)
raw_digest_by_filename = {
    filename: hashlib.sha256(content).hexdigest()
    for filename, content in raw_payloads.items()
}
capture_commitments = {
    'ap_traffic_sha256': raw_digest_by_filename.get('ap-traffic.json', digest('missing')),
    'build_evidence_sha256': raw_digest_by_filename.get('firmware-build.json', digest('missing')),
    'power_timeline_sha256': raw_digest_by_filename.get('power-timeline.json', digest('missing')),
    'reset_summary_sha256': raw_digest_by_filename.get('reset-summary.json', digest('missing')),
    'serial_log_sha256': raw_digest_by_filename.get('serial.log', digest('missing')),
    'ui_health_sha256': raw_digest_by_filename.get('ui-health.json', digest('missing')),
}
observations['voltage_refresh'].update({
    'source_role': 'power-timeline',
    'source_sha256': capture_commitments['power_timeline_sha256'],
})
observations['ap_traffic'].update({
    'source_role': 'ap-traffic',
    'source_sha256': capture_commitments['ap_traffic_sha256'],
})
observations['power_button'].update({
    'source_role': 'power-timeline',
    'source_sha256': capture_commitments['power_timeline_sha256'],
})
observations['critical_shutdown'].update({
    'source_role': 'power-timeline',
    'source_sha256': capture_commitments['power_timeline_sha256'],
})
observations['health']['source_commitments'] = {
    'serial_log_sha256': capture_commitments['serial_log_sha256'],
    'ui_health_sha256': capture_commitments['ui_health_sha256'],
}
observations['power'].update({
    'source_role': 'power-timeline',
    'source_sha256': capture_commitments['power_timeline_sha256'],
})
reset_observation.update({
    'source_role': 'reset-summary',
    'source_sha256': capture_commitments['reset_summary_sha256'],
})
if mutation == 'source-power-unbound':
    observations['power']['source_sha256'] = digest('unrelated-power-source')
if mutation == 'source-health-unbound':
    observations['health']['source_commitments']['serial_log_sha256'] = digest('unrelated-serial')
if mutation == 'reset-source-unbound':
    reset_observation['source_sha256'] = digest('unrelated-reset')
if mutation == 'reset-timing-unbound':
    reset_observation['observed_elapsed_ms'] = 12499
if mutation == 'raw-observation-mismatch':
    observations['voltage_refresh']['refreshed_elapsed_ms'] = 3501
    facts['voltage-refresh-delay-ms'] = 501

capture_fields = (
    'ap_traffic_sha256', 'build_evidence_sha256', 'power_timeline_sha256',
    'reset_summary_sha256', 'serial_log_sha256', 'ui_health_sha256',
)
payload = {
    'schema_version': 1,
    'case_id': argument('--case'),
    'role_id': argument('--role-id'),
    'session_id': argument('--session-id'),
    'attempt_id': argument('--attempt-id'),
    'target_sha': argument('--target-sha'),
    'dut_alias': argument('--dut-alias'),
    'rig_alias': argument('--rig-alias'),
    'execution_mode': 'simulated',
    'hardware_observed': False,
    'started_at_utc': started.isoformat(timespec='seconds').replace('+00:00', 'Z'),
    'completed_at_utc': (started if mutation == 'utc-duration' else now).isoformat(timespec='seconds').replace('+00:00', 'Z'),
    'case_descriptor': case_descriptor,
    'case_descriptor_sha256': argument('--case-descriptor-sha256'),
    'firmware': {
        'environment': environment,
        'build_kind': build_kind,
        'target_sha': argument('--target-sha'),
        'binary_sha256': digest('bsc07-production-binary'),
        'hil_fault_control_active': hil_active,
    },
    'stimuli': stimuli,
    'observations': observations,
    'barriers': barriers,
    'reset_observation': reset_observation,
    'facts': facts,
    'capture_commitments': capture_commitments,
    'raw_artifact_request_sha256': argument('--raw-artifact-request-sha256'),
}
if mutation == 'role-id':
    payload['role_id'] = 'substituted-role'
if mutation == 'target':
    payload['target_sha'] = 'f' * 40
if mutation == 'descriptor-digest':
    payload['case_descriptor_sha256'] = digest('wrong-descriptor')
if mutation == 'record-extra':
    payload['invented-field'] = True
if mutation == 'firmware-extra':
    payload['firmware']['invented-field'] = True
if mutation == 'capture-reuse':
    payload['capture_commitments']['serial_log_sha256'] = payload['capture_commitments']['ui_health_sha256']
if mutation == 'bool-schema':
    payload['schema_version'] = True
if mutation == 'integer-hardware':
    payload['hardware_observed'] = 0
if mutation.startswith('capture-self-binding-'):
    field = mutation.removeprefix('capture-self-binding-')
    payload['capture_commitments'][field] = digest(f'tampered-{field}')
if mutation == 'raw-request-substitution':
    payload['raw_artifact_request_sha256'] = digest('substituted-request')
payload['evidence_binding_sha256'] = commitment(payload)
sys.stdout.write(json.dumps(payload))
""",
    )
    bsc10_adapter = root / "fake-bsc10-adapter.py"
    write_executable(
        bsc10_adapter,
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
    return hashlib.sha256(b'v1simple.bsc10.case-record.v1\\0' + canonical.encode('utf-8')).hexdigest()

role_id = argument('--role-id')
is_fault = role_id == 'wifi-enable-transaction-fault'
now = datetime.now(timezone.utc)
started = now - timedelta(seconds=50)
case_descriptor = {
    'fault_build_required': True,
    'id': 'BSC-10',
    'minimum_runs': 1,
    'production_replay': {
        'barrier_ids': [],
        'build_kind': 'production',
        'facts': [
            {'expected': True, 'id': 'ordinary-enable-succeeded', 'type': 'boolean'},
            {'expected': True, 'id': 'authoritative-status-reconciled', 'type': 'boolean'},
            {'expected': False, 'id': 'hil-fault-control-active', 'type': 'boolean'},
        ],
        'fault_ids': [],
        'reset_contract': {'expected_count': 0, 'expected_kind': 'none', 'unexpected_count': 0},
        'role_id': 'wifi-enable-production-replay',
        'schema': 'case-observation-v1',
        'stimulus_ids': ['request-enable', 'refresh-authoritative-status'],
        'vbus_isolation_required': False,
    },
    'production_replay_required': True,
    'required_dut_capabilities': [
        'firmware-execution',
        'maintenance-mode',
        'serial',
        'wifi-client',
    ],
    'required_rig_capabilities': [
        'artifact-capture',
        'browser-client',
        'http-response-control',
        'utc-time-source',
    ],
    'scenario': {
        'barrier_ids': ['admission-rejection-observed'],
        'build_kind': 'hil-fault',
        'facts': [
            {'expected': True, 'id': 'nvs-remained-disabled-after-rejection', 'type': 'boolean'},
            {'expected': True, 'id': 'runtime-remained-disabled-after-rejection', 'type': 'boolean'},
            {'expected': True, 'id': 'selected-slot-unchanged-after-rejection', 'type': 'boolean'},
            {'expected': True, 'id': 'ui-remained-disabled-after-rejection', 'type': 'boolean'},
            {'id': 'retry-mutation-count', 'maximum': 1, 'minimum': 1, 'type': 'integer'},
            {'id': 'persist-count', 'maximum': 1, 'minimum': 1, 'type': 'integer'},
            {'id': 'unknown-outcome-followup-mutations', 'maximum': 0, 'minimum': 0, 'type': 'integer'},
            {'expected': True, 'id': 'authoritative-status-reconciled', 'type': 'boolean'},
        ],
        'fault_ids': ['wifi-enable-admission-fail-once', 'drop-post-response-once'],
        'reset_contract': {'expected_count': 0, 'expected_kind': 'none', 'unexpected_count': 0},
        'role_id': 'wifi-enable-transaction-fault',
        'schema': 'case-observation-v1',
        'stimulus_ids': [
            'request-enable',
            'clear-admission-fault',
            'retry-enable-once',
            'drop-success-response',
            'refresh-authoritative-status',
        ],
        'vbus_isolation_required': False,
    },
}
mutation = os.environ.get('FAKE_BSC10_MUTATION', '')
if mutation == 'descriptor':
    case_descriptor['required_dut_capabilities'].append('invented-capability')
role = case_descriptor['scenario' if is_fault else 'production_replay']
stimuli = [
    {'id': stimulus_id, 'sequence': sequence, 'elapsed_ms': sequence * 5000, 'result': 'pass'}
    for sequence, stimulus_id in enumerate(role['stimulus_ids'], start=1)
]
if is_fault:
    faults = [
        {
            'id': 'wifi-enable-admission-fail-once',
            'sequence': 1,
            'armed_elapsed_ms': 1000,
            'triggered_elapsed_ms': 4500,
            'cleared_elapsed_ms': 5500,
        },
        {
            'id': 'drop-post-response-once',
            'sequence': 2,
            'armed_elapsed_ms': 18000,
            'triggered_elapsed_ms': 20000,
            'cleared_elapsed_ms': 21000,
        },
    ]
    barriers = [{
        'id': 'admission-rejection-observed',
        'sequence': 1,
        'ready_elapsed_ms': 4500,
        'released_elapsed_ms': 5500,
        'timed_out': False,
    }]
    facts = {
        'nvs-remained-disabled-after-rejection': True,
        'runtime-remained-disabled-after-rejection': True,
        'selected-slot-unchanged-after-rejection': True,
        'ui-remained-disabled-after-rejection': True,
        'retry-mutation-count': 1,
        'persist-count': 1,
        'unknown-outcome-followup-mutations': 0,
        'authoritative-status-reconciled': True,
    }
    environment = 'waveshare-349-hil'
    build_kind = 'hil-fault'
    hil_active = True
else:
    faults = []
    barriers = []
    facts = {
        'ordinary-enable-succeeded': True,
        'authoritative-status-reconciled': True,
        'hil-fault-control-active': False,
    }
    environment = 'waveshare-349'
    build_kind = 'production'
    hil_active = False
resets = {'expected_kind': 'none', 'planned': 0, 'observed': 0, 'unexpected': 0}

if mutation == 'stimulus-order':
    stimuli[0]['id'], stimuli[1]['id'] = stimuli[1]['id'], stimuli[0]['id']
if mutation == 'stimulus-timing':
    stimuli[1]['elapsed_ms'] = stimuli[0]['elapsed_ms']
if mutation == 'stimulus-missing':
    stimuli.pop()
if mutation == 'fault-order' and is_fault:
    faults[0]['id'], faults[1]['id'] = faults[1]['id'], faults[0]['id']
if mutation == 'fault-timing' and is_fault:
    faults[0]['triggered_elapsed_ms'] = 6000
if mutation == 'fault-missing' and is_fault:
    faults.pop()
if mutation == 'response-window' and is_fault:
    faults[1]['cleared_elapsed_ms'] = 26000
if mutation == 'barrier-order' and is_fault:
    barriers[0]['sequence'] = 2
if mutation == 'barrier-timing' and is_fault:
    barriers[0]['released_elapsed_ms'] = 11000
if mutation == 'barrier-timeout' and is_fault:
    barriers[0]['timed_out'] = True
if mutation == 'barrier-missing' and is_fault:
    barriers.clear()
if mutation == 'retry-count' and is_fault:
    facts['retry-mutation-count'] = 2
if mutation == 'persist-count' and is_fault:
    facts['persist-count'] = 2
if mutation == 'followup-count' and is_fault:
    facts['unknown-outcome-followup-mutations'] = 1
if mutation == 'fault-reconciliation' and is_fault:
    facts['authoritative-status-reconciled'] = False
if mutation == 'production-success' and not is_fault:
    facts['ordinary-enable-succeeded'] = False
if mutation == 'production-reconciliation' and not is_fault:
    facts['authoritative-status-reconciled'] = False
if mutation == 'production-hil' and not is_fault:
    facts['hil-fault-control-active'] = True
if mutation == 'fact-extra':
    facts['invented-fact'] = True
if mutation == 'production-fault' and not is_fault:
    faults = [{
        'id': 'drop-post-response-once',
        'sequence': 1,
        'armed_elapsed_ms': 1000,
        'triggered_elapsed_ms': 2000,
        'cleared_elapsed_ms': 3000,
    }]
if mutation == 'production-barrier' and not is_fault:
    barriers = [{
        'id': 'admission-rejection-observed',
        'sequence': 1,
        'ready_elapsed_ms': 1000,
        'released_elapsed_ms': 2000,
        'timed_out': False,
    }]
if mutation == 'reset':
    resets['observed'] = 1
if mutation == 'wrong-firmware':
    environment = 'waveshare-349' if is_fault else 'waveshare-349-hil'
if mutation == 'wrong-hil':
    hil_active = not hil_active
if mutation == 'build-kind':
    build_kind = 'production' if is_fault else 'hil-fault'

payload = {
    'schema_version': 1,
    'case_id': argument('--case'),
    'role_id': role_id,
    'session_id': argument('--session-id'),
    'attempt_id': argument('--attempt-id'),
    'target_sha': argument('--target-sha'),
    'dut_alias': argument('--dut-alias'),
    'rig_alias': argument('--rig-alias'),
    'execution_mode': 'simulated',
    'hardware_observed': False,
    'started_at_utc': started.isoformat(timespec='seconds').replace('+00:00', 'Z'),
    'completed_at_utc': now.isoformat(timespec='seconds').replace('+00:00', 'Z'),
    'case_descriptor': case_descriptor,
    'case_descriptor_sha256': argument('--case-descriptor-sha256'),
    'firmware': {
        'environment': environment,
        'target_sha': argument('--target-sha'),
        'binary_sha256': digest(f'{role_id}-binary'),
        'hil_fault_control_active': hil_active,
        'build_kind': build_kind,
    },
    'stimuli': stimuli,
    'faults': faults,
    'barriers': barriers,
    'vbus_isolated': False,
    'resets': resets,
    'facts': facts,
    'capture_commitments': {
        'browser_trace_sha256': digest(f'{role_id}-browser-trace'),
        'build_evidence_sha256': digest(f'{role_id}-build-evidence'),
        'http_sequence_sha256': digest(f'{role_id}-http-sequence'),
        'nvs_runtime_state_sha256': digest(f'{role_id}-nvs-runtime-state'),
        'serial_log_sha256': digest(f'{role_id}-serial-log'),
    },
}
if mutation == 'record-extra':
    payload['invented-field'] = True
if mutation == 'firmware-extra':
    payload['firmware']['invented-field'] = True
if mutation == 'role-id':
    payload['role_id'] = (
        'wifi-enable-production-replay' if is_fault else 'wifi-enable-transaction-fault'
    )
if mutation == 'target':
    payload['target_sha'] = 'f' * 40
if mutation == 'vbus':
    payload['vbus_isolated'] = True
if mutation == 'descriptor-digest':
    payload['case_descriptor_sha256'] = digest('wrong-descriptor')
if mutation == 'capture-reuse':
    payload['capture_commitments']['serial_log_sha256'] = payload['capture_commitments']['http_sequence_sha256']
if mutation == 'capture-missing':
    payload['capture_commitments'].pop('serial_log_sha256')
if mutation == 'bool-schema':
    payload['schema_version'] = True
if mutation == 'integer-hardware':
    payload['hardware_observed'] = 0
# Mutate after binding so this proves stale record-binding rejection only.
payload['evidence_binding_sha256'] = commitment(payload)
if mutation == 'capture-stale-binding-tamper':
    payload['capture_commitments']['serial_log_sha256'] = digest('substituted-serial')
sys.stdout.write(json.dumps(payload))
""",
    )
    bsc12_adapter = root / "fake-bsc12-adapter.py"
    write_executable(
        bsc12_adapter,
        """#!/usr/bin/env python3
from datetime import datetime, timedelta, timezone
import hashlib
import json
import os
from pathlib import Path
import sys

def argument(name):
    return sys.argv[sys.argv.index(name) + 1]

def digest(value):
    return hashlib.sha256(value.encode('utf-8')).hexdigest()

def commitment(payload):
    canonical = json.dumps(payload, ensure_ascii=False, separators=(',', ':'), sort_keys=True)
    return hashlib.sha256(b'v1simple.bsc12.case-record.v1\\0' + canonical.encode('utf-8')).hexdigest()

case_descriptor = {
    'fault_build_required': False,
    'id': 'BSC-12',
    'minimum_runs': 1,
    'production_replay': None,
    'production_replay_required': False,
    'required_dut_capabilities': [
        'firmware-execution', 'persistence', 'power-button', 'serial',
    ],
    'required_rig_capabilities': [
        'artifact-capture', 'bond-peer', 'power-control', 'sd-media',
        'utc-time-source', 'vbus-isolation', 'wake-input-control',
    ],
    'scenario': {
        'barrier_ids': ['wake-input-asserted-during-handoff', 'writers-completed'],
        'build_kind': 'production',
        'facts': [
            {'expected': True, 'id': 'poweroff-returned-false', 'type': 'boolean'},
            {'expected': True, 'id': 'disconnected-screen-restored', 'type': 'boolean'},
            {'expected': True, 'id': 'session-marker-unclean-after-reset', 'type': 'boolean'},
            {'id': 'settings-writer-count', 'maximum': 1, 'minimum': 1, 'type': 'integer'},
            {'id': 'bond-writer-count', 'maximum': 1, 'minimum': 1, 'type': 'integer'},
            {'expected': True, 'id': 'setting-survived-reset', 'type': 'boolean'},
            {'expected': True, 'id': 'bond-survived-reset', 'type': 'boolean'},
            {'expected': True, 'id': 'real-rtc-wake-input-observed', 'type': 'boolean'},
        ],
        'fault_ids': [],
        'reset_contract': {
            'expected_count': 1, 'expected_kind': 'forced-reset', 'unexpected_count': 0,
        },
        'role_id': 'aborted-shutdown-recovery',
        'schema': 'case-observation-v1',
        'stimulus_ids': [
            'begin-portable-shutdown', 'assert-wake-input', 'mutate-setting',
            'mutate-bond', 'wait-for-writers', 'force-reset',
        ],
        'vbus_isolation_required': True,
    },
}
role = case_descriptor['scenario']
mutation = os.environ.get('FAKE_BSC12_MUTATION', '')
now = datetime.now(timezone.utc)
started = now - timedelta(seconds=10)
stimulus_times = (100, 200, 350, 450, 550, 900)
stimuli = [
    {'id': stimulus_id, 'sequence': sequence, 'elapsed_ms': elapsed_ms, 'result': 'pass'}
    for sequence, (stimulus_id, elapsed_ms) in enumerate(
        zip(role['stimulus_ids'], stimulus_times), start=1
    )
]
barriers = [
    {
        'id': 'wake-input-asserted-during-handoff',
        'source': 'rtc-gpio-wake-input',
        'sequence': 1,
        'ready_elapsed_ms': 150,
        'released_elapsed_ms': 250,
        'timed_out': False,
    },
    {
        'id': 'writers-completed',
        'source': 'shutdown-persistence-writers',
        'sequence': 2,
        'ready_elapsed_ms': 550,
        'released_elapsed_ms': 700,
        'timed_out': False,
    },
]
shutdown = {
    'shutdown_begin_elapsed_ms': 100,
    'handoff_begin_elapsed_ms': 150,
    'wake_asserted_elapsed_ms': 200,
    'power_off_return_elapsed_ms': 250,
    'screen_restored_elapsed_ms': 260,
    'marker_rewritten_elapsed_ms': 270,
    'wake_input_source': 'rtc-gpio-wake-input',
    'wake_trigger': 'active-low',
    'real_rtc_wake_input': True,
    'power_off_result': False,
    'screen_state': 'disconnected',
    'marker_state': 'unclean',
}
safety = {
    'source': 'rig-power-reset-trace',
    'vbus_isolated': True,
    'vbus_verified_elapsed_ms': 50,
    'destructive_reset_triggered': True,
}
writers = [
    {
        'writer_id': 'settings',
        'source': 'deferred-settings-backup-writer',
        'sequence': 1,
        'mutation_elapsed_ms': 350,
        'completion_elapsed_ms': 650,
        'completion_count': 1,
        'duplicate_count': 0,
        'lost_count': 0,
        'stalled': False,
    },
    {
        'writer_id': 'bond',
        'source': 'ble-bond-backup-writer',
        'sequence': 2,
        'mutation_elapsed_ms': 450,
        'completion_elapsed_ms': 700,
        'completion_count': 1,
        'duplicate_count': 0,
        'lost_count': 0,
        'stalled': False,
    },
]
setting_digest = digest('bsc12-setting-mutation')
bond_digest = digest('bsc12-bond-mutation')
persistence = {
    'source': 'post-reset-persistence-readback',
    'setting_mutation_sha256': setting_digest,
    'setting_after_reset_sha256': setting_digest,
    'bond_mutation_sha256': bond_digest,
    'bond_after_reset_sha256': bond_digest,
    'session_marker_after_reset': 'unclean',
    'before_readback_elapsed_ms': 800,
    'after_readback_elapsed_ms': 1200,
}
reset = {
    'expected_kind': 'forced-reset',
    'source': 'rig-forced-reset',
    'planned': 1,
    'observed': 1,
    'unexpected': 0,
    'forced_elapsed_ms': 900,
    'boot_observed_elapsed_ms': 1100,
    'panic_observed': False,
    'watchdog_reset_observed': False,
    'load_prohibited_observed': False,
}
facts = {
    'poweroff-returned-false': True,
    'disconnected-screen-restored': True,
    'session-marker-unclean-after-reset': True,
    'settings-writer-count': 1,
    'bond-writer-count': 1,
    'setting-survived-reset': True,
    'bond-survived-reset': True,
    'real-rtc-wake-input-observed': True,
}

if mutation == 'descriptor':
    case_descriptor['required_dut_capabilities'].append('invented-capability')
if mutation == 'stimulus-missing':
    stimuli.pop()
if mutation == 'stimulus-order':
    stimuli[0], stimuli[1] = stimuli[1], stimuli[0]
if mutation == 'stimulus-negative':
    stimuli[0]['elapsed_ms'] = -1
if mutation == 'stimulus-duration':
    stimuli[-1]['elapsed_ms'] = 11000
if mutation == 'barrier-id':
    barriers[0]['id'] = 'invented-barrier'
if mutation == 'barrier-source':
    barriers[0]['source'] = 'invented-source'
if mutation == 'barrier-order':
    barriers[1]['sequence'] = 1
if mutation == 'barrier-timing':
    barriers[0]['released_elapsed_ms'] = 100
if mutation == 'barrier-timeout':
    barriers[0]['timed_out'] = True
if mutation == 'barrier-duration':
    barriers[1]['released_elapsed_ms'] = 11000
if mutation == 'handoff-order':
    shutdown['wake_asserted_elapsed_ms'] = 260
if mutation == 'wake-source':
    shutdown['wake_input_source'] = 'invented-source'
if mutation == 'wake-trigger':
    shutdown['wake_trigger'] = 'active-high'
if mutation == 'wake-not-real':
    shutdown['real_rtc_wake_input'] = False
if mutation == 'wake-bool-int':
    shutdown['real_rtc_wake_input'] = 1
if mutation == 'poweroff-result':
    shutdown['power_off_result'] = True
if mutation == 'poweroff-bool-int':
    shutdown['power_off_result'] = 0
if mutation == 'screen-state':
    shutdown['screen_state'] = 'goodbye'
if mutation == 'marker-state':
    shutdown['marker_state'] = 'clean'
if mutation == 'marker-timing':
    shutdown['marker_rewritten_elapsed_ms'] = 400
if mutation == 'writer-missing':
    writers.pop()
if mutation == 'writer-source':
    writers[0]['source'] = 'invented-writer'
if mutation == 'writer-count-zero':
    writers[0]['completion_count'] = 0
if mutation == 'writer-count-two':
    writers[0]['completion_count'] = 2
if mutation == 'writer-duplicate':
    writers[0]['duplicate_count'] = 1
if mutation == 'writer-lost':
    writers[0]['lost_count'] = 1
if mutation == 'writer-stalled':
    writers[0]['stalled'] = True
if mutation == 'writer-stalled-int':
    writers[0]['stalled'] = 0
if mutation == 'writer-completion-timing':
    writers[0]['completion_elapsed_ms'] = 901
if mutation == 'writer-mutation-timing':
    writers[0]['mutation_elapsed_ms'] = 351
if mutation == 'persistence-source':
    persistence['source'] = 'invented-source'
if mutation == 'setting-lost':
    persistence['setting_after_reset_sha256'] = digest('lost-setting')
if mutation == 'bond-lost':
    persistence['bond_after_reset_sha256'] = digest('lost-bond')
if mutation == 'post-reset-marker':
    persistence['session_marker_after_reset'] = 'clean'
if mutation == 'persistence-digest':
    persistence['setting_after_reset_sha256'] = 'invalid'
if mutation == 'reset-kind':
    reset['expected_kind'] = 'hard-reset'
if mutation == 'reset-source':
    reset['source'] = 'invented-source'
if mutation == 'reset-planned':
    reset['planned'] = 0
if mutation == 'reset-observed':
    reset['observed'] = 0
if mutation == 'reset-unexpected':
    reset['unexpected'] = 1
if mutation == 'reset-forced-timing':
    reset['forced_elapsed_ms'] = 899
if mutation == 'reset-boot-timing':
    reset['boot_observed_elapsed_ms'] = 900
if mutation == 'panic':
    reset['panic_observed'] = True
if mutation == 'watchdog':
    reset['watchdog_reset_observed'] = True
if mutation == 'load-prohibited':
    reset['load_prohibited_observed'] = True
if mutation == 'panic-int':
    reset['panic_observed'] = 0
if mutation.startswith('fact-'):
    fact_id = {
        'fact-poweroff': 'poweroff-returned-false',
        'fact-screen': 'disconnected-screen-restored',
        'fact-marker': 'session-marker-unclean-after-reset',
        'fact-settings-writer': 'settings-writer-count',
        'fact-bond-writer': 'bond-writer-count',
        'fact-setting': 'setting-survived-reset',
        'fact-bond': 'bond-survived-reset',
        'fact-wake': 'real-rtc-wake-input-observed',
    }[mutation]
    facts[fact_id] = 2 if fact_id.endswith('writer-count') else False

raw_root = Path(argument('--raw-artifact-dir'))
raw_root.mkdir(parents=True, exist_ok=True)
firmware_record = {
    'environment': 'waveshare-349',
    'build_kind': 'production',
    'target_sha': argument('--target-sha'),
    'binary_sha256': digest('bsc12-production-binary'),
    'hil_fault_control_active': False,
}
raw_payloads = {
    'firmware-build.json': {
        'schema_version': 1,
        'source': 'platformio-production-build',
        'firmware': firmware_record,
    },
    'persistence-before.json': {
        'schema_version': 1,
        'source': 'pre-reset-persistence-readback',
        'captured_elapsed_ms': persistence['before_readback_elapsed_ms'],
        'setting_sha256': persistence['setting_mutation_sha256'],
        'bond_sha256': persistence['bond_mutation_sha256'],
        'session_marker': 'unclean',
    },
    'persistence-after.json': {
        'schema_version': 1,
        'source': 'post-reset-persistence-readback',
        'captured_elapsed_ms': persistence['after_readback_elapsed_ms'],
        'setting_sha256': persistence['setting_after_reset_sha256'],
        'bond_sha256': persistence['bond_after_reset_sha256'],
        'session_marker': persistence['session_marker_after_reset'],
    },
    'power-reset-trace.json': {
        'schema_version': 1,
        'source': 'rig-power-reset-trace',
        'vbus_isolated': True,
        'vbus_verified_elapsed_ms': 50,
        'forced_reset_edges_elapsed_ms': [reset['forced_elapsed_ms']],
    },
    'shutdown-timeline.json': {
        'schema_version': 1,
        'source': 'dut-serial-timeline',
        **{
            field: shutdown[field]
            for field in (
                'shutdown_begin_elapsed_ms', 'handoff_begin_elapsed_ms',
                'power_off_return_elapsed_ms', 'screen_restored_elapsed_ms',
                'marker_rewritten_elapsed_ms', 'power_off_result', 'screen_state', 'marker_state',
            )
        },
    },
    'wake-input-trace.json': {
        'schema_version': 1,
        'source': 'rtc-gpio-wake-input',
        'trigger': 'active-low',
        'asserted_elapsed_ms': 200,
        'deasserted_elapsed_ms': 240,
        'observed_during_handoff': True,
    },
}
serial_events = [
    {'event': 'shutdown-begin', 'elapsed_ms': 100},
    {'event': 'handoff-begin', 'elapsed_ms': 150},
    {'event': 'wake-asserted', 'elapsed_ms': 200},
    {'event': 'power-off-returned-false', 'elapsed_ms': 250},
    {'event': 'screen-restored-disconnected', 'elapsed_ms': 260},
    {'event': 'marker-rewritten-unclean', 'elapsed_ms': 270},
    {'event': 'settings-writer-complete', 'elapsed_ms': 650},
    {'event': 'bond-writer-complete', 'elapsed_ms': 700},
    {'event': 'reset-forced', 'elapsed_ms': 900},
    {'event': 'boot-observed', 'elapsed_ms': 1100},
    {'event': 'setting-readback', 'elapsed_ms': 1200},
    {'event': 'bond-readback', 'elapsed_ms': 1200},
]
if mutation == 'fabricated-equal-persistence':
    raw_payloads['persistence-after.json']['setting_sha256'] = digest('actual-lost-setting')
if mutation == 'preboot-readback':
    persistence['after_readback_elapsed_ms'] = 1000
    raw_payloads['persistence-after.json']['captured_elapsed_ms'] = 1000
    serial_events[-2]['elapsed_ms'] = 1000
    serial_events[-1]['elapsed_ms'] = 1000
if mutation == 'assertion-only-vbus':
    raw_payloads['power-reset-trace.json']['vbus_isolated'] = False
if mutation == 'assertion-only-wake':
    raw_payloads['wake-input-trace.json']['observed_during_handoff'] = False
if mutation == 'assertion-only-reset':
    raw_payloads['power-reset-trace.json']['forced_reset_edges_elapsed_ms'] = []
if mutation == 'assertion-only-crash':
    serial_events.append({'event': 'panic', 'elapsed_ms': 1250})
if mutation == 'serial-writer-missing':
    serial_events = [event for event in serial_events if event['event'] != 'settings-writer-complete']
if mutation == 'serial-writer-timing':
    next(event for event in serial_events if event['event'] == 'settings-writer-complete')['elapsed_ms'] = 651

for filename, raw_payload in raw_payloads.items():
    (raw_root / filename).write_text(
        json.dumps(raw_payload, separators=(',', ':'), sort_keys=True) + '\\n', encoding='utf-8'
    )
(raw_root / 'serial.log').write_text(
    ''.join(json.dumps(event, separators=(',', ':'), sort_keys=True) + '\\n' for event in serial_events),
    encoding='utf-8',
)
raw_roles = (
    ('firmware-build', 'firmware-build.json', 'firmware_build_sha256'),
    ('persistence-after', 'persistence-after.json', 'persistence_after_sha256'),
    ('persistence-before', 'persistence-before.json', 'persistence_before_sha256'),
    ('power-reset-trace', 'power-reset-trace.json', 'power_reset_trace_sha256'),
    ('serial-log', 'serial.log', 'serial_log_sha256'),
    ('shutdown-timeline', 'shutdown-timeline.json', 'shutdown_timeline_sha256'),
    ('wake-input-trace', 'wake-input-trace.json', 'wake_input_trace_sha256'),
)
capture_commitments = {}
raw_artifact_manifest = []
for raw_role, filename, commitment_field in raw_roles:
    data = (raw_root / filename).read_bytes()
    capture_commitments[commitment_field] = hashlib.sha256(data).hexdigest()
    raw_artifact_manifest.append({
        'role': raw_role,
        'filename': filename,
        'bytes': len(data),
        'sha256': capture_commitments[commitment_field],
    })
shutdown['shutdown_timeline_sha256'] = capture_commitments['shutdown_timeline_sha256']
shutdown['wake_input_trace_sha256'] = capture_commitments['wake_input_trace_sha256']
shutdown['serial_log_sha256'] = capture_commitments['serial_log_sha256']
persistence['persistence_before_sha256'] = capture_commitments['persistence_before_sha256']
persistence['persistence_after_sha256'] = capture_commitments['persistence_after_sha256']
reset['power_reset_trace_sha256'] = capture_commitments['power_reset_trace_sha256']
reset['serial_log_sha256'] = capture_commitments['serial_log_sha256']
safety['power_reset_trace_sha256'] = capture_commitments['power_reset_trace_sha256']

payload = {
    'schema_version': 1,
    'case_id': argument('--case'),
    'role_id': argument('--role-id'),
    'session_id': argument('--session-id'),
    'attempt_id': argument('--attempt-id'),
    'run_index': int(argument('--run-index')),
    'target_sha': argument('--target-sha'),
    'dut_alias': argument('--dut-alias'),
    'rig_alias': argument('--rig-alias'),
    'execution_mode': 'simulated',
    'hardware_observed': False,
    'started_at_utc': started.isoformat(timespec='seconds').replace('+00:00', 'Z'),
    'completed_at_utc': now.isoformat(timespec='seconds').replace('+00:00', 'Z'),
    'case_descriptor': case_descriptor,
    'case_descriptor_sha256': argument('--case-descriptor-sha256'),
    'descriptor': role,
    'firmware': firmware_record,
    'preconditions': {
        'vbus_isolated': argument('--vbus-isolated') == 'true',
        'destructive_reset_acknowledged': argument('--destructive-reset-acknowledged') == 'true',
    },
    'safety_observation': safety,
    'stimuli': stimuli,
    'barriers': barriers,
    'shutdown_observation': shutdown,
    'writers': writers,
    'persistence': persistence,
    'reset': reset,
    'facts': facts,
    'capture_commitments': capture_commitments,
    'raw_artifact_manifest': raw_artifact_manifest,
    'raw_artifact_request_sha256': argument('--raw-artifact-request-sha256'),
}
if mutation == 'role':
    payload['role_id'] = 'substituted-role'
if mutation == 'session':
    payload['session_id'] = 'substituted-session'
if mutation == 'attempt':
    payload['attempt_id'] = 'substituted-attempt'
if mutation == 'run-index-bool':
    payload['run_index'] = True
if mutation == 'target':
    payload['target_sha'] = 'f' * 40
if mutation == 'descriptor-digest':
    payload['case_descriptor_sha256'] = digest('wrong-descriptor')
if mutation == 'firmware-environment':
    payload['firmware']['environment'] = 'waveshare-349-hil'
if mutation == 'firmware-build':
    payload['firmware']['build_kind'] = 'hil-fault'
if mutation == 'firmware-hil':
    payload['firmware']['hil_fault_control_active'] = True
if mutation == 'firmware-hil-int':
    payload['firmware']['hil_fault_control_active'] = 0
if mutation == 'vbus-precondition':
    payload['preconditions']['vbus_isolated'] = False
if mutation == 'reset-precondition':
    payload['preconditions']['destructive_reset_acknowledged'] = False
if mutation == 'precondition-int':
    payload['preconditions']['vbus_isolated'] = 1
if mutation == 'utc-duration':
    payload['completed_at_utc'] = payload['started_at_utc']
if mutation == 'capture-reuse':
    payload['capture_commitments']['serial_log_sha256'] = payload['capture_commitments']['shutdown_timeline_sha256']
if mutation == 'capture-missing':
    payload['capture_commitments'].pop('serial_log_sha256')
if mutation == 'unrelated-capture':
    payload['capture_commitments']['persistence_after_sha256'] = digest('unrelated-capture')
if mutation == 'bool-schema':
    payload['schema_version'] = True
if mutation == 'integer-hardware':
    payload['hardware_observed'] = 0
payload['evidence_binding_sha256'] = commitment(payload)
if mutation == 'stale-binding':
    payload['capture_commitments']['serial_log_sha256'] = digest('stale-serial')
sys.stdout.write(json.dumps(payload))
""",
    )
    bsc14_adapter = root / "fake-bsc14-adapter.py"
    write_executable(
        bsc14_adapter,
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
    return hashlib.sha256(b'v1simple.bsc14.case-record.v1\\0' + canonical.encode('utf-8')).hexdigest()

role_id = argument('--role-id')
is_fault = role_id == 'touch-persistence-sd-fault'
now = datetime.now(timezone.utc)
started = now - timedelta(seconds=50)
case_descriptor = {
    'fault_build_required': True,
    'id': 'BSC-14',
    'minimum_runs': 1,
    'production_replay': {
        'barrier_ids': [],
        'build_kind': 'production',
        'facts': [
            {'expected': False, 'id': 'gesture-loop-stall-observed', 'type': 'boolean'},
            {'expected': True, 'id': 'latest-state-backup-valid', 'type': 'boolean'},
            {'expected': False, 'id': 'hil-fault-control-active', 'type': 'boolean'},
        ],
        'fault_ids': [],
        'reset_contract': {'expected_count': 0, 'expected_kind': 'none', 'unexpected_count': 0},
        'role_id': 'touch-persistence-production-replay',
        'schema': 'case-observation-v1',
        'stimulus_ids': [
            'slider-exit',
            'stealth-double-press',
            'profile-triple-tap',
            'use-slow-sd-media',
        ],
        'vbus_isolation_required': False,
    },
    'production_replay_required': True,
    'required_dut_capabilities': ['firmware-execution', 'persistence', 'serial', 'touchscreen'],
    'required_rig_capabilities': ['artifact-capture', 'reset-control', 'sd-media', 'utc-time-source'],
    'scenario': {
        'barrier_ids': ['gesture-persisted-to-nvs', 'reset-before-deferred-backup'],
        'build_kind': 'hil-fault',
        'facts': [
            {'expected': False, 'id': 'slider-loop-stall-observed', 'type': 'boolean'},
            {'expected': False, 'id': 'stealth-loop-stall-observed', 'type': 'boolean'},
            {'expected': False, 'id': 'profile-loop-stall-observed', 'type': 'boolean'},
            {'expected': True, 'id': 'slider-state-survived-reset', 'type': 'boolean'},
            {'expected': True, 'id': 'stealth-state-survived-reset', 'type': 'boolean'},
            {'expected': True, 'id': 'profile-state-survived-reset', 'type': 'boolean'},
            {'id': 'coalesced-backup-count', 'maximum': 1, 'minimum': 1, 'type': 'integer'},
            {'expected': True, 'id': 'backup-contains-latest-state', 'type': 'boolean'},
            {'expected': True, 'id': 'real-touch-input-observed', 'type': 'boolean'},
        ],
        'fault_ids': ['sd-mutex-holder'],
        'reset_contract': {'expected_count': 1, 'expected_kind': 'hard-reset', 'unexpected_count': 0},
        'role_id': 'touch-persistence-sd-fault',
        'schema': 'case-observation-v1',
        'stimulus_ids': [
            'hold-sd-mutex',
            'slider-exit',
            'stealth-double-press',
            'profile-triple-tap',
            'hard-reset-before-backup',
            'release-sd-mutex',
        ],
        'vbus_isolation_required': False,
    },
}
mutation = os.environ.get('FAKE_BSC14_MUTATION', '')
if mutation == 'descriptor':
    case_descriptor['required_dut_capabilities'].append('invented-capability')
stimulus_ids = [
    'hold-sd-mutex',
    'slider-exit',
    'stealth-double-press',
    'profile-triple-tap',
    'hard-reset-before-backup',
    'release-sd-mutex',
] if is_fault else [
    'slider-exit',
    'stealth-double-press',
    'profile-triple-tap',
    'use-slow-sd-media',
]
stimuli = [
    {'id': stimulus_id, 'sequence': sequence, 'elapsed_ms': sequence * 5000, 'result': 'pass'}
    for sequence, stimulus_id in enumerate(stimulus_ids, start=1)
]
if mutation == 'stimulus-order':
    stimuli[0]['id'], stimuli[1]['id'] = stimuli[1]['id'], stimuli[0]['id']
if mutation == 'stimulus-missing':
    stimuli.pop()
if mutation == 'stimulus-time':
    stimuli[1]['elapsed_ms'] = stimuli[0]['elapsed_ms']
if mutation == 'stimulus-result':
    stimuli[0]['result'] = 'fail'
if mutation == 'missing-slow-sd' and not is_fault:
    stimuli.pop()

if is_fault:
    facts = {
        'slider-loop-stall-observed': False,
        'stealth-loop-stall-observed': False,
        'profile-loop-stall-observed': False,
        'slider-state-survived-reset': True,
        'stealth-state-survived-reset': True,
        'profile-state-survived-reset': True,
        'coalesced-backup-count': 1,
        'backup-contains-latest-state': True,
        'real-touch-input-observed': True,
    }
    faults = [{
        'id': 'sd-mutex-holder',
        'sequence': 1,
        'armed_elapsed_ms': 1000,
        'triggered_elapsed_ms': 2000,
        'cleared_elapsed_ms': 30000,
    }]
    barriers = [
        {
            'id': 'gesture-persisted-to-nvs',
            'sequence': 1,
            'ready_elapsed_ms': 5000,
            'released_elapsed_ms': 20000,
            'timed_out': False,
        },
        {
            'id': 'reset-before-deferred-backup',
            'sequence': 2,
            'ready_elapsed_ms': 21000,
            'released_elapsed_ms': 30000,
            'timed_out': False,
        },
    ]
    resets = {'expected_kind': 'hard-reset', 'planned': 1, 'observed': 1, 'unexpected': 0}
    environment = 'waveshare-349-hil'
    hil_active = True
    build_kind = 'hil-fault'
else:
    facts = {
        'gesture-loop-stall-observed': False,
        'latest-state-backup-valid': True,
        'hil-fault-control-active': False,
    }
    faults = []
    barriers = []
    resets = {'expected_kind': 'none', 'planned': 0, 'observed': 0, 'unexpected': 0}
    environment = 'waveshare-349'
    hil_active = False
    build_kind = 'production'

fact_mutations = {
    'slider-stall': ('slider-loop-stall-observed', True),
    'stealth-stall': ('stealth-loop-stall-observed', True),
    'profile-stall': ('profile-loop-stall-observed', True),
    'slider-survival': ('slider-state-survived-reset', False),
    'stealth-survival': ('stealth-state-survived-reset', False),
    'profile-survival': ('profile-state-survived-reset', False),
    'backup-count': ('coalesced-backup-count', 2),
    'backup-latest': ('backup-contains-latest-state', False),
    'real-touch': ('real-touch-input-observed', False),
    'production-stall': ('gesture-loop-stall-observed', True),
    'production-backup': ('latest-state-backup-valid', False),
    'production-hil': ('hil-fault-control-active', True),
}
if mutation in fact_mutations:
    fact_id, value = fact_mutations[mutation]
    facts[fact_id] = value
if mutation == 'fault-identity' and is_fault:
    faults[0]['id'] = 'invented-fault'
if mutation == 'fault-order' and is_fault:
    faults[0]['sequence'] = 2
if mutation == 'fault-timing' and is_fault:
    faults[0]['triggered_elapsed_ms'] = 500
if mutation == 'barrier-order' and is_fault:
    barriers[1]['sequence'] = 1
if mutation == 'barrier-identity' and is_fault:
    barriers[0]['id'] = 'invented-barrier'
if mutation == 'barrier-timing' and is_fault:
    barriers[0]['released_elapsed_ms'] = 4000
if mutation == 'barrier-timeout' and is_fault:
    barriers[0]['timed_out'] = True
if mutation == 'reset-kind':
    resets['expected_kind'] = 'soft-reset'
if mutation == 'reset-count':
    resets['observed'] = 0
if mutation == 'unexpected-reset':
    resets['unexpected'] = 1
if mutation == 'production-reset' and not is_fault:
    resets = {'expected_kind': 'hard-reset', 'planned': 1, 'observed': 1, 'unexpected': 0}
if mutation == 'production-fault' and not is_fault:
    faults = [{
        'id': 'sd-mutex-holder',
        'sequence': 1,
        'armed_elapsed_ms': 1000,
        'triggered_elapsed_ms': 2000,
        'cleared_elapsed_ms': 3000,
    }]
if mutation == 'production-barrier' and not is_fault:
    barriers = [{
        'id': 'gesture-persisted-to-nvs',
        'sequence': 1,
        'ready_elapsed_ms': 1000,
        'released_elapsed_ms': 2000,
        'timed_out': False,
    }]
if mutation == 'wrong-firmware':
    environment = 'waveshare-349' if is_fault else 'waveshare-349-hil'
if mutation == 'wrong-hil':
    hil_active = not hil_active
if mutation == 'firmware-build-kind':
    build_kind = 'production' if is_fault else 'hil-fault'

payload = {
    'schema_version': 1,
    'case_id': argument('--case'),
    'role_id': role_id,
    'session_id': argument('--session-id'),
    'attempt_id': argument('--attempt-id'),
    'target_sha': argument('--target-sha'),
    'dut_alias': argument('--dut-alias'),
    'rig_alias': argument('--rig-alias'),
    'execution_mode': 'simulated',
    'hardware_observed': False,
    'started_at_utc': started.isoformat(timespec='seconds').replace('+00:00', 'Z'),
    'completed_at_utc': now.isoformat(timespec='seconds').replace('+00:00', 'Z'),
    'case_descriptor': case_descriptor,
    'case_descriptor_sha256': argument('--case-descriptor-sha256'),
    'firmware': {
        'environment': environment,
        'target_sha': argument('--target-sha'),
        'binary_sha256': digest(f'{role_id}-binary'),
        'hil_fault_control_active': hil_active,
        'build_kind': build_kind,
    },
    'stimuli': stimuli,
    'faults': faults,
    'barriers': barriers,
    'vbus_isolated': False,
    'resets': resets,
    'facts': facts,
    'capture_commitments': {
        'build_evidence_sha256': digest(f'{role_id}-build-evidence'),
        'reset_timeline_sha256': digest(f'{role_id}-reset-timeline'),
        'sd_backup_sha256': digest(f'{role_id}-sd-backup'),
        'serial_log_sha256': digest(f'{role_id}-serial-log'),
        'touch_timeline_sha256': digest(f'{role_id}-touch-timeline'),
    },
}
if mutation == 'role-id':
    payload['role_id'] = (
        'touch-persistence-production-replay'
        if is_fault else 'touch-persistence-sd-fault'
    )
if mutation == 'vbus-isolated':
    payload['vbus_isolated'] = True
if mutation == 'descriptor-digest':
    payload['case_descriptor_sha256'] = digest('wrong-descriptor')
if mutation == 'reuse':
    payload['capture_commitments']['serial_log_sha256'] = payload['capture_commitments']['sd_backup_sha256']
if mutation == 'bool-schema':
    payload['schema_version'] = True
if mutation == 'integer-hardware':
    payload['hardware_observed'] = 0
payload['evidence_binding_sha256'] = commitment(payload)
if mutation == 'tamper':
    payload['capture_commitments']['serial_log_sha256'] = digest('tampered-serial')
sys.stdout.write(json.dumps(payload))
""",
    )
    bsc16_adapter = root / "fake-bsc16-adapter.py"
    write_executable(
        bsc16_adapter,
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
    return hashlib.sha256(b'v1simple.bsc16.case-record.v1\\0' + canonical.encode('utf-8')).hexdigest()

role = argument('--role')
is_fault = role == 'fault-collection'
now = datetime.now(timezone.utc)
started = now - timedelta(seconds=50)
stimulus_ids = [
    'pwr-wake-on-battery',
    'usb-cold-boot',
    'force-adc-init-failure',
    'hold-power-button',
    'transition-battery-to-usb',
    'transition-usb-to-battery',
] if is_fault else [
    'pwr-wake-on-battery',
    'usb-cold-boot',
    'hold-power-button',
    'transition-battery-to-usb',
    'transition-usb-to-battery',
]
stimuli = [
    {'id': stimulus_id, 'sequence': sequence, 'elapsed_ms': sequence * 5000, 'result': 'pass'}
    for sequence, stimulus_id in enumerate(stimulus_ids, start=1)
]
if os.environ.get('FAKE_BSC16_MISSING_STIMULUS') == '1':
    stimuli.pop()

if is_fault:
    facts = {
        'pwr-wake-transient-usb-observed': False,
        'usb-confirmation-delay-ms': 3000,
        'adc-failure-voltage-degraded': True,
        'adc-failure-power-button-operational': True,
        'long-hold-classified-as-usb': False,
        'long-hold-shutdown-succeeded': True,
        'source-flapping-observed': False,
        'gpio16-bounce-ms': 8,
    }
    lifecycle = [{
        'id': event_id,
        'sequence': sequence,
        'elapsed_ms': 10000 + sequence,
        'arm_sequence': 7,
        'ready_sequence': 3,
        'generation': 1,
        'phase': 1,
        'latch_initialized': True,
        'adc_handle_allocated': False,
        'voltage_valid': False,
        'source_classification': 'battery',
        'power_button_enabled': True,
    } for sequence, event_id in enumerate(('ready', 'fired', 'released'), start=1)]
    if os.environ.get('FAKE_BSC16_BAD_LIFECYCLE') == '1':
        lifecycle[1]['latch_initialized'] = False
    if os.environ.get('FAKE_BSC16_BOOL_PHASE') == '1':
        lifecycle[1]['phase'] = True
    if os.environ.get('FAKE_BSC16_HIGH_BOUNCE') == '1':
        facts['gpio16-bounce-ms'] = 25
    environment = 'waveshare-349-hil'
    hil_active = True
else:
    facts = {
        'battery-classification-correct': True,
        'usb-classification-correct': True,
        'power-button-operational': True,
        'source-flapping-observed': False,
        'hil-fault-control-active': False,
    }
    lifecycle = []
    if os.environ.get('FAKE_BSC16_REPLAY_FAULT') == '1':
        lifecycle = [{'id': 'fired'}]
    environment = 'waveshare-349'
    hil_active = False

if os.environ.get('FAKE_BSC16_WRONG_FACT') == '1':
    facts['source-flapping-observed'] = True
if os.environ.get('FAKE_BSC16_WRONG_FIRMWARE') == '1':
    environment = 'waveshare-349' if is_fault else 'waveshare-349-hil'
if os.environ.get('FAKE_BSC16_WRONG_HIL') == '1':
    hil_active = not hil_active

payload = {
    'schema_version': 1,
    'case_id': argument('--case'),
    'role': role,
    'session_id': argument('--session-id'),
    'attempt_id': argument('--attempt-id'),
    'target_sha': argument('--target-sha'),
    'dut_alias': argument('--dut-alias'),
    'rig_alias': argument('--rig-alias'),
    'execution_mode': 'simulated',
    'hardware_observed': False,
    'started_at_utc': started.isoformat(timespec='seconds').replace('+00:00', 'Z'),
    'completed_at_utc': now.isoformat(timespec='seconds').replace('+00:00', 'Z'),
    'firmware': {
        'environment': environment,
        'target_sha': argument('--target-sha'),
        'binary_sha256': digest(f'{role}-binary'),
        'hil_fault_control_active': hil_active,
    },
    'stimuli': stimuli,
    'facts': facts,
    'fault_lifecycle': lifecycle,
    'capture_commitments': {
        'build_evidence_sha256': digest(f'{role}-build-evidence'),
        'logic_analyzer_sha256': digest(f'{role}-logic-analyzer'),
        'poweroff_log_sha256': digest(f'{role}-poweroff-log'),
        'serial_log_sha256': digest(f'{role}-serial-log'),
        'source_transitions_sha256': digest(f'{role}-source-transitions'),
    },
}
if os.environ.get('FAKE_BSC16_REUSE') == '1':
    payload['capture_commitments']['serial_log_sha256'] = payload['capture_commitments']['poweroff_log_sha256']
if os.environ.get('FAKE_BSC16_BOOL_SCHEMA') == '1':
    payload['schema_version'] = True
if os.environ.get('FAKE_BSC16_INTEGER_HARDWARE') == '1':
    payload['hardware_observed'] = 0
payload['evidence_binding_sha256'] = commitment(payload)
if os.environ.get('FAKE_BSC16_TAMPER') == '1':
    payload['capture_commitments']['serial_log_sha256'] = digest('tampered-serial')
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
        "bsc02_adapter": bsc02_adapter,
        "bsc03_adapter": bsc03_adapter,
        "bsc04_adapter": bsc04_adapter,
        "bsc05_adapter": bsc05_adapter,
        "bsc06_adapter": bsc06_adapter,
        "bsc07_adapter": bsc07_adapter,
        "bsc13_adapter": bsc13_adapter,
        "bsc11_adapter": bsc11_adapter,
        "bsc10_adapter": bsc10_adapter,
        "bsc12_adapter": bsc12_adapter,
        "bsc14_adapter": bsc14_adapter,
        "bsc16_adapter": bsc16_adapter,
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


def run_bsc02_fixture(
    fixture: dict[str, Path | str],
    root: Path,
    *,
    production_replay: bool = False,
    runs: int = 1,
    false_ap: bool = False,
    rapid_retry: bool = False,
    rapid_later_retry: bool = False,
    short_guard: bool = False,
    low_free: bool = False,
    low_block: bool = False,
    no_release: bool = False,
    no_heap_stop: bool = False,
    identity_drift: bool = False,
    oversized_overhead: bool = False,
    no_maintenance: bool = False,
    no_http: bool = False,
    wrong_firmware: bool = False,
    wrong_hil_marker: bool = False,
    replay_fault: bool = False,
    reused_evidence: bool = False,
    tampered_evidence: bool = False,
) -> tuple[subprocess.CompletedProcess[str], Path]:
    role = "production" if production_replay else "fault"
    out_dir = root / f"bsc02-{role}-out"
    environment = os.environ.copy()
    environment.update(
        {
            "V1SIMPLE_HIL_TEST_HOOKS": "1",
            "FAKE_BSC02_FALSE_AP": "1" if false_ap else "0",
            "FAKE_BSC02_RAPID_RETRY": "1" if rapid_retry else "0",
            "FAKE_BSC02_RAPID_LATER_RETRY": "1" if rapid_later_retry else "0",
            "FAKE_BSC02_SHORT_GUARD": "1" if short_guard else "0",
            "FAKE_BSC02_LOW_FREE": "1" if low_free else "0",
            "FAKE_BSC02_LOW_BLOCK": "1" if low_block else "0",
            "FAKE_BSC02_NO_RELEASE": "1" if no_release else "0",
            "FAKE_BSC02_NO_HEAP_STOP": "1" if no_heap_stop else "0",
            "FAKE_BSC02_IDENTITY_DRIFT": "1" if identity_drift else "0",
            "FAKE_BSC02_OVERHEAD": "1" if oversized_overhead else "0",
            "FAKE_BSC02_NO_MAINTENANCE": "1" if no_maintenance else "0",
            "FAKE_BSC02_NO_HTTP": "1" if no_http else "0",
            "FAKE_BSC02_WRONG_FIRMWARE": "1" if wrong_firmware else "0",
            "FAKE_BSC02_WRONG_HIL_MARKER": "1" if wrong_hil_marker else "0",
            "FAKE_BSC02_REPLAY_FAULT": "1" if replay_fault else "0",
            "FAKE_BSC02_REUSE": "1" if reused_evidence else "0",
            "FAKE_BSC02_TAMPER": "1" if tampered_evidence else "0",
        }
    )
    command = [
        "python3",
        "-B",
        str(RUNNER),
        "--case",
        "BSC-02",
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
        str(fixture["bsc02_adapter"]),
        "--out-dir",
        str(out_dir),
    ]
    if production_replay:
        command.append("--production-replay")
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


def run_bsc04_fixture(
    fixture: dict[str, Path | str],
    root: Path,
    *,
    production_replay: bool = False,
    runs: int = 1,
    missing_event: bool = False,
    not_same_loop: bool = False,
    early_connection: bool = False,
    bad_deadline: bool = False,
    disconnect: bool = False,
    bad_downstream: bool = False,
    bad_lifecycle: bool = False,
    replay_fault: bool = False,
    wrong_firmware: bool = False,
    wrong_hil: bool = False,
    reused_evidence: bool = False,
    tampered_evidence: bool = False,
    bool_schema: bool = False,
    integer_hardware: bool = False,
) -> tuple[subprocess.CompletedProcess[str], Path]:
    role = "production" if production_replay else "fault"
    out_dir = root / f"bsc04-{role}-out"
    environment = os.environ.copy()
    environment.update(
        {
            "V1SIMPLE_HIL_TEST_HOOKS": "1",
            "FAKE_BSC04_MISSING_EVENT": "1" if missing_event else "0",
            "FAKE_BSC04_NOT_SAME_LOOP": "1" if not_same_loop else "0",
            "FAKE_BSC04_EARLY_CONNECTION": "1" if early_connection else "0",
            "FAKE_BSC04_BAD_DEADLINE": "1" if bad_deadline else "0",
            "FAKE_BSC04_DISCONNECT": "1" if disconnect else "0",
            "FAKE_BSC04_BAD_DOWNSTREAM": "1" if bad_downstream else "0",
            "FAKE_BSC04_BAD_LIFECYCLE": "1" if bad_lifecycle else "0",
            "FAKE_BSC04_REPLAY_FAULT": "1" if replay_fault else "0",
            "FAKE_BSC04_WRONG_FIRMWARE": "1" if wrong_firmware else "0",
            "FAKE_BSC04_WRONG_HIL": "1" if wrong_hil else "0",
            "FAKE_BSC04_REUSE": "1" if reused_evidence else "0",
            "FAKE_BSC04_TAMPER": "1" if tampered_evidence else "0",
            "FAKE_BSC04_BOOL_SCHEMA": "1" if bool_schema else "0",
            "FAKE_BSC04_INTEGER_HARDWARE": "1" if integer_hardware else "0",
        }
    )
    command = [
        "python3",
        "-B",
        str(RUNNER),
        "--case",
        "BSC-04",
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
        str(fixture["bsc04_adapter"]),
        "--out-dir",
        str(out_dir),
    ]
    if production_replay:
        command.append("--production-replay")
    completed = subprocess.run(
        command,
        cwd=ROOT,
        env=environment,
        capture_output=True,
        text=True,
        check=False,
    )
    return completed, out_dir


def run_bsc05_fixture(
    fixture: dict[str, Path | str],
    root: Path,
    *,
    production_replay: bool = False,
    runs: int = 3,
    mutation: str = "",
) -> tuple[subprocess.CompletedProcess[str], Path]:
    role = "production" if production_replay else "fault"
    out_dir = root / f"bsc05-{role}-out"
    environment = os.environ.copy()
    environment.update(
        {
            "V1SIMPLE_HIL_TEST_HOOKS": "1",
            "FAKE_BSC05_MUTATION": mutation,
        }
    )
    command = [
        "python3",
        "-B",
        str(RUNNER),
        "--case",
        "BSC-05",
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
        str(fixture["bsc05_adapter"]),
        "--out-dir",
        str(out_dir),
    ]
    if production_replay:
        command.append("--production-replay")
    completed = subprocess.run(
        command,
        cwd=ROOT,
        env=environment,
        capture_output=True,
        text=True,
        check=False,
    )
    return completed, out_dir


def run_bsc06_fixture(
    fixture: dict[str, Path | str],
    root: Path,
    *,
    production_replay: bool = False,
    runs: int = 3,
    mutation: str = "",
) -> tuple[subprocess.CompletedProcess[str], Path]:
    role = "production" if production_replay else "fault"
    out_dir = root / f"bsc06-{role}-out"
    environment = os.environ.copy()
    environment.update(
        {
            "V1SIMPLE_HIL_TEST_HOOKS": "1",
            "FAKE_BSC06_MUTATION": mutation,
        }
    )
    command = [
        "python3",
        "-B",
        str(RUNNER),
        "--case",
        "BSC-06",
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
        str(fixture["bsc06_adapter"]),
        "--out-dir",
        str(out_dir),
    ]
    if production_replay:
        command.append("--production-replay")
    completed = subprocess.run(
        command,
        cwd=ROOT,
        env=environment,
        capture_output=True,
        text=True,
        check=False,
    )
    return completed, out_dir



def run_bsc13_fixture(
    fixture: dict[str, Path | str],
    root: Path,
    *,
    production_replay: bool = False,
    runs: int = 3,
    missing_window: bool = False,
    wrong_window_role: bool = False,
    bad_generation: bool = False,
    phantom: bool = False,
    bad_resume: bool = False,
    missing_release: bool = False,
    replay_hil: bool = False,
    wrong_descriptor: bool = False,
    wrong_firmware: bool = False,
    wrong_hil: bool = False,
    reused_captures: bool = False,
    reused_generations: bool = False,
    tampered_evidence: bool = False,
    bool_schema: bool = False,
    integer_hardware: bool = False,
) -> tuple[subprocess.CompletedProcess[str], Path]:
    role = "production" if production_replay else "fault"
    out_dir = root / f"bsc13-{role}-out"
    environment = os.environ.copy()
    environment.update(
        {
            "V1SIMPLE_HIL_TEST_HOOKS": "1",
            "FAKE_BSC13_MISSING_WINDOW": "1" if missing_window else "0",
            "FAKE_BSC13_WRONG_WINDOW_ROLE": "1" if wrong_window_role else "0",
            "FAKE_BSC13_BAD_GENERATION": "1" if bad_generation else "0",
            "FAKE_BSC13_PHANTOM": "1" if phantom else "0",
            "FAKE_BSC13_BAD_RESUME": "1" if bad_resume else "0",
            "FAKE_BSC13_MISSING_RELEASE": "1" if missing_release else "0",
            "FAKE_BSC13_REPLAY_HIL": "1" if replay_hil else "0",
            "FAKE_BSC13_WRONG_DESCRIPTOR": "1" if wrong_descriptor else "0",
            "FAKE_BSC13_WRONG_FIRMWARE": "1" if wrong_firmware else "0",
            "FAKE_BSC13_WRONG_HIL": "1" if wrong_hil else "0",
            "FAKE_BSC13_REUSE_CAPTURES": "1" if reused_captures else "0",
            "FAKE_BSC13_REUSE_GENERATIONS": "1" if reused_generations else "0",
            "FAKE_BSC13_TAMPER": "1" if tampered_evidence else "0",
            "FAKE_BSC13_BOOL_SCHEMA": "1" if bool_schema else "0",
            "FAKE_BSC13_INTEGER_HARDWARE": "1" if integer_hardware else "0",
        }
    )
    command = [
        "python3",
        "-B",
        str(RUNNER),
        "--case",
        "BSC-13",
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
        str(fixture["bsc13_adapter"]),
        "--out-dir",
        str(out_dir),
    ]
    if production_replay:
        command.append("--production-replay")
    completed = subprocess.run(
        command,
        cwd=ROOT,
        env=environment,
        capture_output=True,
        text=True,
        check=False,
    )
    return completed, out_dir


def run_bsc07_fixture(
    fixture: dict[str, Path | str],
    root: Path,
    *,
    runs: int = 1,
    mutation: str = "",
    production_replay: bool = False,
    acknowledge_vbus: bool = True,
    drop_dut_capability: str | None = None,
    drop_rig_capability: str | None = None,
) -> tuple[subprocess.CompletedProcess[str], Path]:
    out_dir = root / "bsc07-out"
    environment = os.environ.copy()
    environment.update(
        {
            "V1SIMPLE_HIL_TEST_HOOKS": "1",
            "FAKE_BSC07_MUTATION": mutation,
        }
    )
    if drop_dut_capability is not None or drop_rig_capability is not None:
        assert_true(
            not (drop_dut_capability is not None and drop_rig_capability is not None),
            "BSC-07 fixture removes one capability at a time",
        )
        inventory_path = Path(fixture["inventory"])
        inventory = json.loads(inventory_path.read_text(encoding="utf-8"))
        alias = "release" if drop_dut_capability is not None else "rig"
        capability = drop_dut_capability or drop_rig_capability
        assert capability is not None
        board = next(item for item in inventory["boards"] if item["alias"] == alias)
        board["capabilities"].remove(capability)
        inventory_path.write_text(json.dumps(inventory), encoding="utf-8")
    command = [
        "python3",
        "-B",
        str(RUNNER),
        "--case",
        "BSC-07",
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
        str(fixture["bsc07_adapter"]),
        "--out-dir",
        str(out_dir),
    ]
    if acknowledge_vbus:
        command.append("--ack-vbus-isolated")
    if production_replay:
        command.append("--production-replay")
    completed = subprocess.run(
        command,
        cwd=ROOT,
        env=environment,
        capture_output=True,
        text=True,
        check=False,
    )
    return completed, out_dir


def run_bsc10_fixture(
    fixture: dict[str, Path | str],
    root: Path,
    *,
    production_replay: bool = False,
    runs: int = 1,
    mutation: str = "",
    drop_dut_capability: str | None = None,
    drop_rig_capability: str | None = None,
) -> tuple[subprocess.CompletedProcess[str], Path]:
    role = "production" if production_replay else "fault"
    out_dir = root / f"bsc10-{role}-out"
    environment = os.environ.copy()
    environment.update(
        {
            "V1SIMPLE_HIL_TEST_HOOKS": "1",
            "FAKE_BSC10_MUTATION": mutation,
        }
    )
    if drop_dut_capability is not None or drop_rig_capability is not None:
        assert_true(
            not (drop_dut_capability is not None and drop_rig_capability is not None),
            "BSC-10 fixture removes one capability at a time",
        )
        inventory_path = Path(fixture["inventory"])
        inventory = json.loads(inventory_path.read_text(encoding="utf-8"))
        alias = "release" if drop_dut_capability is not None else "rig"
        capability = drop_dut_capability or drop_rig_capability
        assert capability is not None
        board = next(item for item in inventory["boards"] if item["alias"] == alias)
        board["capabilities"].remove(capability)
        inventory_path.write_text(json.dumps(inventory), encoding="utf-8")
    command = [
        "python3",
        "-B",
        str(RUNNER),
        "--case",
        "BSC-10",
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
        str(fixture["bsc10_adapter"]),
        "--out-dir",
        str(out_dir),
    ]
    if production_replay:
        command.append("--production-replay")
    completed = subprocess.run(
        command,
        cwd=ROOT,
        env=environment,
        capture_output=True,
        text=True,
        check=False,
    )
    return completed, out_dir


def run_bsc14_fixture(
    fixture: dict[str, Path | str],
    root: Path,
    *,
    production_replay: bool = False,
    runs: int = 1,
    include_reset_acknowledgement: bool = True,
    mutation: str = "",
    drop_dut_touchscreen: bool = False,
    drop_rig_reset_control: bool = False,
) -> tuple[subprocess.CompletedProcess[str], Path]:
    role = "production" if production_replay else "fault"
    out_dir = root / f"bsc14-{role}-out"
    environment = os.environ.copy()
    environment.update(
        {
            "V1SIMPLE_HIL_TEST_HOOKS": "1",
            "FAKE_BSC14_MUTATION": mutation,
        }
    )
    if drop_dut_touchscreen or drop_rig_reset_control:
        inventory_path = Path(fixture["inventory"])
        inventory = json.loads(inventory_path.read_text(encoding="utf-8"))
        alias = "release" if drop_dut_touchscreen else "rig"
        capability = "touchscreen" if drop_dut_touchscreen else "reset-control"
        board = next(item for item in inventory["boards"] if item["alias"] == alias)
        board["capabilities"].remove(capability)
        inventory_path.write_text(json.dumps(inventory), encoding="utf-8")
    command = [
        "python3",
        "-B",
        str(RUNNER),
        "--case",
        "BSC-14",
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
        str(fixture["bsc14_adapter"]),
        "--out-dir",
        str(out_dir),
    ]
    if production_replay:
        command.append("--production-replay")
    if include_reset_acknowledgement and not production_replay:
        command.append("--ack-destructive-hard-cuts")
    completed = subprocess.run(
        command,
        cwd=ROOT,
        env=environment,
        capture_output=True,
        text=True,
        check=False,
    )
    return completed, out_dir


def run_bsc12_fixture(
    fixture: dict[str, Path | str],
    root: Path,
    *,
    runs: int = 1,
    production_replay: bool = False,
    include_vbus_acknowledgement: bool = True,
    include_reset_acknowledgement: bool = True,
    mutation: str = "",
    drop_dut_capability: str | None = None,
    drop_rig_capability: str | None = None,
) -> tuple[subprocess.CompletedProcess[str], Path]:
    out_dir = root / "bsc12-out"
    environment = os.environ.copy()
    environment.update(
        {
            "V1SIMPLE_HIL_TEST_HOOKS": "1",
            "FAKE_BSC12_MUTATION": mutation,
        }
    )
    if drop_dut_capability is not None or drop_rig_capability is not None:
        assert_true(
            not (drop_dut_capability is not None and drop_rig_capability is not None),
            "BSC-12 fixture removes one capability at a time",
        )
        inventory_path = Path(fixture["inventory"])
        inventory = json.loads(inventory_path.read_text(encoding="utf-8"))
        alias = "release" if drop_dut_capability is not None else "rig"
        capability = drop_dut_capability or drop_rig_capability
        assert capability is not None
        board = next(item for item in inventory["boards"] if item["alias"] == alias)
        board["capabilities"].remove(capability)
        inventory_path.write_text(json.dumps(inventory), encoding="utf-8")
    command = [
        "python3",
        "-B",
        str(RUNNER),
        "--case",
        "BSC-12",
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
        str(fixture["bsc12_adapter"]),
        "--out-dir",
        str(out_dir),
    ]
    if production_replay:
        command.append("--production-replay")
    if include_vbus_acknowledgement:
        command.append("--ack-vbus-isolated")
    if include_reset_acknowledgement:
        command.append("--ack-destructive-hard-cuts")
    completed = subprocess.run(
        command,
        cwd=ROOT,
        env=environment,
        capture_output=True,
        text=True,
        check=False,
    )
    return completed, out_dir


def run_bsc16_fixture(
    fixture: dict[str, Path | str],
    root: Path,
    *,
    production_replay: bool = False,
    runs: int = 1,
    include_acknowledgements: bool = True,
    missing_stimulus: bool = False,
    wrong_fact: bool = False,
    high_bounce: bool = False,
    bad_lifecycle: bool = False,
    bool_phase: bool = False,
    replay_fault: bool = False,
    wrong_firmware: bool = False,
    wrong_hil: bool = False,
    reused_evidence: bool = False,
    tampered_evidence: bool = False,
    bool_schema: bool = False,
    integer_hardware: bool = False,
) -> tuple[subprocess.CompletedProcess[str], Path]:
    role = "production" if production_replay else "fault"
    out_dir = root / f"bsc16-{role}-out"
    environment = os.environ.copy()
    environment.update(
        {
            "V1SIMPLE_HIL_TEST_HOOKS": "1",
            "FAKE_BSC16_MISSING_STIMULUS": "1" if missing_stimulus else "0",
            "FAKE_BSC16_WRONG_FACT": "1" if wrong_fact else "0",
            "FAKE_BSC16_HIGH_BOUNCE": "1" if high_bounce else "0",
            "FAKE_BSC16_BAD_LIFECYCLE": "1" if bad_lifecycle else "0",
            "FAKE_BSC16_BOOL_PHASE": "1" if bool_phase else "0",
            "FAKE_BSC16_REPLAY_FAULT": "1" if replay_fault else "0",
            "FAKE_BSC16_WRONG_FIRMWARE": "1" if wrong_firmware else "0",
            "FAKE_BSC16_WRONG_HIL": "1" if wrong_hil else "0",
            "FAKE_BSC16_REUSE": "1" if reused_evidence else "0",
            "FAKE_BSC16_TAMPER": "1" if tampered_evidence else "0",
            "FAKE_BSC16_BOOL_SCHEMA": "1" if bool_schema else "0",
            "FAKE_BSC16_INTEGER_HARDWARE": "1" if integer_hardware else "0",
        }
    )
    command = [
        "python3",
        "-B",
        str(RUNNER),
        "--case",
        "BSC-16",
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
        str(fixture["bsc16_adapter"]),
        "--out-dir",
        str(out_dir),
    ]
    if production_replay:
        command.append("--production-replay")
    if include_acknowledgements:
        command.extend(("--ack-vbus-isolated", "--ack-destructive-hard-cuts"))
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


def test_registered_case_without_rig_adapter_cannot_false_pass() -> None:
    completed = subprocess.run(
        [
            "python3",
            "-B",
            str(RUNNER),
            "--case",
            "BSC-05",
            "--board",
            "release",
            "--rig",
            "alert-rig",
            "--runs",
            "3",
        ],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    assert_true(completed.returncode == 1, completed.stdout + completed.stderr)
    payload = json.loads(completed.stdout)
    assert_true(payload["error"]["code"] == "case_rig_adapter_unavailable", str(payload))


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
            "tracked-rig-adapter-not-implemented" in result["qualification_blockers"],
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


def test_bsc02_runner_limits_match_product_constants() -> None:
    wifi_header = ROOT / "src" / "wifi_manager.h"
    fault_header = (
        ROOT / "src" / "modules" / "wifi" / "wifi_bsc02_hil_fault_module.h"
    )
    product = {
        "trigger_free": read_cpp_integer_constant(
            wifi_header, "WIFI_RUNTIME_MIN_FREE_AP_ONLY"
        ),
        "trigger_largest": read_cpp_integer_constant(
            wifi_header, "WIFI_RUNTIME_MIN_BLOCK_AP_ONLY"
        ),
        "safety_free": read_cpp_integer_constant(
            fault_header, "kPressureSafetyFreeBytes"
        ),
        "safety_largest": read_cpp_integer_constant(
            fault_header, "kPressureSafetyLargestBlockBytes"
        ),
        "absolute_free": read_cpp_integer_constant(
            fault_header, "kAbsoluteMinimumFreeBytes"
        ),
        "absolute_largest": read_cpp_integer_constant(
            fault_header, "kAbsoluteMinimumLargestBlockBytes"
        ),
        "allocation_cap": read_cpp_integer_constant(
            fault_header, "kMaximumPressureBytes"
        ),
        "task_overhead_cap": read_cpp_integer_constant(
            fault_header, "kMaximumPressureTaskOverheadBytes"
        ),
        "auto_release": read_cpp_integer_constant(
            fault_header, "kPressureAutomaticReleaseMs"
        ),
        "low_heap_persist": read_cpp_integer_constant(
            wifi_header, "WIFI_LOW_DMA_PERSIST_MS"
        ),
    }
    runner = {
        "trigger_free": hil_runner.BSC02_FREE_FLOOR_BYTES,
        "trigger_largest": hil_runner.BSC02_LARGEST_BLOCK_FLOOR_BYTES,
        "safety_free": hil_runner.BSC02_SAFETY_FREE_BYTES,
        "safety_largest": hil_runner.BSC02_SAFETY_LARGEST_BLOCK_BYTES,
        "absolute_free": hil_runner.BSC02_ABSOLUTE_MINIMUM_FREE_BYTES,
        "absolute_largest": hil_runner.BSC02_ABSOLUTE_MINIMUM_LARGEST_BLOCK_BYTES,
        "allocation_cap": hil_runner.BSC02_PRESSURE_CAP_BYTES,
        "task_overhead_cap": hil_runner.BSC02_PRESSURE_TASK_OVERHEAD_CAP_BYTES,
        "auto_release": hil_runner.BSC02_AUTO_RELEASE_MAX_MS,
        "low_heap_persist": hil_runner.BSC02_LOW_HEAP_PERSIST_MIN_MS,
    }
    assert_true(runner == product, f"BSC-02 runner/product limit drift: {runner} != {product}")


def test_bsc02_fault_and_production_roles_are_bound_hashed_and_nonqualifying() -> None:
    for production_replay, expected_role, expected_environment, expected_hil in (
        (False, "fault-collection", "waveshare-349-hil", True),
        (True, "production-replay", "waveshare-349", False),
    ):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            fixture = prepare_fixture(root)
            completed, out_dir = run_bsc02_fixture(
                fixture, root, production_replay=production_replay
            )
            assert_true(completed.returncode == 0, completed.stdout + completed.stderr)
            result = json.loads(
                (out_dir / "collection_result.json").read_text(encoding="utf-8")
            )
            assert_true(result["result"] == "TEST_PASS", str(result))
            assert_true(result["collection_role"] == expected_role, str(result))
            assert_true(result["execution_mode"] == "simulated", str(result))
            assert_true(result["authoritative"] is False, str(result))
            assert_true(result["hardware_observed"] is False, str(result))
            assert_true(result["physical_collection_completed"] is False, str(result))
            assert_true(result["non_qualifying"] is True, str(result))
            assert_true(result["qualification_status"] == "BLOCKED", str(result))
            assert_true(
                result["qualification_blockers"]
                == [
                    "build-generator-provenance-not-authenticated",
                    "board-resolution-provenance-not-authenticated",
                    "tracked-rig-adapter-not-implemented",
                ],
                str(result),
            )
            assert_true(result["runs_required"] == result["runs_completed"] == 1, str(result))
            assert_true(
                result["production_replay_required"] is (not production_replay),
                str(result),
            )
            assert_true(
                result["firmware_target"]["environment"] == expected_environment
                and result["firmware_target"]["target_sha"] == fixture["target_sha"]
                and result["firmware_target"]["hil_fault_control_active"] is expected_hil,
                str(result),
            )
            assert_true(
                result["configured_heap_floors"]
                == {
                    "trigger": {"free_bytes": 16384, "largest_block_bytes": 8192},
                    "safety": {"free_bytes": 14848, "largest_block_bytes": 6656},
                    "absolute_minimum": {
                        "free_bytes": 14336,
                        "largest_block_bytes": 6144,
                    },
                },
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


def test_bsc02_rejects_false_state_timing_floor_and_terminal_claims() -> None:
    cases = (
        {"false_ap": True},
        {"rapid_retry": True},
        {"rapid_later_retry": True},
        {"short_guard": True},
        {"low_free": True},
        {"low_block": True},
        {"no_release": True},
        {"no_heap_stop": True},
        {"identity_drift": True},
        {"oversized_overhead": True},
        {"no_maintenance": True},
        {"no_http": True},
        {"runs": 2},
    )
    for options in cases:
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            fixture = prepare_fixture(root)
            completed, out_dir = run_bsc02_fixture(fixture, root, **options)
            assert_true(completed.returncode != 0, f"{options} unexpectedly passed")
            payload = json.loads(completed.stdout)
            expected = "invalid_runs" if options == {"runs": 2} else "case_record_invalid"
            assert_true(payload["error"]["code"] == expected, str(payload))
            assert_true(not (out_dir / "collection_result.json").exists(), str(options))
            assert_true(not (out_dir / "qualification_result.json").exists(), str(options))


def test_bsc02_rejects_identity_replay_fault_and_evidence_tampering() -> None:
    cases = (
        {"wrong_firmware": True},
        {"wrong_hil_marker": True},
        {"reused_evidence": True},
        {"tampered_evidence": True},
        {"production_replay": True, "wrong_firmware": True},
        {"production_replay": True, "wrong_hil_marker": True},
        {"production_replay": True, "replay_fault": True},
        {"production_replay": True, "no_maintenance": True},
        {"production_replay": True, "no_http": True},
    )
    for options in cases:
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            fixture = prepare_fixture(root)
            completed, out_dir = run_bsc02_fixture(fixture, root, **options)
            assert_true(completed.returncode != 0, f"{options} unexpectedly passed")
            payload = json.loads(completed.stdout)
            assert_true(payload["error"]["code"] == "case_record_invalid", str(payload))
            assert_true(not (out_dir / "collection_result.json").exists(), str(options))
            assert_true(not (out_dir / "qualification_result.json").exists(), str(options))


def test_bsc02_physical_mode_remains_blocked_without_tracked_adapter() -> None:
    completed = subprocess.run(
        [
            "python3",
            "-B",
            str(RUNNER),
            "--case",
            "BSC-02",
            "--board",
            "release",
            "--rig",
            "rig",
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
    assert_true(completed.returncode != 0, "physical BSC-02 must remain blocked")
    payload = json.loads(completed.stdout)
    assert_true(payload["error"]["code"] == "case_rig_adapter_unavailable", str(payload))


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
        assert_true(
            result["qualification_blockers"]
            == [
                "build-generator-provenance-not-authenticated",
                "board-resolution-provenance-not-authenticated",
                "tracked-rig-adapter-not-implemented",
            ],
            str(result),
        )
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


def test_bsc04_fault_and_production_roles_are_bound_hashed_and_nonqualifying() -> None:
    for production_replay, expected_role, expected_environment, expected_hil in (
        (False, "fault-collection", "waveshare-349-hil", True),
        (True, "production-replay", "waveshare-349", False),
    ):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            fixture = prepare_fixture(root)
            completed, out_dir = run_bsc04_fixture(
                fixture, root, production_replay=production_replay
            )
            assert_true(completed.returncode == 0, completed.stdout + completed.stderr)
            result = json.loads(
                (out_dir / "collection_result.json").read_text(encoding="utf-8")
            )
            assert_true(result["result"] == "TEST_PASS", str(result))
            assert_true(result["collection_role"] == expected_role, str(result))
            assert_true(result["execution_mode"] == "simulated", str(result))
            assert_true(result["hardware_observed"] is False, str(result))
            assert_true(result["authoritative"] is False, str(result))
            assert_true(result["physical_collection_completed"] is False, str(result))
            assert_true(result["non_qualifying"] is True, str(result))
            assert_true(result["qualification_status"] == "BLOCKED", str(result))
            assert_true(
                result["qualification_blockers"]
                == [
                    "build-generator-provenance-not-authenticated",
                    "board-resolution-provenance-not-authenticated",
                    "tracked-rig-adapter-not-implemented",
                ],
                str(result),
            )
            assert_true(result["runs_required"] == result["runs_completed"] == 1, str(result))
            assert_true(
                result["production_replay_required"] is (not production_replay), str(result)
            )
            assert_true(
                result["firmware_target"]["environment"] == expected_environment
                and result["firmware_target"]["target_sha"] == fixture["target_sha"]
                and result["firmware_target"]["hil_fault_control_active"] is expected_hil,
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
            assert_true(not (out_dir / "qualification_result.json").exists(), str(result))
            public_output = completed.stdout + completed.stderr + json.dumps(result)
            assert_true("SECRET-USB-IDENTITY" not in public_output, public_output)
            assert_true(str(fixture["port"]) not in public_output, public_output)
            assert_true("release" not in public_output and '"rig"' not in public_output, public_output)


def test_bsc04_rejects_missing_timing_disconnect_role_and_tampered_evidence() -> None:
    cases = (
        ({"missing_event": True}, "case_record_invalid"),
        ({"not_same_loop": True}, "case_record_invalid"),
        ({"early_connection": True}, "case_record_invalid"),
        ({"bad_deadline": True}, "case_record_invalid"),
        ({"disconnect": True}, "case_record_invalid"),
        ({"bad_downstream": True}, "case_record_invalid"),
        ({"bad_lifecycle": True}, "case_record_invalid"),
        ({"wrong_firmware": True}, "case_record_invalid"),
        ({"wrong_hil": True}, "case_record_invalid"),
        ({"reused_evidence": True}, "case_record_invalid"),
        ({"tampered_evidence": True}, "case_record_invalid"),
        ({"bool_schema": True}, "case_record_invalid"),
        ({"integer_hardware": True}, "case_record_invalid"),
        ({"production_replay": True, "replay_fault": True}, "case_record_invalid"),
        ({"runs": 2}, "invalid_runs"),
    )
    for options, expected_code in cases:
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            fixture = prepare_fixture(root)
            completed, out_dir = run_bsc04_fixture(fixture, root, **options)
            assert_true(completed.returncode != 0, f"{options} unexpectedly passed")
            payload = json.loads(completed.stdout)
            assert_true(payload["error"]["code"] == expected_code, str(payload))
            assert_true(not (out_dir / "collection_result.json").exists(), str(options))
            assert_true(not (out_dir / "qualification_result.json").exists(), str(options))


def test_bsc04_physical_mode_remains_blocked_without_tracked_adapter() -> None:
    completed = subprocess.run(
        [
            "python3",
            "-B",
            str(RUNNER),
            "--case",
            "BSC-04",
            "--board",
            "release",
            "--rig",
            "rig",
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
    assert_true(completed.returncode != 0, "physical BSC-04 must remain blocked")
    payload = json.loads(completed.stdout)
    assert_true(payload["error"]["code"] == "case_rig_adapter_unavailable", str(payload))


def test_bsc05_hil_fault_and_production_roles_are_three_run_bound_and_nonqualifying() -> None:
    descriptor = hil_runner.load_bsc05_case_descriptor()
    descriptor_sha = hil_runner.bsc05_descriptor_commitment(descriptor)
    assert_true(descriptor["fault_build_required"] is True, str(descriptor))
    assert_true(
        descriptor["scenario"]["build_kind"] == "hil-fault"
        and descriptor["scenario"]["fault_ids"] == ["v1-notification-delay-once"],
        str(descriptor),
    )
    for production_replay, collection_role, profile_role, expected_environment, expected_hil in (
        (False, "fault-collection", "alert-generation-fence", "waveshare-349-hil", True),
        (True, "production-replay", "alert-generation-production-replay", "waveshare-349", False),
    ):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            fixture = prepare_fixture(root)
            completed, out_dir = run_bsc05_fixture(
                fixture, root, production_replay=production_replay
            )
            assert_true(completed.returncode == 0, completed.stdout + completed.stderr)
            result = json.loads((out_dir / "collection_result.json").read_text(encoding="utf-8"))
            assert_true(result["result"] == "TEST_PASS", str(result))
            assert_true(result["collection_role"] == collection_role, str(result))
            assert_true(result["profile_role_id"] == profile_role, str(result))
            assert_true(result["case_descriptor_sha256"] == descriptor_sha, str(result))
            assert_true(result["execution_mode"] == "simulated", str(result))
            assert_true(result["hardware_observed"] is False, str(result))
            assert_true(result["authoritative"] is False, str(result))
            assert_true(result["physical_collection_completed"] is False, str(result))
            assert_true(result["non_qualifying"] is True, str(result))
            assert_true(result["qualification_status"] == "BLOCKED", str(result))
            assert_true(
                result["qualification_blockers"]
                == [
                    "build-generator-provenance-not-authenticated",
                    "board-resolution-provenance-not-authenticated",
                    "tracked-rig-adapter-not-implemented",
                ],
                str(result),
            )
            assert_true(result["runs_required"] == result["runs_completed"] == 3, str(result))
            assert_true(
                result["production_replay_required"] is (not production_replay), str(result)
            )
            assert_true(
                result["firmware_target"]["environment"] == expected_environment
                and result["firmware_target"]["target_sha"] == fixture["target_sha"]
                and result["firmware_target"]["hil_fault_control_active"] is expected_hil
                and result["firmware_target"]["build_kind"]
                == ("production" if production_replay else "hil-fault"),
                str(result),
            )
            assert_true(len(result["run_artifacts"]) == 3, str(result))
            records = [
                json.loads((out_dir / row["artifact"]).read_text(encoding="utf-8"))
                for row in result["run_artifacts"]
            ]
            assert_true([record["run_index"] for record in records] == [1, 2, 3], str(records))
            assert_true(
                all(record["case_descriptor"] == descriptor for record in records), str(records)
            )
            assert_true(
                all(
                    record["generation_timeline"]["old_generation"]
                    != record["generation_timeline"]["new_generation"]
                    and record["generation_timeline"]["unexpected_generation_admissions"] == 0
                    and (
                        record["barriers"] == []
                        if production_replay
                        else record["barriers"][0]["timed_out"] is False
                        and [event["id"] for event in record["fault_lifecycle"]]
                        == ["ready", "fired", "released"]
                        and all(
                            event["fault_id"] == "v1-notification-delay-once"
                            for event in record["fault_lifecycle"]
                        )
                        and record["resets"]
                        == {
                            "expected_kind": "none",
                            "planned": 0,
                            "observed": 0,
                            "unexpected": 0,
                        }
                    )
                    for record in records
                ),
                str(records),
            )
            assert_true(not (out_dir / "qualification_result.json").exists(), str(result))
            public_output = completed.stdout + completed.stderr + json.dumps(result)
            assert_true("SECRET-USB-IDENTITY" not in public_output, public_output)
            assert_true(str(fixture["port"]) not in public_output, public_output)


def test_bsc05_rejects_descriptor_generation_timing_fact_and_evidence_drift() -> None:
    cases = (
        ({"mutation": "descriptor"}, "case_record_invalid"),
        ({"mutation": "descriptor-digest"}, "case_record_invalid"),
        ({"mutation": "role"}, "case_record_invalid"),
        ({"mutation": "stimulus-order"}, "case_record_invalid"),
        ({"mutation": "timeline-order"}, "case_record_invalid"),
        ({"mutation": "shifted-beyond-duration"}, "case_record_invalid"),
        ({"mutation": "negative-elapsed"}, "case_record_invalid"),
        ({"mutation": "generation"}, "case_record_invalid"),
        ({"mutation": "barrier-timeout"}, "case_record_invalid"),
        ({"mutation": "fact"}, "case_record_invalid"),
        ({"mutation": "unexpected-reset"}, "case_record_invalid"),
        ({"mutation": "bool-reset"}, "case_record_invalid"),
        ({"mutation": "fault-production-build"}, "case_record_invalid"),
        ({"mutation": "hil-inactive"}, "case_record_invalid"),
        ({"mutation": "wrong-fault-id"}, "case_record_invalid"),
        ({"mutation": "missing-fault-id"}, "case_record_invalid"),
        ({"mutation": "missing-hil-event"}, "case_record_invalid"),
        ({"mutation": "reordered-hil-events"}, "case_record_invalid"),
        ({"mutation": "substituted-hil-event"}, "case_record_invalid"),
        ({"mutation": "hil-arm"}, "case_record_invalid"),
        ({"mutation": "hil-generation"}, "case_record_invalid"),
        ({"mutation": "hil-timing"}, "case_record_invalid"),
        ({"mutation": "capture-role-reuse"}, "case_record_invalid"),
        ({"mutation": "reuse-captures"}, "case_runs_reused"),
        ({"mutation": "reuse-generations"}, "case_runs_reused"),
        ({"mutation": "mixed-firmware"}, "case_runs_mixed"),
        ({"mutation": "tamper"}, "case_record_invalid"),
        ({"mutation": "bool-schema"}, "case_record_invalid"),
        ({"mutation": "integer-hardware"}, "case_record_invalid"),
        ({"production_replay": True, "mutation": "replay-barrier"}, "case_record_invalid"),
        ({"production_replay": True, "mutation": "replay-fault-id"}, "case_record_invalid"),
        ({"production_replay": True, "mutation": "replay-hil-event"}, "case_record_invalid"),
        ({"production_replay": True, "mutation": "replay-hil-build"}, "case_record_invalid"),
        ({"runs": 2}, "invalid_runs"),
    )
    for options, expected_code in cases:
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            fixture = prepare_fixture(root)
            completed, out_dir = run_bsc05_fixture(fixture, root, **options)
            assert_true(completed.returncode != 0, f"{options} unexpectedly passed")
            payload = json.loads(completed.stdout)
            assert_true(payload["error"]["code"] == expected_code, str(payload))
            assert_true(not (out_dir / "collection_result.json").exists(), str(options))
            assert_true(not (out_dir / "qualification_result.json").exists(), str(options))


def test_bsc05_rejects_each_missing_pinned_hardware_capability() -> None:
    descriptor = hil_runner.load_bsc05_case_descriptor()
    for alias, capability_field in (
        ("release", "required_dut_capabilities"),
        ("rig", "required_rig_capabilities"),
    ):
        capabilities = descriptor[capability_field]
        assert isinstance(capabilities, list)
        for capability in capabilities:
            with tempfile.TemporaryDirectory() as raw:
                root = Path(raw)
                fixture = prepare_fixture(root)
                inventory_path = fixture["inventory"]
                assert isinstance(inventory_path, Path)
                inventory = json.loads(inventory_path.read_text(encoding="utf-8"))
                board = next(row for row in inventory["boards"] if row["alias"] == alias)
                board["capabilities"].remove(capability)
                inventory_path.write_text(json.dumps(inventory), encoding="utf-8")
                completed, out_dir = run_bsc05_fixture(fixture, root)
                assert_true(
                    completed.returncode != 0,
                    f"BSC-05 accepted {alias} without {capability}",
                )
                payload = json.loads(completed.stdout)
                assert_true(payload["error"]["code"] == "case_board_resolution_failed", str(payload))
                assert_true(not (out_dir / "collection_result.json").exists(), capability)


def test_bsc05_physical_mode_remains_blocked_before_discovery_or_mutation() -> None:
    with tempfile.TemporaryDirectory() as raw:
        missing_inventory = Path(raw) / "missing-inventory.json"
        completed = subprocess.run(
            [
                "python3",
                "-B",
                str(RUNNER),
                "--case",
                "BSC-05",
                "--board",
                "release",
                "--rig",
                "rig",
                "--runs",
                "3",
                "--inventory",
                str(missing_inventory),
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
        assert_true(completed.returncode != 0, "physical BSC-05 must remain blocked")
        payload = json.loads(completed.stdout)
        assert_true(payload["error"]["code"] == "case_rig_adapter_unavailable", str(payload))
        assert_true(not missing_inventory.exists(), "physical BSC-05 mutated discovery state")


def test_bsc06_fault_and_production_roles_are_three_run_bound_and_nonqualifying() -> None:
    descriptor = hil_runner.load_bsc06_case_descriptor()
    descriptor_sha = hil_runner.bsc06_descriptor_commitment(descriptor)
    for production_replay, descriptor_key, expected_environment, expected_build, expected_hil in (
        (False, "scenario", "waveshare-349-hil", "hil-fault", True),
        (True, "production_replay", "waveshare-349", "production", False),
    ):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            fixture = prepare_fixture(root)
            completed, out_dir = run_bsc06_fixture(
                fixture, root, production_replay=production_replay
            )
            assert_true(completed.returncode == 0, completed.stdout + completed.stderr)
            result = json.loads((out_dir / "collection_result.json").read_text(encoding="utf-8"))
            assert_true(result["result"] == "TEST_PASS", str(result))
            assert_true(result["collection_role"] == descriptor[descriptor_key]["role_id"], str(result))
            assert_true(result["case_descriptor_sha256"] == descriptor_sha, str(result))
            assert_true(result["execution_mode"] == "simulated", str(result))
            assert_true(result["hardware_observed"] is False, str(result))
            assert_true(result["authoritative"] is False, str(result))
            assert_true(result["physical_collection_completed"] is False, str(result))
            assert_true(result["non_qualifying"] is True, str(result))
            assert_true(result["qualification_status"] == "BLOCKED", str(result))
            assert_true(
                result["qualification_blockers"]
                == [
                    "build-generator-provenance-not-authenticated",
                    "board-resolution-provenance-not-authenticated",
                    "tracked-rig-adapter-not-implemented",
                ],
                str(result),
            )
            assert_true(result["runs_required"] == result["runs_completed"] == 3, str(result))
            assert_true(result["production_replay_required"] is (not production_replay), str(result))
            assert_true(
                result["firmware_target"]["environment"] == expected_environment
                and result["firmware_target"]["build_kind"] == expected_build
                and result["firmware_target"]["target_sha"] == fixture["target_sha"]
                and result["firmware_target"]["hil_fault_control_active"] is expected_hil,
                str(result),
            )
            assert_true(len(result["run_artifacts"]) == 3, str(result))
            records = [
                json.loads((out_dir / row["artifact"]).read_text(encoding="utf-8"))
                for row in result["run_artifacts"]
            ]
            assert_true([record["run_index"] for record in records] == [1, 2, 3], str(records))
            assert_true(all(record["descriptor"] == descriptor[descriptor_key] for record in records), str(records))
            expected_races = ["proxy-attach", "adapter-loss"]
            if not production_replay:
                expected_races.extend(("forget-device", "shutdown"))
            assert_true(
                all(
                    [window["race_id"] for window in record["race_windows"]] == expected_races
                    for record in records
                ),
                str(records),
            )
            assert_true(
                all(
                    all(
                        window["post_cancel_gatt_observed"] is False
                        and window["last_gatt_operation_elapsed_ms"]
                        <= window["cancellation_elapsed_ms"]
                        and window["callback_link_down_generation"]
                        == window["captured_generation"]
                        and window["link_down_elapsed_ms"]
                        <= window["handle_retired_elapsed_ms"]
                        and window["reconnect_count"] == 1
                        and (
                            [event["hil_event"] for event in window["hil_events"]]
                            == ["ready", "fired", "released"]
                            and all(
                                event["fault_id"] == "obd-transport-operation-barrier-once"
                                for event in window["hil_events"]
                            )
                            and (
                                window["cancellation_epoch_after"]
                                == window["cancellation_epoch_before"]
                                and window["hil_events"][2]["link_down_generation"]
                                == window["captured_generation"]
                                if window["release_cause"] == "link-down"
                                else window["cancellation_epoch_after"]
                                > window["cancellation_epoch_before"]
                                and window["hil_events"][2]["link_down_generation"] == 0
                            )
                            and [barrier["id"] for barrier in window["barriers"]]
                            == ["connect-window-ready", "cancel-window-ready"]
                            if not production_replay
                            else window["hil_events"] == [] and window["barriers"] == []
                        )
                        for window in record["race_windows"]
                    )
                    for record in records
                ),
                str(records),
            )
            assert_true(
                all(
                    record["reset_observation"]
                    == {
                        "expected_kind": "none",
                        "expected_count": 0,
                        "unexpected_count": 0,
                        "panic_observed": False,
                        "watchdog_reset_observed": False,
                        "load_prohibited_observed": False,
                    }
                    for record in records
                ),
                str(records),
            )
            generations = [
                window["captured_generation"]
                for record in records
                for window in record["race_windows"]
            ]
            assert_true(len(generations) == len(set(generations)), str(records))
            assert_true(not (out_dir / "qualification_result.json").exists(), str(result))
            public_output = completed.stdout + completed.stderr + json.dumps(result)
            assert_true("SECRET-USB-IDENTITY" not in public_output, public_output)
            assert_true(str(fixture["port"]) not in public_output, public_output)


def test_bsc06_rejects_descriptor_race_timing_role_reuse_and_stale_binding_mutations() -> None:
    cases = (
        ({"mutation": "role"}, "case_record_invalid"),
        ({"mutation": "descriptor"}, "case_record_invalid"),
        ({"mutation": "descriptor-digest"}, "case_record_invalid"),
        ({"mutation": "stimulus-order"}, "case_record_invalid"),
        ({"mutation": "stimulus-timing"}, "case_record_invalid"),
        ({"mutation": "window-order"}, "case_record_invalid"),
        ({"mutation": "wrong-generation"}, "case_record_invalid"),
        ({"mutation": "post-cancel-operation"}, "case_record_invalid"),
        ({"mutation": "negative-last-gatt"}, "case_record_invalid"),
        ({"mutation": "last-gatt-before-window"}, "case_record_invalid"),
        ({"mutation": "overlapping-windows"}, "case_record_invalid"),
        ({"mutation": "utc-duration"}, "case_record_invalid"),
        ({"mutation": "reconnect-cardinality"}, "case_record_invalid"),
        ({"mutation": "aggregate-reconnect"}, "case_record_invalid"),
        ({"mutation": "barrier-missing"}, "case_record_invalid"),
        ({"mutation": "barrier-order"}, "case_record_invalid"),
        ({"mutation": "barrier-timing"}, "case_record_invalid"),
        ({"mutation": "fault-identity"}, "case_record_invalid"),
        ({"mutation": "fault-logical-id"}, "case_record_invalid"),
        ({"mutation": "fault-reason"}, "case_record_invalid"),
        ({"mutation": "fault-completion-schema"}, "case_record_invalid"),
        ({"mutation": "fault-completion-timestamp"}, "case_record_invalid"),
        ({"mutation": "fault-order"}, "case_record_invalid"),
        ({"mutation": "fault-timing"}, "case_record_invalid"),
        ({"mutation": "cancellation-epoch-unchanged"}, "case_record_invalid"),
        ({"mutation": "cancellation-link-generation"}, "case_record_invalid"),
        ({"mutation": "link-down-epoch-advanced"}, "case_record_invalid"),
        ({"mutation": "link-down-generation-zero"}, "case_record_invalid"),
        ({"mutation": "event-operation-bool-zero"}, "case_record_invalid"),
        ({"mutation": "event-operation-bool-one"}, "case_record_invalid"),
        ({"mutation": "event-operation-bool-string"}, "case_record_invalid"),
        ({"mutation": "event-controller-bool-zero"}, "case_record_invalid"),
        ({"mutation": "event-controller-bool-one"}, "case_record_invalid"),
        ({"mutation": "event-controller-bool-string"}, "case_record_invalid"),
        ({"mutation": "wrong-firmware"}, "case_record_invalid"),
        ({"mutation": "wrong-build-role"}, "case_record_invalid"),
        ({"mutation": "wrong-hil"}, "case_record_invalid"),
        ({"mutation": "firmware-hil-bool-zero"}, "case_record_invalid"),
        ({"mutation": "firmware-hil-bool-one"}, "case_record_invalid"),
        ({"mutation": "firmware-hil-bool-string"}, "case_record_invalid"),
        (
            {"production_replay": True, "mutation": "firmware-hil-bool-zero"},
            "case_record_invalid",
        ),
        ({"mutation": "reset-expected"}, "case_record_invalid"),
        ({"mutation": "reset-unexpected"}, "case_record_invalid"),
        ({"mutation": "reset-panic"}, "case_record_invalid"),
        ({"mutation": "reset-watchdog"}, "case_record_invalid"),
        ({"mutation": "reset-load-prohibited"}, "case_record_invalid"),
        ({"mutation": "reset-expected-bool"}, "case_record_invalid"),
        ({"mutation": "reset-unexpected-string"}, "case_record_invalid"),
        ({"mutation": "reset-panic-int"}, "case_record_invalid"),
        ({"mutation": "reset-watchdog-int"}, "case_record_invalid"),
        ({"mutation": "reset-load-prohibited-int"}, "case_record_invalid"),
        ({"production_replay": True, "mutation": "reset-panic"}, "case_record_invalid"),
        ({"mutation": "role-capture-reuse"}, "case_record_invalid"),
        ({"mutation": "stale-run-capture-binding"}, "case_runs_reused"),
        ({"mutation": "reuse-generations"}, "case_runs_reused"),
        ({"mutation": "stale-evidence-binding"}, "case_record_invalid"),
        ({"mutation": "bool-schema"}, "case_record_invalid"),
        ({"mutation": "integer-hardware"}, "case_record_invalid"),
        ({"production_replay": True, "mutation": "production-hil"}, "case_record_invalid"),
        ({"runs": 2}, "invalid_runs"),
    )
    for options, expected_code in cases:
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            fixture = prepare_fixture(root)
            completed, out_dir = run_bsc06_fixture(fixture, root, **options)
            assert_true(completed.returncode != 0, f"{options} unexpectedly passed")
            payload = json.loads(completed.stdout)
            assert_true(payload["error"]["code"] == expected_code, str(payload))
            assert_true(not (out_dir / "collection_result.json").exists(), str(options))
            assert_true(not (out_dir / "qualification_result.json").exists(), str(options))


def test_bsc06_physical_mode_remains_blocked_before_discovery_or_mutation() -> None:
    completed = subprocess.run(
        [
            "python3",
            "-B",
            str(RUNNER),
            "--case",
            "BSC-06",
            "--board",
            "release",
            "--rig",
            "rig",
            "--runs",
            "3",
            "--repo-root",
            "/does-not-exist",
            "--inventory",
            "/does-not-exist/inventory.json",
        ],
        cwd=ROOT,
        env={key: value for key, value in os.environ.items() if key != "V1SIMPLE_HIL_TEST_HOOKS"},
        capture_output=True,
        text=True,
        check=False,
    )
    assert_true(completed.returncode != 0, "physical BSC-06 must remain blocked")
    payload = json.loads(completed.stdout)
    assert_true(payload["error"]["code"] == "case_rig_adapter_unavailable", str(payload))


def test_bsc06_rejects_each_required_inventory_capability_removal() -> None:
    required = (
        ("release", tuple(hil_runner.BSC06_DUT_CAPABILITIES)),
        ("rig", tuple(hil_runner.BSC06_RIG_CAPABILITIES)),
    )
    assert_true([len(capabilities) for _, capabilities in required] == [4, 5], str(required))
    for alias, capabilities in required:
        for capability in capabilities:
            with tempfile.TemporaryDirectory() as raw:
                root = Path(raw)
                fixture = prepare_fixture(root)
                inventory_path = Path(fixture["inventory"])
                inventory = json.loads(inventory_path.read_text(encoding="utf-8"))
                board = next(row for row in inventory["boards"] if row["alias"] == alias)
                board["capabilities"].remove(capability)
                inventory_path.write_text(json.dumps(inventory), encoding="utf-8")

                completed, out_dir = run_bsc06_fixture(fixture, root)
                assert_true(completed.returncode != 0, f"missing {alias}:{capability} unexpectedly passed")
                payload = json.loads(completed.stdout)
                assert_true(payload["error"]["code"] == "case_board_resolution_failed", str(payload))
                assert_true(not (out_dir / "collection_result.json").exists(), str(payload))
                assert_true(not (out_dir / "qualification_result.json").exists(), str(payload))



def test_bsc13_fault_and_production_roles_are_three_run_bound_and_nonqualifying() -> None:
    for production_replay, expected_role, expected_environment, expected_hil in (
        (False, "fault-collection", "waveshare-349-hil", True),
        (True, "production-replay", "waveshare-349", False),
    ):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            fixture = prepare_fixture(root)
            completed, out_dir = run_bsc13_fixture(
                fixture, root, production_replay=production_replay
            )
            assert_true(completed.returncode == 0, completed.stdout + completed.stderr)
            result = json.loads((out_dir / "collection_result.json").read_text(encoding="utf-8"))
            assert_true(result["result"] == "TEST_PASS", str(result))
            assert_true(result["collection_role"] == expected_role, str(result))
            assert_true(result["execution_mode"] == "simulated", str(result))
            assert_true(result["hardware_observed"] is False, str(result))
            assert_true(result["authoritative"] is False, str(result))
            assert_true(result["physical_collection_completed"] is False, str(result))
            assert_true(result["non_qualifying"] is True, str(result))
            assert_true(result["qualification_status"] == "BLOCKED", str(result))
            assert_true(
                result["qualification_blockers"]
                == [
                    "build-generator-provenance-not-authenticated",
                    "board-resolution-provenance-not-authenticated",
                    "tracked-rig-adapter-not-implemented",
                ],
                str(result),
            )
            assert_true(result["runs_required"] == result["runs_completed"] == 3, str(result))
            assert_true(
                result["production_replay_required"] is (not production_replay), str(result)
            )
            assert_true(
                result["critical_window_roles"]
                == ["proxy-takeover", "qualification-disable"],
                str(result),
            )
            assert_true(
                result["firmware_target"]["environment"] == expected_environment
                and result["firmware_target"]["target_sha"] == fixture["target_sha"]
                and result["firmware_target"]["hil_fault_control_active"] is expected_hil,
                str(result),
            )
            assert_true(len(result["run_artifacts"]) == 3, str(result))
            records = [
                json.loads((out_dir / row["artifact"]).read_text(encoding="utf-8"))
                for row in result["run_artifacts"]
            ]
            assert_true([record["run_index"] for record in records] == [1, 2, 3], str(records))
            assert_true(
                all(
                    [window["role"] for window in record["critical_windows"]]
                    == ["proxy-takeover", "qualification-disable"]
                    for record in records
                ),
                str(records),
            )
            assert_true(
                all(
                    all(
                        window["callback_confirmed_link_down"] is True
                        and window["callback_link_down_generation"] == window["captured_generation"]
                        and window["resume_scan_count"] == 1
                        and window["phantom_connected_status_observed"] is False
                        and (
                            [event["id"] for event in window["hil_events"]]
                            == ["ready", "fired", "released"]
                            if not production_replay
                            else window["hil_events"] == []
                        )
                        for window in record["critical_windows"]
                    )
                    for record in records
                ),
                str(records),
            )
            assert_true(not (out_dir / "qualification_result.json").exists(), str(result))
            public_output = completed.stdout + completed.stderr + json.dumps(result)
            assert_true("SECRET-USB-IDENTITY" not in public_output, public_output)
            assert_true(str(fixture["port"]) not in public_output, public_output)


def test_bsc13_rejects_window_descriptor_identity_and_evidence_substitution() -> None:
    cases = (
        ({"missing_window": True}, "case_record_invalid"),
        ({"wrong_window_role": True}, "case_record_invalid"),
        ({"bad_generation": True}, "case_record_invalid"),
        ({"phantom": True}, "case_record_invalid"),
        ({"bad_resume": True}, "case_record_invalid"),
        ({"missing_release": True}, "case_record_invalid"),
        ({"wrong_descriptor": True}, "case_record_invalid"),
        ({"wrong_firmware": True}, "case_record_invalid"),
        ({"wrong_hil": True}, "case_record_invalid"),
        ({"reused_captures": True}, "case_runs_reused"),
        ({"reused_generations": True}, "case_runs_reused"),
        ({"tampered_evidence": True}, "case_record_invalid"),
        ({"bool_schema": True}, "case_record_invalid"),
        ({"integer_hardware": True}, "case_record_invalid"),
        ({"production_replay": True, "replay_hil": True}, "case_record_invalid"),
        ({"runs": 2}, "invalid_runs"),
    )
    for options, expected_code in cases:
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            fixture = prepare_fixture(root)
            completed, out_dir = run_bsc13_fixture(fixture, root, **options)
            assert_true(completed.returncode != 0, f"{options} unexpectedly passed")
            payload = json.loads(completed.stdout)
            assert_true(payload["error"]["code"] == expected_code, str(payload))
            assert_true(not (out_dir / "collection_result.json").exists(), str(options))
            assert_true(not (out_dir / "qualification_result.json").exists(), str(options))


def test_bsc13_physical_mode_remains_blocked_before_rig_mutation() -> None:
    completed = subprocess.run(
        [
            "python3",
            "-B",
            str(RUNNER),
            "--case",
            "BSC-13",
            "--board",
            "release",
            "--rig",
            "rig",
            "--runs",
            "3",
        ],
        cwd=ROOT,
        env={key: value for key, value in os.environ.items() if key != "V1SIMPLE_HIL_TEST_HOOKS"},
        capture_output=True,
        text=True,
        check=False,
    )
    assert_true(completed.returncode != 0, "physical BSC-13 must remain blocked")
    payload = json.loads(completed.stdout)
    assert_true(payload["error"]["code"] == "case_rig_adapter_unavailable", str(payload))


def test_bsc07_profile_v5_production_collector_is_bound_and_nonqualifying() -> None:
    descriptor = hil_runner.load_bsc07_case_descriptor()
    descriptor_sha = hil_runner.bsc07_descriptor_commitment(descriptor)
    with tempfile.TemporaryDirectory() as raw:
        root = Path(raw)
        fixture = prepare_fixture(root)
        completed, out_dir = run_bsc07_fixture(fixture, root)
        assert_true(completed.returncode == 0, completed.stdout + completed.stderr)
        result = json.loads((out_dir / "collection_result.json").read_text(encoding="utf-8"))
        attempt = json.loads((out_dir / "attempt.json").read_text(encoding="utf-8"))
        assert_true(result["result"] == "TEST_PASS", str(result))
        assert_true(result["collection_role"] == "maintenance-power-safety", str(result))
        assert_true(result["case_descriptor_sha256"] == descriptor_sha, str(result))
        assert_true(result["execution_mode"] == "simulated", str(result))
        assert_true(result["hardware_observed"] is False, str(result))
        assert_true(result["authoritative"] is False, str(result))
        assert_true(result["physical_collection_completed"] is False, str(result))
        assert_true(result["non_qualifying"] is True, str(result))
        assert_true(result["qualification_status"] == "BLOCKED", str(result))
        assert_true(
            result["qualification_blockers"]
            == [
                "build-generator-provenance-not-authenticated",
                "board-resolution-provenance-not-authenticated",
                "tracked-rig-adapter-not-implemented",
            ],
            str(result),
        )
        assert_true(result["runs_required"] == result["runs_completed"] == 1, str(result))
        assert_true(result["production_replay_required"] is False, str(result))
        assert_true(
            result["firmware_target"]["environment"] == "waveshare-349"
            and result["firmware_target"]["build_kind"] == "production"
            and result["firmware_target"]["target_sha"] == fixture["target_sha"]
            and result["firmware_target"]["hil_fault_control_active"] is False,
            str(result),
        )
        assert_true(attempt["case_descriptor"] == descriptor, str(attempt))
        assert_true(attempt["case_descriptor_sha256"] == descriptor_sha, str(attempt))
        assert_true(
            [row["id"] for row in attempt["stimuli"]]
            == descriptor["scenario"]["stimulus_ids"],
            str(attempt),
        )
        power = attempt["observations"]["power"]
        assert_true(
            power["vbus_isolated"] is True
            and power["external_power_removed"] is True
            and power["power_removed_elapsed_ms"] == 12500
            and power["source_role"] == "power-timeline",
            str(attempt),
        )
        reset = attempt["reset_observation"]
        assert_true(
            reset["expected_kind"] == "intentional-shutdown"
            and reset["planned_count"] == reset["observed_count"] == 1
            and reset["unexpected_count"] == 0
            and reset["panic_observed"] is False
            and reset["watchdog_reset_observed"] is False
            and reset["shutdown_elapsed_ms"] == reset["observed_elapsed_ms"] == 12500
            and reset["reason"] == "intentional-critical-voltage-shutdown"
            and reset["source_role"] == "reset-summary",
            str(attempt),
        )
        captures = attempt["capture_commitments"]
        assert_true(set(captures) == set(hil_runner.BSC07_CAPTURE_COMMITMENTS), str(captures))
        assert_true(len(set(captures.values())) == len(captures), str(captures))
        manifest = json.loads(
            (out_dir / "raw-artifact-manifest.json").read_text(encoding="utf-8")
        )
        manifest_by_role = {row["role"]: row["sha256"] for row in manifest["artifacts"]}
        assert_true(
            captures
            == {
                field: manifest_by_role[role]
                for field, role in hil_runner.BSC07_CAPTURE_ROLE_BY_FIELD.items()
            },
            str(manifest),
        )
        assert_true(
            attempt["raw_artifact_request_sha256"]
            == manifest["request_commitment_sha256"],
            str(manifest),
        )
        assert_true(not (out_dir / "qualification_result.json").exists(), str(result))
        public_output = completed.stdout + completed.stderr + json.dumps(result)
        assert_true("SECRET-USB-IDENTITY" not in public_output, public_output)
        assert_true(str(fixture["port"]) not in public_output, public_output)


def test_bsc07_rejects_timeline_health_power_reset_and_capture_mutations() -> None:
    descriptor = hil_runner.load_bsc07_case_descriptor()
    mutation_names = (
        "role-id",
        "target",
        "descriptor",
        "descriptor-digest",
        "record-extra",
        "firmware-extra",
        "wrong-firmware",
        "wrong-build",
        "wrong-hil",
        "stimulus-order",
        "stimulus-timing",
        "stimulus-missing",
        "utc-duration",
        "voltage-slow",
        "hold-short",
        "traffic-not-continuous",
        "traffic-int",
        "traffic-before-hold-missing",
        "traffic-after-hold-missing",
        "button-not-handled",
        "grace-short",
        "grace-long",
        "warning-missing",
        "warning-int",
        "health-stale",
        "health-after-shutdown",
        "vbus-not-isolated",
        "vbus-int",
        "power-not-removed",
        "power-removal-time",
        "barrier-missing",
        "barrier-timing",
        "barrier-timeout",
        "reset-kind",
        "reset-count",
        "unexpected-reset",
        "panic",
        "watchdog",
        "reset-bool-count",
        "panic-int",
        "watchdog-int",
        "capture-reuse",
        "bool-schema",
        "integer-hardware",
        "source-power-unbound",
        "source-health-unbound",
        "reset-source-unbound",
        "reset-timing-unbound",
        "raw-observation-mismatch",
        "raw-request-substitution",
    )
    cases: tuple[tuple[dict[str, object], str], ...] = tuple(
        ({"mutation": mutation}, "case_record_invalid") for mutation in mutation_names
    )
    cases += tuple(
        ({"mutation": f"capture-self-binding-{field}"}, "case_record_invalid")
        for field in hil_runner.BSC07_CAPTURE_COMMITMENTS
    )
    cases += (
        ({"mutation": "raw-missing"}, "raw_artifact_set_invalid"),
        ({"mutation": "raw-extra"}, "raw_artifact_set_invalid"),
    )
    cases += (({"runs": 2}, "invalid_runs"),)
    cases += (({"production_replay": True}, "unsupported_mode"),)
    cases += (({"acknowledge_vbus": False}, "operator_preconditions_incomplete"),)
    cases += tuple(
        ({"drop_dut_capability": capability}, "case_board_resolution_failed")
        for capability in descriptor["required_dut_capabilities"]
    )
    cases += tuple(
        ({"drop_rig_capability": capability}, "case_board_resolution_failed")
        for capability in descriptor["required_rig_capabilities"]
    )
    for options, expected_code in cases:
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            fixture = prepare_fixture(root)
            completed, out_dir = run_bsc07_fixture(fixture, root, **options)
            assert_true(completed.returncode != 0, f"{options} unexpectedly passed")
            payload = json.loads(completed.stdout)
            assert_true(payload["error"]["code"] == expected_code, str(payload))
            assert_true(not (out_dir / "collection_result.json").exists(), str(options))
            assert_true(not (out_dir / "qualification_result.json").exists(), str(options))


def test_bsc07_authoritative_mode_blocks_before_git_output_or_discovery() -> None:
    with tempfile.TemporaryDirectory() as raw:
        root = Path(raw)
        out_dir = root / "must-not-exist"
        inventory = root / "missing-inventory.json"
        with mock.patch.object(hil_runner, "read_git_state") as read_git, mock.patch.object(
            hil_runner, "resolve_bsc07_hardware"
        ) as resolve_hardware:
            args = hil_runner.build_parser().parse_args(
                [
                    "--case",
                    "BSC-07",
                    "--board",
                    "opaque-dut",
                    "--rig",
                    "opaque-rig",
                    "--ack-vbus-isolated",
                    "--inventory",
                    str(inventory),
                    "--out-dir",
                    str(out_dir),
                ]
            )
            with mock.patch.object(hil_runner, "test_hooks_enabled", return_value=False):
                try:
                    hil_runner.run_bsc07_case(args)
                except hil_runner.RunnerError as exc:
                    assert_true(exc.code == "case_rig_adapter_unavailable", str(exc))
                else:
                    raise AssertionError("authoritative BSC-07 unexpectedly passed admission")
        read_git.assert_not_called()
        resolve_hardware.assert_not_called()
        assert_true(not out_dir.exists(), "authoritative BSC-07 created output before admission")
        assert_true(not inventory.exists(), "authoritative BSC-07 touched inventory before admission")


def test_bsc10_fault_and_production_roles_are_bound_hashed_and_nonqualifying() -> None:
    descriptor = hil_runner.load_bsc10_case_descriptor()
    descriptor_sha = hil_runner.bsc10_descriptor_commitment(descriptor)
    for production_replay, descriptor_key, expected_environment, expected_hil in (
        (False, "scenario", "waveshare-349-hil", True),
        (True, "production_replay", "waveshare-349", False),
    ):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            fixture = prepare_fixture(root)
            completed, out_dir = run_bsc10_fixture(
                fixture, root, production_replay=production_replay
            )
            assert_true(completed.returncode == 0, completed.stdout + completed.stderr)
            result = json.loads(
                (out_dir / "collection_result.json").read_text(encoding="utf-8")
            )
            attempt = json.loads((out_dir / "attempt.json").read_text(encoding="utf-8"))
            role = descriptor[descriptor_key]
            assert_true(result["result"] == "TEST_PASS", str(result))
            assert_true(result["collection_role"] == role["role_id"], str(result))
            assert_true(result["case_descriptor_sha256"] == descriptor_sha, str(result))
            assert_true(result["execution_mode"] == "simulated", str(result))
            assert_true(result["hardware_observed"] is False, str(result))
            assert_true(result["authoritative"] is False, str(result))
            assert_true(result["physical_collection_completed"] is False, str(result))
            assert_true(result["non_qualifying"] is True, str(result))
            assert_true(result["qualification_status"] == "BLOCKED", str(result))
            assert_true(
                result["qualification_blockers"]
                == [
                    "build-generator-provenance-not-authenticated",
                    "board-resolution-provenance-not-authenticated",
                    "tracked-rig-adapter-not-implemented",
                ],
                str(result),
            )
            assert_true(result["runs_required"] == result["runs_completed"] == 1, str(result))
            assert_true(
                result["production_replay_required"] is (not production_replay), str(result)
            )
            assert_true(
                result["firmware_target"]["environment"] == expected_environment
                and result["firmware_target"]["target_sha"] == fixture["target_sha"]
                and result["firmware_target"]["hil_fault_control_active"] is expected_hil
                and result["firmware_target"]["build_kind"] == role["build_kind"],
                str(result),
            )
            assert_true(attempt["case_descriptor"] == descriptor, str(attempt))
            assert_true(attempt["case_descriptor_sha256"] == descriptor_sha, str(attempt))
            assert_true(attempt["role_id"] == role["role_id"], str(attempt))
            assert_true(
                [row["id"] for row in attempt["stimuli"]] == role["stimulus_ids"],
                str(attempt),
            )
            assert_true([row["id"] for row in attempt["faults"]] == role["fault_ids"], str(attempt))
            assert_true(
                [row["id"] for row in attempt["barriers"]] == role["barrier_ids"],
                str(attempt),
            )
            expected_facts = {
                fact["id"]: fact["expected"] if fact["type"] == "boolean" else fact["minimum"]
                for fact in role["facts"]
            }
            assert_true(attempt["facts"] == expected_facts, str(attempt))
            assert_true(attempt["vbus_isolated"] is False, str(attempt))
            assert_true(
                attempt["resets"]
                == {
                    "expected_kind": role["reset_contract"]["expected_kind"],
                    "planned": role["reset_contract"]["expected_count"],
                    "observed": role["reset_contract"]["expected_count"],
                    "unexpected": role["reset_contract"]["unexpected_count"],
                },
                str(attempt),
            )
            if production_replay:
                assert_true(attempt["faults"] == attempt["barriers"] == [], str(attempt))
            else:
                assert_true(
                    attempt["facts"]["retry-mutation-count"] == 1
                    and attempt["facts"]["persist-count"] == 1
                    and attempt["facts"]["unknown-outcome-followup-mutations"] == 0
                    and attempt["facts"]["authoritative-status-reconciled"] is True,
                    str(attempt),
                )
            assert_true(
                all(
                    len(value) == 64
                    for value in (
                        result["session_sha256"],
                        result["attempt_sha256"],
                        result["case_descriptor_sha256"],
                        result["evidence_binding_sha256"],
                        *result["artifact_sha256"].values(),
                    )
                ),
                str(result),
            )
            assert_true(not (out_dir / "qualification_result.json").exists(), str(result))
            public_output = completed.stdout + completed.stderr + json.dumps(result)
            assert_true("SECRET-USB-IDENTITY" not in public_output, public_output)
            assert_true(str(fixture["port"]) not in public_output, public_output)
            assert_true(
                "release" not in public_output and '"rig"' not in public_output,
                public_output,
            )


def test_bsc10_rejects_full_record_contract_mutations() -> None:
    descriptor = hil_runner.load_bsc10_case_descriptor()
    mutation_cases = (
        ({"mutation": "role-id"}, "case_record_invalid"),
        ({"production_replay": True, "mutation": "role-id"}, "case_record_invalid"),
        ({"mutation": "build-kind"}, "case_record_invalid"),
        ({"production_replay": True, "mutation": "build-kind"}, "case_record_invalid"),
        ({"mutation": "wrong-firmware"}, "case_record_invalid"),
        ({"production_replay": True, "mutation": "wrong-firmware"}, "case_record_invalid"),
        ({"mutation": "wrong-hil"}, "case_record_invalid"),
        ({"production_replay": True, "mutation": "wrong-hil"}, "case_record_invalid"),
        ({"mutation": "vbus"}, "case_record_invalid"),
        ({"production_replay": True, "mutation": "vbus"}, "case_record_invalid"),
        ({"mutation": "stimulus-missing"}, "case_record_invalid"),
        ({"production_replay": True, "mutation": "stimulus-missing"}, "case_record_invalid"),
        ({"mutation": "stimulus-order"}, "case_record_invalid"),
        ({"production_replay": True, "mutation": "stimulus-order"}, "case_record_invalid"),
        ({"mutation": "stimulus-timing"}, "case_record_invalid"),
        ({"production_replay": True, "mutation": "stimulus-timing"}, "case_record_invalid"),
        ({"mutation": "fault-missing"}, "case_record_invalid"),
        ({"mutation": "fault-order"}, "case_record_invalid"),
        ({"mutation": "fault-timing"}, "case_record_invalid"),
        ({"mutation": "response-window"}, "case_record_invalid"),
        ({"mutation": "barrier-missing"}, "case_record_invalid"),
        ({"mutation": "barrier-order"}, "case_record_invalid"),
        ({"mutation": "barrier-timing"}, "case_record_invalid"),
        ({"mutation": "barrier-timeout"}, "case_record_invalid"),
        ({"mutation": "retry-count"}, "case_record_invalid"),
        ({"mutation": "persist-count"}, "case_record_invalid"),
        ({"mutation": "followup-count"}, "case_record_invalid"),
        ({"mutation": "fault-reconciliation"}, "case_record_invalid"),
        ({"production_replay": True, "mutation": "production-success"}, "case_record_invalid"),
        (
            {"production_replay": True, "mutation": "production-reconciliation"},
            "case_record_invalid",
        ),
        ({"production_replay": True, "mutation": "production-hil"}, "case_record_invalid"),
        ({"production_replay": True, "mutation": "production-fault"}, "case_record_invalid"),
        ({"production_replay": True, "mutation": "production-barrier"}, "case_record_invalid"),
        ({"mutation": "reset"}, "case_record_invalid"),
        ({"production_replay": True, "mutation": "reset"}, "case_record_invalid"),
        ({"mutation": "target"}, "case_record_invalid"),
        ({"mutation": "descriptor"}, "case_record_invalid"),
        ({"mutation": "descriptor-digest"}, "case_record_invalid"),
        ({"mutation": "record-extra"}, "case_record_invalid"),
        ({"mutation": "firmware-extra"}, "case_record_invalid"),
        ({"mutation": "fact-extra"}, "case_record_invalid"),
        ({"mutation": "capture-missing"}, "case_record_invalid"),
        ({"mutation": "capture-reuse"}, "case_record_invalid"),
        ({"mutation": "capture-stale-binding-tamper"}, "case_record_invalid"),
        ({"mutation": "bool-schema"}, "case_record_invalid"),
        ({"mutation": "integer-hardware"}, "case_record_invalid"),
        ({"runs": 2}, "invalid_runs"),
    )
    capability_cases = tuple(
        ({"drop_dut_capability": capability}, "case_board_resolution_failed")
        for capability in descriptor["required_dut_capabilities"]
    ) + tuple(
        ({"drop_rig_capability": capability}, "case_board_resolution_failed")
        for capability in descriptor["required_rig_capabilities"]
    )
    cases = mutation_cases + capability_cases
    for options, expected_code in cases:
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            fixture = prepare_fixture(root)
            completed, out_dir = run_bsc10_fixture(fixture, root, **options)
            assert_true(completed.returncode != 0, f"{options} unexpectedly passed")
            payload = json.loads(completed.stdout)
            assert_true(payload["error"]["code"] == expected_code, str(payload))
            assert_true(not (out_dir / "collection_result.json").exists(), str(options))
            assert_true(not (out_dir / "qualification_result.json").exists(), str(options))


def test_bsc10_physical_mode_remains_blocked_before_rig_discovery() -> None:
    completed = subprocess.run(
        [
            "python3",
            "-B",
            str(RUNNER),
            "--case",
            "BSC-10",
            "--board",
            "opaque-dut",
            "--rig",
            "opaque-rig",
            "--inventory",
            "/definitely/not/a/local/inventory.json",
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
    assert_true(completed.returncode != 0, "physical BSC-10 must remain blocked")
    payload = json.loads(completed.stdout)
    assert_true(payload["error"]["code"] == "case_rig_adapter_unavailable", str(payload))


def test_bsc12_profile_v5_shutdown_recovery_is_bound_and_nonqualifying() -> None:
    descriptor = hil_runner.load_bsc12_case_descriptor()
    role = descriptor["scenario"]
    descriptor_sha = hil_runner.bsc12_descriptor_commitment(descriptor)
    with tempfile.TemporaryDirectory() as raw:
        root = Path(raw)
        fixture = prepare_fixture(root)
        completed, out_dir = run_bsc12_fixture(fixture, root)
        assert_true(completed.returncode == 0, completed.stdout + completed.stderr)
        result = json.loads((out_dir / "collection_result.json").read_text(encoding="utf-8"))
        attempt = json.loads((out_dir / "attempt.json").read_text(encoding="utf-8"))
        assert_true(result["result"] == "TEST_PASS", str(result))
        assert_true(result["collection_role"] == "aborted-shutdown-recovery", str(result))
        assert_true(result["case_descriptor_sha256"] == descriptor_sha, str(result))
        assert_true(result["execution_mode"] == "simulated", str(result))
        assert_true(result["hardware_observed"] is False, str(result))
        assert_true(result["authoritative"] is False, str(result))
        assert_true(result["physical_collection_completed"] is False, str(result))
        assert_true(result["non_qualifying"] is True, str(result))
        assert_true(result["qualification_status"] == "BLOCKED", str(result))
        assert_true(
            result["qualification_blockers"]
            == [
                "build-generator-provenance-not-authenticated",
                "board-resolution-provenance-not-authenticated",
                "tracked-rig-adapter-not-implemented",
            ],
            str(result),
        )
        assert_true(result["runs_required"] == result["runs_completed"] == 1, str(result))
        assert_true(result["production_replay_required"] is False, str(result))
        assert_true(result["vbus_isolation_acknowledged"] is True, str(result))
        assert_true(result["destructive_reset_acknowledged"] is True, str(result))
        assert_true(
            result["firmware_target"]
            == {
                "environment": "waveshare-349",
                "build_kind": "production",
                "target_sha": fixture["target_sha"],
                "binary_sha256": attempt["firmware"]["binary_sha256"],
                "hil_fault_control_active": False,
            },
            str(result),
        )
        assert_true(attempt["case_descriptor"] == descriptor, str(attempt))
        assert_true(attempt["descriptor"] == role, str(attempt))
        assert_true(attempt["case_descriptor_sha256"] == descriptor_sha, str(attempt))
        assert_true(attempt["run_index"] == 1, str(attempt))
        assert_true(
            attempt["preconditions"]
            == {"vbus_isolated": True, "destructive_reset_acknowledged": True},
            str(attempt),
        )
        assert_true(
            [row["id"] for row in attempt["stimuli"]] == role["stimulus_ids"],
            str(attempt),
        )
        assert_true(
            [row["id"] for row in attempt["barriers"]] == role["barrier_ids"],
            str(attempt),
        )
        assert_true(
            attempt["shutdown_observation"]["wake_trigger"] == "active-low"
            and attempt["shutdown_observation"]["real_rtc_wake_input"] is True
            and attempt["shutdown_observation"]["power_off_result"] is False
            and attempt["shutdown_observation"]["screen_state"] == "disconnected"
            and attempt["shutdown_observation"]["marker_state"] == "unclean",
            str(attempt),
        )
        assert_true(
            [(row["writer_id"], row["completion_count"]) for row in attempt["writers"]]
            == [("settings", 1), ("bond", 1)],
            str(attempt),
        )
        assert_true(
            all(
                row["duplicate_count"] == 0
                and row["lost_count"] == 0
                and row["stalled"] is False
                for row in attempt["writers"]
            ),
            str(attempt),
        )
        assert_true(
            attempt["persistence"]["session_marker_after_reset"] == "unclean"
            and attempt["persistence"]["setting_mutation_sha256"]
            == attempt["persistence"]["setting_after_reset_sha256"]
            and attempt["persistence"]["bond_mutation_sha256"]
            == attempt["persistence"]["bond_after_reset_sha256"],
            str(attempt),
        )
        assert_true(
            attempt["reset"]["expected_kind"] == "forced-reset"
            and attempt["reset"]["planned"] == attempt["reset"]["observed"] == 1
            and attempt["reset"]["unexpected"] == 0
            and attempt["reset"]["panic_observed"] is False
            and attempt["reset"]["watchdog_reset_observed"] is False
            and attempt["reset"]["load_prohibited_observed"] is False,
            str(attempt),
        )
        expected_facts = {
            fact["id"]: fact["expected"] if fact["type"] == "boolean" else fact["minimum"]
            for fact in role["facts"]
        }
        assert_true(attempt["facts"] == expected_facts, str(attempt))
        assert_true(
            result["raw_artifact_contract"]
            == [
                {
                    "role": artifact.role,
                    "filename": artifact.filename,
                    "maximum_bytes": artifact.maximum_bytes,
                }
                for artifact in hil_runner.rig_adapters.BSC12_RAW_ARTIFACTS
            ],
            str(result),
        )
        assert_true(
            [row["role"] for row in attempt["raw_artifact_manifest"]]
            == [artifact.role for artifact in hil_runner.rig_adapters.BSC12_RAW_ARTIFACTS],
            str(attempt),
        )
        for row in attempt["raw_artifact_manifest"]:
            raw_path = out_dir / "raw" / row["filename"]
            assert_true(raw_path.is_file() and not raw_path.is_symlink(), str(row))
            assert_true(raw_path.stat().st_size == row["bytes"], str(row))
            assert_true(hil_runner.sha256_file(raw_path) == row["sha256"], str(row))
        assert_true(
            result["raw_artifact_sha256"]
            == {row["role"]: row["sha256"] for row in attempt["raw_artifact_manifest"]},
            str(result),
        )
        assert_true(
            attempt["persistence"]["after_readback_elapsed_ms"]
            > attempt["reset"]["boot_observed_elapsed_ms"],
            str(attempt),
        )
        assert_true(
            attempt["safety_observation"]["power_reset_trace_sha256"]
            == attempt["reset"]["power_reset_trace_sha256"]
            == attempt["capture_commitments"]["power_reset_trace_sha256"],
            str(attempt),
        )
        assert_true(
            all(
                len(value) == 64
                for value in (
                    result["session_sha256"],
                    result["attempt_sha256"],
                    result["case_descriptor_sha256"],
                    result["evidence_binding_sha256"],
                    *result["artifact_sha256"].values(),
                )
            ),
            str(result),
        )
        assert_true(not (out_dir / "qualification_result.json").exists(), str(result))
        public_output = completed.stdout + completed.stderr + json.dumps(result)
        assert_true("SECRET-USB-IDENTITY" not in public_output, public_output)
        assert_true(str(fixture["port"]) not in public_output, public_output)
        assert_true("release" not in public_output and '"rig"' not in public_output, public_output)


def test_bsc12_rejects_typed_source_timing_reset_fact_and_capture_mutations() -> None:
    mutation_names = (
        "role",
        "session",
        "attempt",
        "run-index-bool",
        "target",
        "descriptor",
        "descriptor-digest",
        "firmware-environment",
        "firmware-build",
        "firmware-hil",
        "firmware-hil-int",
        "vbus-precondition",
        "reset-precondition",
        "precondition-int",
        "stimulus-missing",
        "stimulus-order",
        "stimulus-negative",
        "stimulus-duration",
        "barrier-id",
        "barrier-source",
        "barrier-order",
        "barrier-timing",
        "barrier-timeout",
        "barrier-duration",
        "handoff-order",
        "wake-source",
        "wake-trigger",
        "wake-not-real",
        "wake-bool-int",
        "poweroff-result",
        "poweroff-bool-int",
        "screen-state",
        "marker-state",
        "marker-timing",
        "writer-missing",
        "writer-source",
        "writer-count-zero",
        "writer-count-two",
        "writer-duplicate",
        "writer-lost",
        "writer-stalled",
        "writer-stalled-int",
        "writer-completion-timing",
        "writer-mutation-timing",
        "persistence-source",
        "setting-lost",
        "bond-lost",
        "post-reset-marker",
        "persistence-digest",
        "fabricated-equal-persistence",
        "preboot-readback",
        "reset-kind",
        "reset-source",
        "reset-planned",
        "reset-observed",
        "reset-unexpected",
        "reset-forced-timing",
        "reset-boot-timing",
        "panic",
        "watchdog",
        "load-prohibited",
        "assertion-only-vbus",
        "assertion-only-wake",
        "assertion-only-reset",
        "assertion-only-crash",
        "serial-writer-missing",
        "serial-writer-timing",
        "panic-int",
        "fact-poweroff",
        "fact-screen",
        "fact-marker",
        "fact-settings-writer",
        "fact-bond-writer",
        "fact-setting",
        "fact-bond",
        "fact-wake",
        "utc-duration",
        "capture-reuse",
        "capture-missing",
        "unrelated-capture",
        "bool-schema",
        "integer-hardware",
        "stale-binding",
    )
    cases = tuple(({"mutation": mutation}, "case_record_invalid") for mutation in mutation_names)
    cases += (
        ({"runs": 2}, "invalid_runs"),
        ({"production_replay": True}, "unsupported_mode"),
        ({"include_vbus_acknowledgement": False}, "operator_preconditions_incomplete"),
        ({"include_reset_acknowledgement": False}, "safety_ack_required"),
    )
    descriptor = hil_runner.load_bsc12_case_descriptor()
    cases += tuple(
        ({"drop_dut_capability": capability}, "case_board_resolution_failed")
        for capability in descriptor["required_dut_capabilities"]
    )
    cases += tuple(
        ({"drop_rig_capability": capability}, "case_board_resolution_failed")
        for capability in descriptor["required_rig_capabilities"]
    )
    for options, expected_code in cases:
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            fixture = prepare_fixture(root)
            completed, out_dir = run_bsc12_fixture(fixture, root, **options)
            assert_true(completed.returncode != 0, f"{options} unexpectedly passed")
            payload = json.loads(completed.stdout)
            assert_true(payload["error"]["code"] == expected_code, str(payload))
            assert_true(not (out_dir / "collection_result.json").exists(), str(options))
            assert_true(not (out_dir / "qualification_result.json").exists(), str(options))


def test_bsc12_authoritative_admission_blocks_before_git_output_or_discovery() -> None:
    with tempfile.TemporaryDirectory() as raw:
        out_dir = Path(raw) / "must-not-exist"
        completed = subprocess.run(
            [
                "python3",
                "-B",
                str(RUNNER),
                "--case",
                "BSC-12",
                "--board",
                "opaque-dut",
                "--rig",
                "opaque-rig",
                "--repo-root",
                "/does-not-exist",
                "--inventory",
                "/does-not-exist/inventory.json",
                "--out-dir",
                str(out_dir),
                "--ack-vbus-isolated",
                "--ack-destructive-hard-cuts",
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
        assert_true(completed.returncode != 0, "physical BSC-12 must remain blocked")
        payload = json.loads(completed.stdout)
        assert_true(payload["error"]["code"] == "case_rig_adapter_unavailable", str(payload))
        assert_true(not out_dir.exists(), str(payload))


def test_bsc14_fault_and_production_roles_are_bound_hashed_and_nonqualifying() -> None:
    descriptor = hil_runner.load_bsc14_case_descriptor()
    descriptor_sha = hil_runner.bsc14_descriptor_commitment(descriptor)
    for production_replay, descriptor_key, expected_environment, expected_hil in (
        (False, "scenario", "waveshare-349-hil", True),
        (True, "production_replay", "waveshare-349", False),
    ):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            fixture = prepare_fixture(root)
            completed, out_dir = run_bsc14_fixture(
                fixture, root, production_replay=production_replay
            )
            assert_true(completed.returncode == 0, completed.stdout + completed.stderr)
            result = json.loads(
                (out_dir / "collection_result.json").read_text(encoding="utf-8")
            )
            attempt = json.loads((out_dir / "attempt.json").read_text(encoding="utf-8"))
            role = descriptor[descriptor_key]
            assert_true(result["result"] == "TEST_PASS", str(result))
            assert_true(result["collection_role"] == role["role_id"], str(result))
            assert_true(result["case_descriptor_sha256"] == descriptor_sha, str(result))
            assert_true(result["execution_mode"] == "simulated", str(result))
            assert_true(result["hardware_observed"] is False, str(result))
            assert_true(result["authoritative"] is False, str(result))
            assert_true(result["physical_collection_completed"] is False, str(result))
            assert_true(result["non_qualifying"] is True, str(result))
            assert_true(result["qualification_status"] == "BLOCKED", str(result))
            assert_true(
                result["qualification_blockers"]
                == [
                    "build-generator-provenance-not-authenticated",
                    "board-resolution-provenance-not-authenticated",
                    "tracked-rig-adapter-not-implemented",
                ],
                str(result),
            )
            assert_true(result["runs_required"] == result["runs_completed"] == 1, str(result))
            assert_true(
                result["production_replay_required"] is (not production_replay), str(result)
            )
            assert_true(
                result["firmware_target"]["environment"] == expected_environment
                and result["firmware_target"]["target_sha"] == fixture["target_sha"]
                and result["firmware_target"]["hil_fault_control_active"] is expected_hil,
                str(result),
            )
            assert_true(result["firmware_target"]["build_kind"] == role["build_kind"], str(result))
            assert_true(attempt["case_descriptor"] == descriptor, str(attempt))
            assert_true(attempt["case_descriptor_sha256"] == descriptor_sha, str(attempt))
            assert_true(attempt["role_id"] == role["role_id"], str(attempt))
            assert_true(
                [row["id"] for row in attempt["stimuli"]] == role["stimulus_ids"],
                str(attempt),
            )
            assert_true([row["id"] for row in attempt["faults"]] == role["fault_ids"], str(attempt))
            assert_true(
                [row["id"] for row in attempt["barriers"]] == role["barrier_ids"],
                str(attempt),
            )
            assert_true(attempt["vbus_isolated"] is False, str(attempt))
            assert_true(
                attempt["resets"]
                == {
                    "expected_kind": role["reset_contract"]["expected_kind"],
                    "planned": role["reset_contract"]["expected_count"],
                    "observed": role["reset_contract"]["expected_count"],
                    "unexpected": role["reset_contract"]["unexpected_count"],
                },
                str(attempt),
            )
            expected_facts = {
                fact["id"]: fact["expected"] if fact["type"] == "boolean" else fact["minimum"]
                for fact in role["facts"]
            }
            assert_true(attempt["facts"] == expected_facts, str(attempt))
            if production_replay:
                assert_true(attempt["stimuli"][-1]["id"] == "use-slow-sd-media", str(attempt))
                assert_true(attempt["faults"] == attempt["barriers"] == [], str(attempt))
                assert_true(attempt["resets"]["observed"] == 0, str(attempt))
            assert_true(
                all(
                    len(value) == 64
                    for value in (
                        result["session_sha256"],
                        result["attempt_sha256"],
                        result["case_descriptor_sha256"],
                        result["evidence_binding_sha256"],
                        *result["artifact_sha256"].values(),
                    )
                ),
                str(result),
            )
            assert_true(not (out_dir / "qualification_result.json").exists(), str(result))
            public_output = completed.stdout + completed.stderr + json.dumps(result)
            assert_true("SECRET-USB-IDENTITY" not in public_output, public_output)
            assert_true(str(fixture["port"]) not in public_output, public_output)
            assert_true(
                "release" not in public_output and '"rig"' not in public_output,
                public_output,
            )


def test_bsc14_rejects_descriptor_capability_observation_and_evidence_drift() -> None:
    cases = (
        ({"mutation": "role-id"}, "case_record_invalid"),
        ({"production_replay": True, "mutation": "role-id"}, "case_record_invalid"),
        ({"mutation": "firmware-build-kind"}, "case_record_invalid"),
        (
            {"production_replay": True, "mutation": "firmware-build-kind"},
            "case_record_invalid",
        ),
        ({"mutation": "vbus-isolated"}, "case_record_invalid"),
        (
            {"production_replay": True, "mutation": "vbus-isolated"},
            "case_record_invalid",
        ),
        ({"mutation": "stimulus-missing"}, "case_record_invalid"),
        (
            {"production_replay": True, "mutation": "stimulus-missing"},
            "case_record_invalid",
        ),
        ({"mutation": "stimulus-order"}, "case_record_invalid"),
        (
            {"production_replay": True, "mutation": "stimulus-order"},
            "case_record_invalid",
        ),
        ({"mutation": "stimulus-time"}, "case_record_invalid"),
        (
            {"production_replay": True, "mutation": "stimulus-time"},
            "case_record_invalid",
        ),
        ({"mutation": "stimulus-result"}, "case_record_invalid"),
        (
            {"production_replay": True, "mutation": "stimulus-result"},
            "case_record_invalid",
        ),
        ({"mutation": "slider-stall"}, "case_record_invalid"),
        ({"mutation": "stealth-stall"}, "case_record_invalid"),
        ({"mutation": "profile-stall"}, "case_record_invalid"),
        ({"mutation": "slider-survival"}, "case_record_invalid"),
        ({"mutation": "stealth-survival"}, "case_record_invalid"),
        ({"mutation": "profile-survival"}, "case_record_invalid"),
        ({"mutation": "backup-count"}, "case_record_invalid"),
        ({"mutation": "backup-latest"}, "case_record_invalid"),
        ({"mutation": "real-touch"}, "case_record_invalid"),
        ({"mutation": "fault-identity"}, "case_record_invalid"),
        ({"mutation": "fault-order"}, "case_record_invalid"),
        ({"mutation": "fault-timing"}, "case_record_invalid"),
        ({"mutation": "barrier-order"}, "case_record_invalid"),
        ({"mutation": "barrier-identity"}, "case_record_invalid"),
        ({"mutation": "barrier-timing"}, "case_record_invalid"),
        ({"mutation": "barrier-timeout"}, "case_record_invalid"),
        ({"mutation": "reset-kind"}, "case_record_invalid"),
        ({"mutation": "reset-count"}, "case_record_invalid"),
        ({"mutation": "unexpected-reset"}, "case_record_invalid"),
        ({"mutation": "wrong-firmware"}, "case_record_invalid"),
        ({"mutation": "wrong-hil"}, "case_record_invalid"),
        ({"mutation": "reuse"}, "case_record_invalid"),
        ({"mutation": "tamper"}, "case_record_invalid"),
        ({"mutation": "bool-schema"}, "case_record_invalid"),
        ({"mutation": "integer-hardware"}, "case_record_invalid"),
        ({"mutation": "descriptor"}, "case_record_invalid"),
        ({"mutation": "descriptor-digest"}, "case_record_invalid"),
        ({"production_replay": True, "mutation": "missing-slow-sd"}, "case_record_invalid"),
        ({"production_replay": True, "mutation": "production-stall"}, "case_record_invalid"),
        ({"production_replay": True, "mutation": "production-backup"}, "case_record_invalid"),
        ({"production_replay": True, "mutation": "production-hil"}, "case_record_invalid"),
        ({"production_replay": True, "mutation": "production-reset"}, "case_record_invalid"),
        ({"production_replay": True, "mutation": "production-fault"}, "case_record_invalid"),
        ({"production_replay": True, "mutation": "production-barrier"}, "case_record_invalid"),
        ({"drop_dut_touchscreen": True}, "case_board_resolution_failed"),
        ({"drop_rig_reset_control": True}, "case_board_resolution_failed"),
        ({"runs": 2}, "invalid_runs"),
        ({"include_reset_acknowledgement": False}, "safety_ack_required"),
    )
    for options, expected_code in cases:
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            fixture = prepare_fixture(root)
            completed, out_dir = run_bsc14_fixture(fixture, root, **options)
            assert_true(completed.returncode != 0, f"{options} unexpectedly passed")
            payload = json.loads(completed.stdout)
            assert_true(payload["error"]["code"] == expected_code, str(payload))
            assert_true(not (out_dir / "collection_result.json").exists(), str(options))
            assert_true(not (out_dir / "qualification_result.json").exists(), str(options))


def test_bsc14_physical_mode_remains_blocked_without_tracked_adapter() -> None:
    completed = subprocess.run(
        [
            "python3",
            "-B",
            str(RUNNER),
            "--case",
            "BSC-14",
            "--board",
            "release",
            "--rig",
            "rig",
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
    assert_true(completed.returncode != 0, "physical BSC-14 must remain blocked")
    payload = json.loads(completed.stdout)
    assert_true(payload["error"]["code"] == "case_rig_adapter_unavailable", str(payload))


def test_bsc16_fault_and_production_roles_are_bound_hashed_and_nonqualifying() -> None:
    for production_replay, expected_role, expected_environment, expected_hil in (
        (False, "fault-collection", "waveshare-349-hil", True),
        (True, "production-replay", "waveshare-349", False),
    ):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            fixture = prepare_fixture(root)
            completed, out_dir = run_bsc16_fixture(
                fixture, root, production_replay=production_replay
            )
            assert_true(completed.returncode == 0, completed.stdout + completed.stderr)
            result = json.loads(
                (out_dir / "collection_result.json").read_text(encoding="utf-8")
            )
            assert_true(result["result"] == "TEST_PASS", str(result))
            assert_true(result["collection_role"] == expected_role, str(result))
            assert_true(result["execution_mode"] == "simulated", str(result))
            assert_true(result["hardware_observed"] is False, str(result))
            assert_true(result["authoritative"] is False, str(result))
            assert_true(result["physical_collection_completed"] is False, str(result))
            assert_true(result["non_qualifying"] is True, str(result))
            assert_true(result["qualification_status"] == "BLOCKED", str(result))
            assert_true(
                result["qualification_blockers"]
                == [
                    "build-generator-provenance-not-authenticated",
                    "board-resolution-provenance-not-authenticated",
                    "tracked-rig-adapter-not-implemented",
                ],
                str(result),
            )
            assert_true(result["runs_required"] == result["runs_completed"] == 1, str(result))
            assert_true(
                result["production_replay_required"] is (not production_replay), str(result)
            )
            assert_true(
                result["firmware_target"]["environment"] == expected_environment
                and result["firmware_target"]["target_sha"] == fixture["target_sha"]
                and result["firmware_target"]["hil_fault_control_active"] is expected_hil,
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
            assert_true(not (out_dir / "qualification_result.json").exists(), str(result))
            public_output = completed.stdout + completed.stderr + json.dumps(result)
            assert_true("SECRET-USB-IDENTITY" not in public_output, public_output)
            assert_true(str(fixture["port"]) not in public_output, public_output)
            assert_true("release" not in public_output and '"rig"' not in public_output, public_output)


def test_bsc16_rejects_missing_unsafe_wrong_role_and_tampered_evidence() -> None:
    cases = (
        ({"missing_stimulus": True}, "case_record_invalid"),
        ({"wrong_fact": True}, "case_record_invalid"),
        ({"high_bounce": True}, "case_record_invalid"),
        ({"bad_lifecycle": True}, "case_record_invalid"),
        ({"bool_phase": True}, "case_record_invalid"),
        ({"wrong_firmware": True}, "case_record_invalid"),
        ({"wrong_hil": True}, "case_record_invalid"),
        ({"reused_evidence": True}, "case_record_invalid"),
        ({"tampered_evidence": True}, "case_record_invalid"),
        ({"bool_schema": True}, "case_record_invalid"),
        ({"integer_hardware": True}, "case_record_invalid"),
        ({"production_replay": True, "replay_fault": True}, "case_record_invalid"),
        ({"runs": 2}, "invalid_runs"),
        ({"include_acknowledgements": False}, "safety_ack_required"),
    )
    for options, expected_code in cases:
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            fixture = prepare_fixture(root)
            completed, out_dir = run_bsc16_fixture(fixture, root, **options)
            assert_true(completed.returncode != 0, f"{options} unexpectedly passed")
            payload = json.loads(completed.stdout)
            assert_true(payload["error"]["code"] == expected_code, str(payload))
            assert_true(not (out_dir / "collection_result.json").exists(), str(options))
            assert_true(not (out_dir / "qualification_result.json").exists(), str(options))


def test_bsc16_physical_mode_remains_blocked_without_tracked_adapter() -> None:
    completed = subprocess.run(
        [
            "python3",
            "-B",
            str(RUNNER),
            "--case",
            "BSC-16",
            "--board",
            "release",
            "--rig",
            "rig",
            "--ack-vbus-isolated",
            "--ack-destructive-hard-cuts",
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
    assert_true(completed.returncode != 0, "physical BSC-16 must remain blocked")
    payload = json.loads(completed.stdout)
    assert_true(payload["error"]["code"] == "case_rig_adapter_unavailable", str(payload))


def make_bsc08_record(
    run_index: int = 1,
    *,
    physical: bool = True,
    target_sha: str = "a" * 40,
    adapter: object | None = None,
    adapter_source_sha256: str = "c" * 64,
) -> tuple[
    dict[str, object],
    dict[str, object],
    dict[str, object],
    dict[str, object],
    dict[str, bytes],
]:
    if adapter is None:
        adapter = hil_runner._bsc08_test_adapter_descriptor(
            hil_runner.rig_adapters.get_rig_adapter("BSC-08")
        )
    profile, errors = hil_runner.qualification.load_pinned_profile()
    assert_true(profile is not None and not errors, str(errors))
    request = hil_runner.adapter_protocol.build_adapter_request(
        adapter=adapter,
        target_sha=target_sha,
        profile_id=profile["profile_id"],
        profile_version=profile["profile_version"],
        profile_sha256=hil_runner.qualification.PINNED_PROFILE_SHA256,
        adapter_source_sha256=adapter_source_sha256,
        role_id="proxy-epoch-teardown",
        run_index=run_index,
        session_id="bsc08-session",
        attempt_id=f"bsc08-attempt-{run_index}",
        nonce=f"{run_index:032x}",
        dut_alias="release",
        dut_capabilities=hil_runner.BSC08_DUT_CAPABILITIES,
        rig_alias="rig",
        rig_capabilities=hil_runner.BSC08_RIG_CAPABILITIES,
        firmware_binary_sha256="b" * 64,
    )
    old_epoch = 100 + (run_index * 10)
    new_epoch = old_epoch + 1
    elapsed = (10, 20, 30, 40, 50, 60, 70)
    rows = (
        (old_epoch, old_epoch, [10, 10], [8, 8], [0, 0], [1, 0, 0, 0], False, False, [100000, 50000]),
        (old_epoch, old_epoch, [11, 11], [9, 9], [0, 0], [1, 0, 0, 0], False, False, [99900, 50100]),
        (old_epoch, 0, [12, 12], [9, 9], [0, 0], [1, 1, 1, 0], True, True, [99850, 50050]),
        (new_epoch, new_epoch, [12, 12], [9, 9], [0, 0], [2, 1, 1, 1], True, True, [99950, 50150]),
        (new_epoch, new_epoch, [13, 13], [9, 9], [1, 1], [2, 1, 1, 1], True, True, [99800, 50000]),
        (new_epoch, new_epoch, [14, 14], [10, 10], [1, 1], [2, 1, 1, 1], True, True, [99900, 50100]),
        (new_epoch, new_epoch, [14, 14], [10, 10], [1, 1], [2, 1, 1, 1], True, True, [99875, 50075]),
    )
    queries: list[dict[str, object]] = []
    for sequence, (phase, at_ms, row) in enumerate(
        zip(hil_runner.BSC08_SNAPSHOT_PHASES, elapsed, rows, strict=True), start=1
    ):
        epoch, gate, entries, admissions, stale, lifecycle, overlap, release, heap = row
        nonce = hil_runner.bsc08_serial_nonce(request["nonce"], phase)
        queries.append(
            {
                "phase": phase,
                "sequence": sequence,
                "elapsed_ms": at_ms,
                "nonce": nonce,
                "response": {
                    "schema": 1,
                    "nonce": nonce,
                    "status": "ready",
                    "epoch": epoch,
                    "gateEpoch": gate,
                    "active": 0,
                    "callbackEntries": entries,
                    "admissions": admissions,
                    "staleRejects": stale,
                    "lifecycle": lifecycle,
                    "activeOverlap": overlap,
                    "releaseOpportunity": release,
                    "oldForwarded": False,
                    "proxyQueue": [1, 0, 1, 8],
                    "phoneQueue": [1, 0, 1, 16],
                    "heap": heap,
                },
            }
        )
    phase_elapsed = {query["phase"]: query["elapsed_ms"] for query in queries}
    safety = {
        "panic_observed": False,
        "watchdog_reset_observed": False,
        "load_prohibited_observed": False,
    }
    request_commitment = request["request_commitment_sha256"]
    payloads = {
        phase: {
            direction: hashlib.sha256(
                f"bsc08-{run_index}-{phase}-{direction}".encode("ascii")
            ).hexdigest()
            for direction in ("proxy_to_v1", "v1_to_proxy")
        }
        for phase in ("old-traffic", "fresh-traffic")
    }
    serial_rows = [
        {
            "schema_version": 1,
            "phase": query["phase"],
            "sequence": query["sequence"],
            "elapsed_ms": query["elapsed_ms"],
            "nonce": query["nonce"],
            "response_line": "QBSC08 "
            + json.dumps(query["response"], separators=(",", ":"), sort_keys=True),
        }
        for query in queries
    ]
    transcript_rows = []
    for sequence, (stimulus_id, phase) in enumerate(
        zip(
            hil_runner.BSC08_STIMULUS_IDS,
            ("streaming", "disabled", "reenabled", "old-traffic", "fresh-traffic"),
            strict=True,
        ),
        start=1,
    ):
        transcript_rows.append(
            {
                "schema_version": 1,
                "request_commitment_sha256": request_commitment,
                "event": "stimulus",
                "id": stimulus_id,
                "sequence": sequence,
                "phase": phase,
                "elapsed_ms": phase_elapsed[phase],
                "observed": True,
                "payload_sha256": payloads.get(
                    phase, {"proxy_to_v1": None, "v1_to_proxy": None}
                ),
            }
        )
    fresh_epoch = queries[5]["response"]["epoch"]
    raw_content = {
        "adapter-transcript": b"".join(
            hil_runner.adapter_protocol.canonical_json_bytes(row) + b"\n"
            for row in transcript_rows
        ),
        "firmware-build": hil_runner.adapter_protocol.canonical_json_bytes(
            {
                "schema_version": 1,
                "request_commitment_sha256": request_commitment,
                "environment": "waveshare-349",
                "build_kind": "production",
                "target_sha": target_sha,
                "binary_sha256": "b" * 64,
                "hil_fault_control_active": False,
            }
        )
        + b"\n",
        "proxy-peer-receipts": hil_runner.adapter_protocol.canonical_json_bytes(
            {
                "schema_version": 1,
                "request_commitment_sha256": request_commitment,
                "receipt_id": f"receipt-proxy-{run_index}",
                "direction": "v1-to-proxy",
                "phase": "fresh-traffic",
                "epoch": fresh_epoch,
                "payload_sha256": payloads["fresh-traffic"]["v1_to_proxy"],
                "delivered": True,
                "elapsed_ms": 65,
            }
        )
        + b"\n",
        "safety-summary": hil_runner.adapter_protocol.canonical_json_bytes(
            {
                "schema_version": 1,
                "request_commitment_sha256": request_commitment,
                "safety": safety,
                "resets": {"expected_kind": "none", "planned": 0, "observed": 0, "unexpected": 0},
            }
        )
        + b"\n",
        "serial-log": b"".join(
            hil_runner.adapter_protocol.canonical_json_bytes(row) + b"\n"
            for row in serial_rows
        ),
        "v1-peer-receipts": hil_runner.adapter_protocol.canonical_json_bytes(
            {
                "schema_version": 1,
                "request_commitment_sha256": request_commitment,
                "receipt_id": f"receipt-v1-{run_index}",
                "direction": "proxy-to-v1",
                "phase": "fresh-traffic",
                "epoch": fresh_epoch,
                "payload_sha256": payloads["fresh-traffic"]["proxy_to_v1"],
                "delivered": True,
                "elapsed_ms": 66,
            }
        )
        + b"\n",
    }
    raw_manifest: dict[str, object] = {
        "schema_version": hil_runner.adapter_protocol.SCHEMA_VERSION,
        "protocol_version": hil_runner.rig_adapters.ADAPTER_PROTOCOL_VERSION,
        "request_commitment_sha256": request_commitment,
        "artifacts": [
            {
                "filename": contract.filename,
                "role": contract.role,
                "sha256": hashlib.sha256(raw_content[contract.role]).hexdigest(),
                "size_bytes": len(raw_content[contract.role]),
            }
            for contract in hil_runner.rig_adapters.BSC08_RAW_ARTIFACTS
        ],
    }
    raw_manifest["manifest_commitment_sha256"] = hil_runner.adapter_protocol.canonical_commitment(
        hil_runner.adapter_protocol.MANIFEST_DOMAIN, raw_manifest
    )
    expected = {
        "case_id": "BSC-08",
        "role_id": "proxy-epoch-teardown",
        "session_id": "bsc08-session",
        "attempt_id": f"bsc08-attempt-{run_index}",
        "run_index": run_index,
        "target_sha": target_sha,
        "dut_alias": "release",
        "rig_alias": "rig",
        "execution_mode": "physical" if physical else "simulated",
        "hardware_observed": physical,
        "request_commitment_sha256": request_commitment,
    }
    record = hil_runner.build_bsc08_record_from_raw(
        expected=expected,
        request=request,
        raw_manifest=raw_manifest,
        raw_content=raw_content,
        started_at_utc="2026-07-22T20:00:00Z",
        completed_at_utc="2026-07-22T20:01:00Z",
    )
    return record, expected, request, raw_manifest, raw_content


def rebind_bsc08(record: dict[str, object]) -> None:
    record["source_capture_binding_sha256"] = hil_runner.bsc08_source_capture_commitment(record)
    record["evidence_binding_sha256"] = hil_runner.bsc08_record_commitment(record)


def test_bsc08_typed_record_is_profile_v5_bound_and_simulation_is_nonqualifying() -> None:
    record, expected, request, manifest, content = make_bsc08_record()
    validated = hil_runner.validate_bsc08_record(
        record, expected=expected, raw_manifest=manifest, raw_content=content, request=request
    )
    assert_true(validated is record, "physical BSC-08 record did not validate")

    simulated, simulated_expected, simulated_request, simulated_manifest, simulated_content = make_bsc08_record(physical=False)
    validated_simulated = hil_runner.validate_bsc08_record(
        simulated,
        expected=simulated_expected,
        raw_manifest=simulated_manifest,
        raw_content=simulated_content,
        request=simulated_request,
        qualifying=False,
    )
    assert_true(validated_simulated is simulated, "non-qualifying simulation should remain testable")
    try:
        hil_runner.validate_bsc08_record(
            simulated,
            expected=simulated_expected,
            raw_manifest=simulated_manifest,
            raw_content=simulated_content,
            request=simulated_request,
        )
    except hil_runner.RunnerError as exc:
        assert_true(exc.code == "case_evidence_nonqualifying", exc.code)
    else:
        raise AssertionError("simulated BSC-08 evidence qualified")

    records = []
    for run_index in range(1, 4):
        current, current_expected, current_request, current_manifest, current_content = make_bsc08_record(run_index)
        hil_runner.validate_bsc08_record(
            current,
            expected=current_expected,
            raw_manifest=current_manifest,
            raw_content=current_content,
            request=current_request,
        )
        records.append(current)
    hil_runner.validate_bsc08_distinct_runs(records)


def test_bsc08_runner_hashes_and_reverifies_serial_capture_bytes() -> None:
    _, expected, _, _, raw_content = make_bsc08_record()
    with tempfile.TemporaryDirectory() as temp_name:
        root = Path(temp_name)
        raw_directory = root / "raw"
        raw_directory.mkdir()
        for contract in hil_runner.rig_adapters.BSC08_RAW_ARTIFACTS:
            (raw_directory / contract.filename).write_bytes(raw_content[contract.role])
        manifest = hil_runner.collect_bsc08_raw_artifact_manifest(
            raw_directory=raw_directory,
            request_commitment_sha256=expected["request_commitment_sha256"],
            manifest_path=root / "manifest.json",
        )
        captured = hil_runner.adapter_protocol.read_collected_raw_artifacts(
            raw_directory=raw_directory,
            role=hil_runner._bsc08_role_contract(),
            manifest=manifest,
        )
        assert_true(captured == raw_content, "BSC-08 runner did not retain every verified raw role")
        serial_path = raw_directory / "serial.log"
        serial_path.write_bytes(raw_content["serial-log"] + b"{}\n")
        try:
            hil_runner.adapter_protocol.read_collected_raw_artifacts(
                raw_directory=raw_directory,
                role=hil_runner._bsc08_role_contract(),
                manifest=manifest,
            )
        except hil_runner.adapter_protocol.AdapterProtocolError as exc:
            assert_true(exc.code == "raw_artifact_changed", exc.code)
        else:
            raise AssertionError("BSC-08 accepted a serial capture changed after manifest hashing")


def test_bsc08_rejects_adversarial_nonce_epoch_race_fact_and_capture_mutations() -> None:
    record, expected, request, manifest, content = make_bsc08_record()
    try:
        hil_runner.validate_bsc08_record(
            record, expected=expected, raw_manifest=manifest, raw_content={}, request=request
        )
    except hil_runner.RunnerError:
        pass
    else:
        raise AssertionError("BSC-08 accepted fabricated manifest-only evidence without raw bytes")

    undelivered = copy.deepcopy(content)
    receipt = json.loads(undelivered["proxy-peer-receipts"])
    receipt["delivered"] = False
    undelivered["proxy-peer-receipts"] = hil_runner.adapter_protocol.canonical_json_bytes(receipt) + b"\n"
    undelivered_manifest = copy.deepcopy(manifest)
    for row in undelivered_manifest["artifacts"]:
        if row["role"] == "proxy-peer-receipts":
            row["sha256"] = hashlib.sha256(undelivered["proxy-peer-receipts"]).hexdigest()
            row["size_bytes"] = len(undelivered["proxy-peer-receipts"])
    committed = dict(undelivered_manifest)
    committed.pop("manifest_commitment_sha256")
    undelivered_manifest["manifest_commitment_sha256"] = hil_runner.adapter_protocol.canonical_commitment(
        hil_runner.adapter_protocol.MANIFEST_DOMAIN, committed
    )
    try:
        hil_runner.build_bsc08_record_from_raw(
            expected=expected,
            request=request,
            raw_manifest=undelivered_manifest,
            raw_content=undelivered,
            started_at_utc="2026-07-22T20:00:00Z",
            completed_at_utc="2026-07-22T20:01:00Z",
        )
    except hil_runner.RunnerError as exc:
        assert_true(exc.code == "case_incomplete", exc.code)
    else:
        raise AssertionError("BSC-08 enqueue counters qualified without a delivered peer receipt")

    record["evidence_binding_sha256"] = "f" * 64
    try:
        hil_runner.validate_bsc08_record(
            record,
            expected=expected,
            raw_manifest=manifest,
            raw_content=content,
            request=request,
        )
    except hil_runner.RunnerError as exc:
        assert_true(exc.code == "case_record_invalid", exc.code)
    else:
        raise AssertionError("BSC-08 stale binding passed")


def test_bsc08_authenticated_tracked_adapter_is_executed() -> None:
    with tempfile.TemporaryDirectory() as temp_name:
        root = Path(temp_name)
        repository = root / "repository"
        repository.mkdir()
        initialize_repository(repository)
        adapter_relative = Path("scripts/bug_squash_hil_bsc08_test_adapter.py")
        adapter_path = repository / adapter_relative
        adapter_path.parent.mkdir()
        write_executable(
            adapter_path,
            """#!/usr/bin/env python3
import json
import os
from pathlib import Path
import sys
import time

request = json.load(sys.stdin)
Path(os.environ["BSC08_ACTIVATION_MARKER"]).write_text(
    json.dumps({"argv": sys.argv[1:], "request": request["request_commitment_sha256"]}),
    encoding="utf-8",
)
time.sleep(0.12)
json.dump(
    {
        "schema_version": 1,
        "protocol_version": 1,
        "status": "complete",
        "request_commitment_sha256": request["request_commitment_sha256"],
        "nonce": request["nonce"],
    },
    sys.stdout,
)
""",
        )
        subprocess.run(["git", "add", adapter_relative.as_posix()], cwd=repository, check=True)
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
                "tracked BSC-08 adapter",
            ],
            cwd=repository,
            check=True,
        )
        target_sha = subprocess.run(
            ["git", "rev-parse", "HEAD"],
            cwd=repository,
            capture_output=True,
            text=True,
            check=True,
        ).stdout.strip()
        adapter = hil_runner._bsc08_test_adapter_descriptor(
            hil_runner.rig_adapters.get_rig_adapter("BSC-08")
        )
        args = mock.Mock(case_adapter=None, repo_root=repository)
        with (
            mock.patch.object(hil_runner, "test_hooks_enabled", return_value=False),
            mock.patch.object(
                hil_runner.rig_adapters,
                "get_rig_adapter",
                return_value=adapter,
            ),
        ):
            role_descriptor = hil_runner.bsc08_profile_descriptor()
            admission = hil_runner.admit_case_rig_adapter(
                args,
                case_contract={
                    "id": hil_runner.BSC08_CASE_ID,
                    "minimum_runs": hil_runner.BSC08_REQUIRED_RUNS,
                    "required_dut_capabilities": list(hil_runner.BSC08_DUT_CAPABILITIES),
                    "required_rig_capabilities": list(hil_runner.BSC08_RIG_CAPABILITIES),
                    "scenario": role_descriptor,
                    "production_replay": None,
                },
                role_id="proxy-epoch-teardown",
            )
        assert_true(admission.simulated is False, "tracked BSC-08 adapter was not authenticated")
        assert_true(admission.git_state is not None, "tracked BSC-08 target was not bound")
        assert_true(admission.source_sha256 is not None, "tracked BSC-08 source was not hashed")

        _, expected, request, _, raw_content = make_bsc08_record(
            target_sha=target_sha,
            adapter=adapter,
            adapter_source_sha256=admission.source_sha256,
        )
        raw_directory = root / "raw"
        raw_directory.mkdir()
        for contract in hil_runner.rig_adapters.BSC08_RAW_ARTIFACTS:
            (raw_directory / contract.filename).write_bytes(raw_content[contract.role])
        marker = root / "adapter-activated.json"
        environment = dict(os.environ)
        environment["BSC08_ACTIVATION_MARKER"] = str(marker)
        record, _ = hil_runner.run_bsc08_adapter(
            adapter_path=adapter_path,
            adapter=adapter,
            repository=repository,
            request=request,
            expected=expected,
            raw_directory=raw_directory,
            raw_manifest_path=root / "raw-artifact-manifest.json",
            environment=environment,
        )
        activation = json.loads(marker.read_text(encoding="utf-8"))
        assert_true(
            activation["request"] == request["request_commitment_sha256"],
            "authenticated BSC-08 adapter did not receive its bound request",
        )
        assert_true(
            activation["argv"]
            == ["--entrypoint", "main", "--raw-artifact-dir", str(raw_directory)],
            "authenticated BSC-08 adapter was not called through the shared protocol",
        )
        assert_true(record["hardware_observed"] is True, "authenticated BSC-08 path lost physical typing")


def test_bsc08_physical_mode_remains_blocked_before_hardware_discovery() -> None:
    completed = subprocess.run(
        [
            "python3",
            "-B",
            str(RUNNER),
            "--case",
            "BSC-08",
            "--board",
            "release",
            "--rig",
            "rig",
            "--runs",
            "3",
        ],
        cwd=ROOT,
        env={key: value for key, value in os.environ.items() if key != "V1SIMPLE_HIL_TEST_HOOKS"},
        capture_output=True,
        text=True,
        check=False,
    )
    assert_true(completed.returncode != 0, "physical BSC-08 must remain blocked")
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
    test_registered_case_without_rig_adapter_cannot_false_pass()
    test_bsc03_simulation_is_three_run_bound_and_explicitly_nonqualifying()
    test_bsc03_rejects_missing_preconditions_timing_and_reused_runs()
    test_bsc03_resume_requires_recovery_and_never_counts_interrupted_attempt()
    test_bsc02_runner_limits_match_product_constants()
    test_bsc02_fault_and_production_roles_are_bound_hashed_and_nonqualifying()
    test_bsc02_rejects_false_state_timing_floor_and_terminal_claims()
    test_bsc02_rejects_identity_replay_fault_and_evidence_tampering()
    test_bsc02_physical_mode_remains_blocked_without_tracked_adapter()
    test_bsc11_simulation_is_one_run_bound_hashed_and_nonqualifying()
    test_bsc11_rejects_short_wrong_target_and_missing_operator_boundary()
    test_bsc11_rejects_every_portable_shutdown_action_and_service_loss()
    test_bsc11_rejects_service_evidence_that_does_not_span_the_hold()
    test_bsc11_rejects_usb_powerdown_reused_and_tampered_evidence()
    test_bsc11_physical_mode_remains_blocked_without_tracked_adapter()
    test_bsc04_fault_and_production_roles_are_bound_hashed_and_nonqualifying()
    test_bsc04_rejects_missing_timing_disconnect_role_and_tampered_evidence()
    test_bsc04_physical_mode_remains_blocked_without_tracked_adapter()
    test_bsc05_hil_fault_and_production_roles_are_three_run_bound_and_nonqualifying()
    test_bsc05_rejects_descriptor_generation_timing_fact_and_evidence_drift()
    test_bsc05_rejects_each_missing_pinned_hardware_capability()
    test_bsc05_physical_mode_remains_blocked_before_discovery_or_mutation()
    test_bsc06_fault_and_production_roles_are_three_run_bound_and_nonqualifying()
    test_bsc06_rejects_descriptor_race_timing_role_reuse_and_stale_binding_mutations()
    test_bsc06_physical_mode_remains_blocked_before_discovery_or_mutation()
    test_bsc06_rejects_each_required_inventory_capability_removal()
    test_bsc08_typed_record_is_profile_v5_bound_and_simulation_is_nonqualifying()
    test_bsc08_runner_hashes_and_reverifies_serial_capture_bytes()
    test_bsc08_rejects_adversarial_nonce_epoch_race_fact_and_capture_mutations()
    test_bsc08_authenticated_tracked_adapter_is_executed()
    test_bsc08_physical_mode_remains_blocked_before_hardware_discovery()
    test_bsc13_fault_and_production_roles_are_three_run_bound_and_nonqualifying()
    test_bsc13_rejects_window_descriptor_identity_and_evidence_substitution()
    test_bsc13_physical_mode_remains_blocked_before_rig_mutation()
    test_bsc07_profile_v5_production_collector_is_bound_and_nonqualifying()
    test_bsc07_rejects_timeline_health_power_reset_and_capture_mutations()
    test_bsc07_authoritative_mode_blocks_before_git_output_or_discovery()
    test_bsc10_fault_and_production_roles_are_bound_hashed_and_nonqualifying()
    test_bsc10_rejects_full_record_contract_mutations()
    test_bsc10_physical_mode_remains_blocked_before_rig_discovery()
    test_bsc12_profile_v5_shutdown_recovery_is_bound_and_nonqualifying()
    test_bsc12_rejects_typed_source_timing_reset_fact_and_capture_mutations()
    test_bsc12_authoritative_admission_blocks_before_git_output_or_discovery()
    test_bsc14_fault_and_production_roles_are_bound_hashed_and_nonqualifying()
    test_bsc14_rejects_descriptor_capability_observation_and_evidence_drift()
    test_bsc14_physical_mode_remains_blocked_without_tracked_adapter()
    test_bsc16_fault_and_production_roles_are_bound_hashed_and_nonqualifying()
    test_bsc16_rejects_missing_unsafe_wrong_role_and_tampered_evidence()
    test_bsc16_physical_mode_remains_blocked_without_tracked_adapter()
    test_authoritative_mode_rejects_tool_path_overrides()
    test_git_and_child_environment_overrides_are_ignored()
    print("bug-squash HIL runner regression tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Regression tests for the fail-closed bug-squash HIL runner."""

from __future__ import annotations

import ast
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
                            "firmware-execution",
                            "maintenance-mode",
                            "obd-connectivity",
                            "persistence",
                            "power-button",
                            "proxy-connectivity",
                            "serial",
                            "touchscreen",
                            "v1-connectivity",
                        ],
                        "usb_serial": "SECRET-USB-IDENTITY",
                    },
                    {
                        "alias": "rig",
                        "capabilities": [
                            "artifact-capture",
                            "battery-source",
                            "bond-peer",
                            "ignition-control",
                            "lan-client",
                            "logic-analyzer",
                            "obd-peer",
                            "power-control",
                            "power-button",
                            "proxy-client",
                            "reset-control",
                            "sd-media",
                            "sram-pressure-control",
                            "utc-time-source",
                            "usb-source",
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
if mutation == 'fault-id' and is_fault:
    faults[0]['id'] = 'invented-fault'
if mutation == 'barrier-order' and is_fault:
    barriers[0]['id'], barriers[1]['id'] = barriers[1]['id'], barriers[0]['id']
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
        "bsc13_adapter": bsc13_adapter,
        "bsc11_adapter": bsc11_adapter,
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
            "hil-fault-control-not-implemented" in result["qualification_blockers"],
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
        ({"mutation": "stimulus-order"}, "case_record_invalid"),
        ({"mutation": "slider-stall"}, "case_record_invalid"),
        ({"mutation": "stealth-stall"}, "case_record_invalid"),
        ({"mutation": "profile-stall"}, "case_record_invalid"),
        ({"mutation": "slider-survival"}, "case_record_invalid"),
        ({"mutation": "stealth-survival"}, "case_record_invalid"),
        ({"mutation": "profile-survival"}, "case_record_invalid"),
        ({"mutation": "backup-count"}, "case_record_invalid"),
        ({"mutation": "backup-latest"}, "case_record_invalid"),
        ({"mutation": "real-touch"}, "case_record_invalid"),
        ({"mutation": "fault-id"}, "case_record_invalid"),
        ({"mutation": "barrier-order"}, "case_record_invalid"),
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
    test_bsc13_fault_and_production_roles_are_three_run_bound_and_nonqualifying()
    test_bsc13_rejects_window_descriptor_identity_and_evidence_substitution()
    test_bsc13_physical_mode_remains_blocked_before_rig_mutation()
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

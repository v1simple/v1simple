#!/usr/bin/env python3
"""Regression tests for the hardware device-suite runner shell script."""

from __future__ import annotations

import json
import os
import re
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RUNNER = ROOT / "scripts" / "run_device_tests.sh"
PLATFORMIO_INI = ROOT / "platformio.ini"


def assert_true(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def write_fake_pio(path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        r'''#!/usr/bin/env bash
set -euo pipefail

if [[ "${1:-}" != "test" ]]; then
  echo "fake pio only supports 'test'" >&2
  exit 2
fi

json_path=""
xml_path=""
suite=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    -f)
      suite="$2"
      shift
      ;;
    --json-output-path)
      json_path="$2"
      shift
      ;;
    --junit-output-path)
      xml_path="$2"
      shift
      ;;
  esac
  shift
done

if [[ -z "$json_path" || -z "$xml_path" || -z "$suite" ]]; then
  echo "missing fake pio output arguments" >&2
  exit 2
fi

mkdir -p "$(dirname "$json_path")" "$(dirname "$xml_path")"
python3 - "$json_path" "$xml_path" "$suite" <<'PY_FAKE_PIO'
import json
import os
import sys
from pathlib import Path

json_path = Path(sys.argv[1])
xml_path = Path(sys.argv[2])
suite = sys.argv[3]
bad_metric = os.environ.get("FAKE_PIO_BAD_METRIC") == "1"
zero_exit_error = os.environ.get("FAKE_PIO_ZERO_EXIT_ERROR") == "1"
partial_skip = os.environ.get("FAKE_PIO_PARTIAL_SKIP") == "1"
test_cases = (
    [
        {"name": "fixture-pass", "status": "PASSED"},
        {"name": "fixture-skip", "status": "SKIPPED"},
    ]
    if partial_skip
    else [
        {
            "name": "fixture",
            "status": "ERRORED" if zero_exit_error else "PASSED",
        }
    ]
)

payload = {
    "test_suites": [
        {
            "env_name": "device",
            "test_name": suite,
            "status": "ERRORED" if zero_exit_error else "PASSED",
            "testcase_nums": len(test_cases),
            "skipped_nums": 1 if partial_skip else 0,
            "failure_nums": 0,
            "error_nums": 1 if zero_exit_error else 0,
            "duration": 0.01,
            "test_cases": test_cases,
        }
    ]
}
json_path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
xml_path.write_text(
    f'<testsuites tests="{len(test_cases)}" failures="0" errors="0">'
    f'<testsuite name="device:collected-but-not-selected" tests="0" failures="0" errors="0" skipped="0" />'
    f'<testsuite name="device:{suite}" tests="{len(test_cases)}" failures="0" errors="0" skipped="{1 if partial_skip else 0}" />'
    f'</testsuites>\n',
    encoding="utf-8",
)

metrics_by_suite = {
    "test_device_boot": {
        "internal_free_bytes": 1 if bad_metric else 200000,
        "internal_largest_block_bytes": 100000,
        "psram_size_bytes": 8 * 1024 * 1024,
        "free_sketch_bytes": 2 * 1024 * 1024,
        "main_stack_high_water_bytes": 4096,
    },
    "test_device_heap": {
        "baseline_internal_free_bytes": 190000,
        "baseline_internal_largest_block_bytes": 90000,
        "internal_alloc_recovery_delta_bytes": 0,
        "spiram_alloc_recovery_delta_bytes": 0,
    },
}

print(f"fake pio running {suite}")
for metric, value in metrics_by_suite.get(suite, {}).items():
    print(json.dumps({"metric": metric, "value": value, "unit": "count", "tags": {}}))
PY_FAKE_PIO
if [[ "${FAKE_PIO_INFRA_EXIT:-0}" == "1" ]]; then
  exit 9
fi
''',
        encoding="utf-8",
    )
    path.chmod(0o755)


def run_device_tests(
    tmp_dir: Path,
    *,
    bad_metric: bool = False,
    infra_exit: bool = False,
    zero_exit_error: bool = False,
    partial_skip: bool = False,
    compare_to: Path | None = None,
) -> tuple[subprocess.CompletedProcess[str], Path]:
    fake_bin = tmp_dir / "bin"
    write_fake_pio(fake_bin / "pio")
    fake_port = tmp_dir / "ttyFAKE0"
    fake_port.write_text("", encoding="utf-8")
    out_dir = tmp_dir / "out"

    env = os.environ.copy()
    env.update(
        {
            "DEVICE_PORT": str(fake_port),
            "DEVICE_BOARD_ID": "regression",
            "PIO_CMD": str(fake_bin / "pio"),
            "PLATFORMIO_SKIP_CA_BOOTSTRAP": "1",
            "PATH": f"{fake_bin}{os.pathsep}{env.get('PATH', '')}",
        }
    )
    if bad_metric:
        env["FAKE_PIO_BAD_METRIC"] = "1"
    if infra_exit:
        env["FAKE_PIO_INFRA_EXIT"] = "1"
        env["DEVICE_FAIL_CLOSED_TRANSPORT"] = "1"
    if zero_exit_error:
        env["FAKE_PIO_ZERO_EXIT_ERROR"] = "1"
    if partial_skip:
        env["FAKE_PIO_PARTIAL_SKIP"] = "1"

    command = [str(RUNNER), "--quick", "--cooldown-seconds", "0", "--out-dir", str(out_dir)]
    if compare_to is not None:
        command.extend(["--compare-to", str(compare_to)])

    completed = subprocess.run(
        command,
        cwd=ROOT,
        env=env,
        capture_output=True,
        text=True,
        check=False,
    )

    return completed, out_dir


def test_empty_compare_args_passes_on_nounset_shells() -> None:
    with tempfile.TemporaryDirectory() as tmp_dir_raw:
        completed, out_dir = run_device_tests(Path(tmp_dir_raw))
        assert_true(
            completed.returncode == 0,
            f"device runner should pass without --compare-to; stdout={completed.stdout}\nstderr={completed.stderr}",
        )

        manifest = json.loads((out_dir / "manifest.json").read_text(encoding="utf-8"))
        assert_true(len(manifest["git_sha"]) == 40, f"git SHA must be full: {manifest}")
        assert_true(manifest["base_result"] == "PASS", f"unexpected suite base result: {manifest}")
        assert_true(manifest["result"] == "NO_BASELINE", f"unexpected manifest result: {manifest}")
        assert_true((out_dir / "scoring.json").exists(), "scoring.json should be written")
        assert_true((out_dir / "summary.md").exists(), "summary.md should be written")


def test_scorer_hard_failure_controls_exit_status() -> None:
    with tempfile.TemporaryDirectory() as tmp_dir_raw:
        completed, out_dir = run_device_tests(Path(tmp_dir_raw), bad_metric=True)
        assert_true(completed.returncode != 0, "hard scorer failure should make device runner exit non-zero")
        manifest = json.loads((out_dir / "manifest.json").read_text(encoding="utf-8"))
        scoring = json.loads((out_dir / "scoring.json").read_text(encoding="utf-8"))
        assert_true(manifest["base_result"] == "PASS", f"suite base result should remain PASS: {manifest}")
        assert_true(manifest["result"] == "FAIL", f"manifest should reflect scorer failure: {manifest}")
        assert_true(scoring["result"] == "FAIL", f"scoring should fail: {scoring}")


def test_legacy_device_manifests_retain_baseline_comparison() -> None:
    with tempfile.TemporaryDirectory() as tmp_dir_raw:
        root = Path(tmp_dir_raw)
        baseline_completed, baseline_out = run_device_tests(root / "baseline")
        assert_true(
            baseline_completed.returncode == 0,
            baseline_completed.stdout + baseline_completed.stderr,
        )
        baseline_manifest = baseline_out / "manifest.json"
        baseline_payload = json.loads(baseline_manifest.read_text(encoding="utf-8"))
        assert_true(
            "hardware_scoring_fingerprint" not in baseline_payload,
            f"legacy fixture unexpectedly declared a scoring identity: {baseline_payload}",
        )

        current_completed, current_out = run_device_tests(
            root / "current",
            compare_to=baseline_manifest,
        )
        assert_true(
            current_completed.returncode == 0,
            current_completed.stdout + current_completed.stderr,
        )
        scoring = json.loads((current_out / "scoring.json").read_text(encoding="utf-8"))
        assert_true(scoring["comparison_kind"] == "run_variance", str(scoring))
        assert_true(scoring["baseline_window"]["candidate_count"] == 1, str(scoring))


def test_fail_closed_transport_rejects_nonzero_pio_exit() -> None:
    with tempfile.TemporaryDirectory() as tmp_dir_raw:
        completed, out_dir = run_device_tests(Path(tmp_dir_raw), infra_exit=True)
        assert_true(completed.returncode != 0, "fail-closed transport must reject nonzero pio exit")
        manifest = json.loads((out_dir / "manifest.json").read_text(encoding="utf-8"))
        first = manifest["suite_results"][0]
        assert_true(first["status"] == "INFRA_ERROR", f"transport status must be explicit: {manifest}")
        assert_true(manifest["base_result"] == "FAIL", f"base result must fail: {manifest}")


def test_zero_exit_with_reported_test_error_fails() -> None:
    with tempfile.TemporaryDirectory() as tmp_dir_raw:
        completed, out_dir = run_device_tests(Path(tmp_dir_raw), zero_exit_error=True)
        assert_true(completed.returncode != 0, "reported test errors must fail even on exit zero")
        manifest = json.loads((out_dir / "manifest.json").read_text(encoding="utf-8"))
        first = manifest["suite_results"][0]
        assert_true(first["status"] == "PASS", f"raw row remains transport-oriented: {manifest}")
        assert_true(manifest["base_result"] == "FAIL", f"base result must fail: {manifest}")
        assert_true("ERROR: test_device_boot" in completed.stdout, completed.stdout)
        assert_true("ERROR: unknown" not in completed.stdout, completed.stdout)


def test_partial_skip_uses_real_platformio_counters_and_passes() -> None:
    with tempfile.TemporaryDirectory() as tmp_dir_raw:
        completed, out_dir = run_device_tests(
            Path(tmp_dir_raw),
            partial_skip=True,
        )
        assert_true(completed.returncode == 0, completed.stdout + completed.stderr)
        manifest = json.loads((out_dir / "manifest.json").read_text(encoding="utf-8"))
        assert_true(manifest["base_result"] == "PASS", str(manifest))
        assert_true("Tests: 2" in completed.stdout, completed.stdout)


def test_device_lane_owns_only_the_three_surviving_suites() -> None:
    runner = RUNNER.read_text(encoding="utf-8")
    device_env = PLATFORMIO_INI.read_text(encoding="utf-8").split("[env:device]", 1)[1]
    surviving = {"test_device_boot", "test_device_heap", "test_device_event_bus"}

    assert_true("test_filter =" in device_env, "device env must use an explicit ownership filter")
    assert_true("test_ignore =" not in device_env, "device env must not use a broad ignore list")
    assert_true(set(re.findall(r"\btest_device_[a-z0-9_]+\b", runner)) == surviving,
                "device runner suite ownership must match the surviving set exactly")
    assert_true(set(re.findall(r"\btest_device_[a-z0-9_]+\b", device_env)) == surviving,
                "device PlatformIO filter must match the surviving set exactly")
    assert_true('SUITES=("${CORE_SUITES[@]}")' in runner,
                "quick mode must select only the core boot/heap suites")
    assert_true(runner.count('SUITES=("${CORE_SUITES[@]}" "${CONCURRENCY_SUITES[@]}")') == 1,
                "default mode must select core plus event-bus suites exactly once")
    assert_true("--full" not in runner, "duplicate full/default device mode returned")
    assert_true(runner.count("score_hardware_run.py") == 1, "device results must be scored once")
    expected_usage = "[--quick] [--cooldown-seconds N] [--compare-to PATH ...] [--out-dir PATH]"
    assert_true(runner.count(expected_usage) == 2,
                "device runner help and error usage must expose only the supported option shape")


def main() -> int:
    test_empty_compare_args_passes_on_nounset_shells()
    test_scorer_hard_failure_controls_exit_status()
    test_legacy_device_manifests_retain_baseline_comparison()
    test_fail_closed_transport_rejects_nonzero_pio_exit()
    test_zero_exit_with_reported_test_error_fails()
    test_partial_skip_uses_real_platformio_counters_and_passes()
    test_device_lane_owns_only_the_three_surviving_suites()
    print("run_device_tests.sh regression tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

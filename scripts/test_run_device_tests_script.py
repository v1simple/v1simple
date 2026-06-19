#!/usr/bin/env python3
"""Regression tests for the hardware device-suite runner shell script."""

from __future__ import annotations

import json
import os
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RUNNER = ROOT / "scripts" / "run_device_tests.sh"


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

payload = {
    "test_suites": [
        {
            "env_name": "device",
            "test_suite_name": suite,
            "status": "PASSED",
            "testcase_nums": 1,
            "pass_nums": 1,
            "failure_nums": 0,
            "error_nums": 0,
            "duration": 0.01,
        }
    ]
}
json_path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
xml_path.write_text(
    f'<testsuites><testsuite name="device:{suite}" tests="1" failures="0" errors="0" /></testsuites>\n',
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
''',
        encoding="utf-8",
    )
    path.chmod(0o755)


def run_device_tests(tmp_dir: Path, *, bad_metric: bool = False) -> tuple[subprocess.CompletedProcess[str], Path]:
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

    completed = subprocess.run(
        [str(RUNNER), "--quick", "--cooldown-seconds", "0", "--out-dir", str(out_dir)],
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


def main() -> int:
    test_empty_compare_args_passes_on_nounset_shells()
    test_scorer_hard_failure_controls_exit_status()
    print("run_device_tests.sh regression tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

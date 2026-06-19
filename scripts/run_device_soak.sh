#!/usr/bin/env bash
#
# run_device_soak.sh - Run repeated hardware device test cycles and collect
# stability/resource metrics from real board logs.
#
# Usage examples:
#   ./scripts/run_device_soak.sh --cycles 20
#   ./scripts/run_device_soak.sh --cycles 50 --cooldown-seconds 6
#   ./scripts/run_device_soak.sh --cycles 10 --quick --stop-on-fail
#
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

CYCLES=20
STOP_ON_FAIL=0
RUN_ARGS=()
OUT_DIR=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --cycles)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --cycles" >&2
        exit 2
      fi
      CYCLES="$2"
      shift
      ;;
    --quick|--full|--stress)
      RUN_ARGS+=("$1")
      ;;
    --cooldown-seconds)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --cooldown-seconds" >&2
        exit 2
      fi
      RUN_ARGS+=("$1" "$2")
      shift
      ;;
    --stop-on-fail)
      STOP_ON_FAIL=1
      ;;
    --out-dir)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --out-dir" >&2
        exit 2
      fi
      OUT_DIR="$2"
      shift
      ;;
    -h|--help)
      echo "Usage: $0 [options]"
      echo ""
      echo "Options:"
      echo "  --cycles N             Number of soak cycles to run (default: 20)"
      echo "  --quick                Pass-through to run_device_tests.sh"
      echo "  --full                 Pass-through to run_device_tests.sh"
      echo "  --stress               Pass-through to run_device_tests.sh"
      echo "  --cooldown-seconds N   Pass-through to run_device_tests.sh"
      echo "  --stop-on-fail         Stop immediately on first failed cycle"
      echo "  --out-dir PATH         Write soak artifacts to PATH"
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      echo "Usage: $0 [--cycles N] [--quick|--full] [--stress] [--cooldown-seconds N] [--stop-on-fail] [--out-dir PATH]" >&2
      exit 2
      ;;
  esac
  shift
done

if ! [[ "$CYCLES" =~ ^[0-9]+$ ]] || [[ "$CYCLES" -lt 1 ]]; then
  echo "Invalid --cycles value '$CYCLES' (expected positive integer)." >&2
  exit 2
fi

if [[ ! -x "$ROOT_DIR/scripts/run_device_tests.sh" ]]; then
  echo "Missing executable: $ROOT_DIR/scripts/run_device_tests.sh" >&2
  exit 1
fi

if [[ -z "$OUT_DIR" ]]; then
  timestamp="$(date +%Y%m%d_%H%M%S)"
  OUT_DIR="$ROOT_DIR/.artifacts/test_reports/device_soak_$timestamp"
fi
mkdir -p "$OUT_DIR"

SOAK_LOG="$OUT_DIR/soak.log"
CSV_PATH="$OUT_DIR/cycles.csv"
SUMMARY_MD="$OUT_DIR/summary.md"
CYCLE_JSONL="$OUT_DIR/cycles.jsonl"

: > "$SOAK_LOG"
: > "$CYCLE_JSONL"
printf "cycle,start_utc,end_utc,duration_s,command_status,base_status,failed_suite,report_dir,manifest_path,scoring_path,scoring_result,comparison_kind,metrics_scored,hard_failures,advisory_failures,info_regressions,missing_required,missing_optional\n" > "$CSV_PATH"

SOAK_START_UTC="$(date -u +"%Y-%m-%dT%H:%M:%SZ")"
SOAK_START_EPOCH="$(date +%s)"

pass_cycles=0
fail_cycles=0
completed_cycles=0
total_duration_s=0
previous_manifest=""

echo "==> Starting device soak run"
echo "    cycles: $CYCLES"
echo "    run args: ${RUN_ARGS[*]:-(none)}"
echo "    out dir: $OUT_DIR"
echo ""

for cycle in $(seq 1 "$CYCLES"); do
  cycle_start_utc="$(date -u +"%Y-%m-%dT%H:%M:%SZ")"
  cycle_start_epoch="$(date +%s)"
  cycle_log="$OUT_DIR/cycle_${cycle}.log"
  device_cmd=(./scripts/run_device_tests.sh)

  echo "==> [cycle $cycle/$CYCLES] starting at $cycle_start_utc"

  if [[ ${#RUN_ARGS[@]} -gt 0 ]]; then
    device_cmd+=("${RUN_ARGS[@]}")
  fi
  set +e
  if [[ -n "$previous_manifest" ]]; then
    device_cmd+=(--compare-to "$previous_manifest")
    "${device_cmd[@]}" 2>&1 | tee "$cycle_log"
  else
    "${device_cmd[@]}" 2>&1 | tee "$cycle_log"
  fi
  cmd_status=${PIPESTATUS[0]}
  set -e

  cat "$cycle_log" >> "$SOAK_LOG"

  cycle_end_utc="$(date -u +"%Y-%m-%dT%H:%M:%SZ")"
  cycle_end_epoch="$(date +%s)"
  cycle_duration_s=$((cycle_end_epoch - cycle_start_epoch))
  total_duration_s=$((total_duration_s + cycle_duration_s))
  completed_cycles=$((completed_cycles + 1))

  report_dir="$(awk -F'Reports written to: ' '/Reports written to:/ { path=$2 } END { print path }' "$cycle_log" | xargs)"
  failed_suite="$(awk -F'Device test run stopped at: ' '/Device test run stopped at:/ { suite=$2 } END { print suite }' "$cycle_log" | xargs)"
  manifest_path=""
  scoring_path=""
  if [[ -n "$report_dir" && -f "$report_dir/manifest.json" ]]; then
    manifest_path="$report_dir/manifest.json"
  fi
  if [[ -n "$report_dir" && -f "$report_dir/scoring.json" ]]; then
    scoring_path="$report_dir/scoring.json"
  fi

  status_word="PASS"
  if [[ "$cmd_status" -eq 0 ]]; then
    pass_cycles=$((pass_cycles + 1))
  else
    status_word="FAIL"
    fail_cycles=$((fail_cycles + 1))
  fi

  cycle_summary_json="$OUT_DIR/cycle_${cycle}.json"
  python3 - "$cycle_summary_json" "$cycle" "$cycle_start_utc" "$cycle_end_utc" "$cycle_duration_s" "$cmd_status" "$status_word" "$failed_suite" "$report_dir" "$manifest_path" "$scoring_path" <<'PY'
import json
import sys
from pathlib import Path

out_path = Path(sys.argv[1])
cycle = int(sys.argv[2])
start_utc = sys.argv[3]
end_utc = sys.argv[4]
duration_s = int(sys.argv[5])
command_status = int(sys.argv[6])
base_status = sys.argv[7]
failed_suite = sys.argv[8]
report_dir = sys.argv[9]
manifest_path = Path(sys.argv[10]) if sys.argv[10] else None
scoring_path = Path(sys.argv[11]) if sys.argv[11] else None

payload = {
    "cycle": cycle,
    "start_utc": start_utc,
    "end_utc": end_utc,
    "duration_s": duration_s,
    "command_status": command_status,
    "base_status": base_status,
    "failed_suite": failed_suite,
    "report_dir": report_dir,
    "manifest_path": str(manifest_path) if manifest_path else "",
    "scoring_path": str(scoring_path) if scoring_path else "",
    "scoring_result": "",
    "comparison_kind": "",
    "metrics_scored": 0,
    "hard_failures": 0,
    "advisory_failures": 0,
    "info_regressions": 0,
    "missing_required": 0,
    "missing_optional": 0,
}

if scoring_path and scoring_path.exists():
    scoring = json.loads(scoring_path.read_text(encoding="utf-8"))
    summary = scoring.get("summary", {})
    payload["scoring_result"] = scoring.get("result", "")
    payload["comparison_kind"] = scoring.get("comparison_kind", "")
    payload["metrics_scored"] = int(summary.get("metrics_scored", 0))
    payload["hard_failures"] = int(summary.get("hard_failures", 0))
    payload["advisory_failures"] = int(summary.get("advisory_failures", 0))
    payload["info_regressions"] = int(summary.get("info_regressions", 0))
    payload["missing_required"] = int(summary.get("missing_required", 0))
    payload["missing_optional"] = int(summary.get("missing_optional", 0))

out_path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
PY
  cycle_summary="$(cat "$cycle_summary_json")"
  printf "%s\n" "$cycle_summary" >> "$CYCLE_JSONL"

  cycle_fields="$(python3 - "$cycle_summary_json" <<'PY'
import json
import sys
from pathlib import Path

payload = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
print(
    "\t".join(
        str(payload[key]) for key in [
            "cycle",
            "start_utc",
            "end_utc",
            "duration_s",
            "command_status",
            "base_status",
            "failed_suite",
            "report_dir",
            "manifest_path",
            "scoring_path",
            "scoring_result",
            "comparison_kind",
            "metrics_scored",
            "hard_failures",
            "advisory_failures",
            "info_regressions",
            "missing_required",
            "missing_optional",
        ]
    )
)
PY
)"
  printf "%s\n" "$cycle_fields" | tr '\t' ',' >> "$CSV_PATH"
  read -r cycle_scoring_result cycle_comparison_kind <<<"$(python3 - "$cycle_summary_json" <<'PY'
import json
import sys
from pathlib import Path

payload = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
print((payload["scoring_result"] or "n/a") + " " + (payload["comparison_kind"] or "n/a"))
PY
)"

  echo "==> [cycle $cycle/$CYCLES] $status_word exit=$cmd_status duration=${cycle_duration_s}s scoring=${cycle_scoring_result} compare=${cycle_comparison_kind}"
  echo ""

  if [[ -n "$manifest_path" ]]; then
    previous_manifest="$manifest_path"
  fi

  if [[ "$cmd_status" -ne 0 && "$STOP_ON_FAIL" -eq 1 ]]; then
    echo "Stopping soak early due to --stop-on-fail." >&2
    break
  fi
done

SOAK_END_UTC="$(date -u +"%Y-%m-%dT%H:%M:%SZ")"
SOAK_END_EPOCH="$(date +%s)"
SOAK_ELAPSED_S=$((SOAK_END_EPOCH - SOAK_START_EPOCH))

failure_rate_pct="$(awk -v total="$completed_cycles" -v failed="$fail_cycles" 'BEGIN { if (total == 0) printf "0.00"; else printf "%.2f", (failed * 100.0) / total }')"
pass_rate_pct="$(awk -v total="$completed_cycles" -v passed="$pass_cycles" 'BEGIN { if (total == 0) printf "0.00"; else printf "%.2f", (passed * 100.0) / total }')"

python3 - "$CYCLE_JSONL" "$SUMMARY_MD" "$SOAK_START_UTC" "$SOAK_END_UTC" "$CYCLES" "$completed_cycles" "$pass_cycles" "$pass_rate_pct" "$fail_cycles" "$failure_rate_pct" "$SOAK_ELAPSED_S" "$total_duration_s" "$CSV_PATH" "$SOAK_LOG" <<'PY'
import json
import math
import statistics
import sys
from collections import Counter, defaultdict
from pathlib import Path

cycle_jsonl = Path(sys.argv[1])
summary_path = Path(sys.argv[2])
start_utc = sys.argv[3]
end_utc = sys.argv[4]
requested_cycles = int(sys.argv[5])
completed_cycles = int(sys.argv[6])
pass_cycles = int(sys.argv[7])
pass_rate_pct = sys.argv[8]
fail_cycles = int(sys.argv[9])
failure_rate_pct = sys.argv[10]
soak_elapsed_s = int(sys.argv[11])
total_duration_s = int(sys.argv[12])
csv_path = sys.argv[13]
soak_log = sys.argv[14]

cycles = []
with cycle_jsonl.open("r", encoding="utf-8") as handle:
    for raw in handle:
        line = raw.strip()
        if line:
            cycles.append(json.loads(line))

score_counts = Counter(c["scoring_result"] or "n/a" for c in cycles)
metric_values = defaultdict(list)
delta_rows = []
failed_cycles = []

for cycle in cycles:
    if cycle["command_status"] != 0:
        failed_cycles.append(cycle)
    scoring_path = Path(cycle["scoring_path"]) if cycle.get("scoring_path") else None
    if not scoring_path or not scoring_path.exists():
        continue
    scoring = json.loads(scoring_path.read_text(encoding="utf-8"))
    for metric in scoring.get("metrics", []):
        key = f"{metric['suite_or_profile']}::{metric['metric']}"
        current = metric.get("current_value")
        if isinstance(current, (int, float)):
            metric_values[key].append(float(current))
        delta_pct = metric.get("delta_pct")
        delta_abs = metric.get("delta_abs")
        if delta_pct is not None or delta_abs is not None:
            score = abs(delta_pct) if isinstance(delta_pct, (int, float)) else abs(delta_abs or 0.0)
            delta_rows.append(
                (
                    score,
                    cycle["cycle"],
                    metric["suite_or_profile"],
                    metric["metric"],
                    metric["current_value"],
                    metric["baseline_value"],
                    metric["delta_abs"],
                    metric["delta_pct"],
                    metric["classification"],
                )
            )

variation_rows = []
for key, values in metric_values.items():
    if not values:
        continue
    mean = statistics.fmean(values)
    span = max(values) - min(values)
    stdev = statistics.pstdev(values) if len(values) > 1 else 0.0
    variation_rows.append((span, stdev, key, min(values), max(values), mean, len(values)))

variation_rows.sort(reverse=True)
delta_rows.sort(reverse=True)

lines = [
    "# Device Soak Summary",
    "",
    f"- Start (UTC): {start_utc}",
    f"- End (UTC): {end_utc}",
    f"- Requested cycles: {requested_cycles}",
    f"- Completed cycles: {completed_cycles}",
    f"- Passed cycles: {pass_cycles} ({pass_rate_pct}%)",
    f"- Failed cycles: {fail_cycles} ({failure_rate_pct}%)",
    f"- Total runtime (wall): {soak_elapsed_s}s",
    f"- Aggregate cycle runtime: {total_duration_s}s",
    "",
    "## Structured Results",
    "",
    "| Result | Count |",
    "|--------|------:|",
]
for key, count in sorted(score_counts.items()):
    lines.append(f"| `{key}` | {count} |")

lines.extend(["", "## Failed Cycles", ""])
if not failed_cycles:
    lines.append("None.")
else:
    lines.extend(
        [
            "| Cycle | Exit | Failed Suite | Report Dir |",
            "|------:|-----:|--------------|------------|",
        ]
    )
    for cycle in failed_cycles:
        lines.append(
            f"| {cycle['cycle']} | {cycle['command_status']} | "
            f"{cycle.get('failed_suite') or '-'} | {cycle.get('report_dir') or '-'} |"
        )

lines.extend(["", "## Highest Run-to-Run Variation", ""])
if not variation_rows:
    lines.append("No metric variation data available.")
else:
    lines.extend(
        [
            "| Track Metric | Samples | Min | Max | Span | Mean | Stdev |",
            "|--------------|--------:|----:|----:|-----:|-----:|------:|",
        ]
    )
    for span, stdev, key, min_v, max_v, mean, samples in variation_rows[:12]:
        lines.append(
            f"| `{key}` | {samples} | {min_v:.3f} | {max_v:.3f} | {span:.3f} | {mean:.3f} | {stdev:.3f} |"
        )

lines.extend(["", "## Worst Deltas Vs Prior Cycle", ""])
if not delta_rows:
    lines.append("No baseline deltas available.")
else:
    lines.extend(
        [
            "| Cycle | Track | Metric | Current | Baseline | Delta | Delta % | Classification |",
            "|------:|-------|--------|--------:|---------:|------:|--------:|----------------|",
        ]
    )
    for _score, cycle_num, track, metric, current, baseline, delta_abs, delta_pct, classification in delta_rows[:12]:
        pct_text = "n/a" if delta_pct is None else f"{delta_pct:.3f}%"
        abs_text = "n/a" if delta_abs is None else f"{delta_abs:.3f}"
        lines.append(
            f"| {cycle_num} | `{track}` | `{metric}` | "
            f"{current if current is not None else 'n/a'} | "
            f"{baseline if baseline is not None else 'n/a'} | "
            f"{abs_text} | {pct_text} | {classification} |"
        )

lines.extend(
    [
        "",
        "## Artifacts",
        "",
        f"- Cycle CSV: `{csv_path}`",
        f"- Cycle JSONL: `{cycle_jsonl}`",
        f"- Soak log: `{soak_log}`",
    ]
)

summary_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
PY

echo "==> Soak complete"
echo "    completed: $completed_cycles / $CYCLES"
echo "    pass: $pass_cycles"
echo "    fail: $fail_cycles"
echo "    failure rate: ${failure_rate_pct}%"
echo "    summary: $SUMMARY_MD"
echo "    csv: $CSV_PATH"

if [[ "$fail_cycles" -gt 0 ]]; then
  exit 1
fi

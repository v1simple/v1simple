#!/usr/bin/env python3
"""Import a captured drive metrics log into the canonical hardware scoring artifacts."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

import score_hardware_run  # type: ignore
from hardware_report_utils import write_comparison_text, write_comparison_tsv  # type: ignore
from metric_schema import SOAK_TREND_METRIC_KV_ALIASES, SOAK_TREND_METRIC_UNITS  # type: ignore


ROOT = Path(__file__).resolve().parents[1]
CATALOG_PATH = ROOT / "tools" / "hardware_metric_catalog.json"
SOAK_PARSE_METRICS = ROOT / "tools" / "soak_parse_metrics.py"
SOAK_PARSE_PANIC = ROOT / "tools" / "soak_parse_panic.py"

RESET_RE = re.compile(r"rst:0x")
PANIC_SIGNATURE_RE = re.compile(r"task watchdog|task_wdt|Guru Meditation|panic|abort\(", re.IGNORECASE)
GURU_RE = re.compile(r"Guru Meditation", re.IGNORECASE)

@dataclass(frozen=True)
class SourceBundle:
    root_dir: Path
    manifest: Path | None
    metrics_jsonl: Path | None
    panic_jsonl: Path | None
    serial_log: Path | None


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--input",
        required=True,
        help="Path to a soak run directory, metrics.jsonl, panic.jsonl, or serial.log",
    )
    parser.add_argument("--out-dir", required=True, help="Output directory for manifest/scoring artifacts")
    parser.add_argument(
        "--compare-to",
        action="append",
        default=[],
        help="Optional baseline manifest.json (repeat for a baseline window)",
    )
    parser.add_argument("--board-id", default="", help="Override board_id in emitted manifest")
    parser.add_argument("--git-sha", default="", help="Override git_sha in emitted manifest")
    parser.add_argument("--git-ref", default="", help="Override git_ref in emitted manifest")
    parser.add_argument("--suite-or-profile", default="", help="Override suite_or_profile in emitted manifest")
    parser.add_argument("--stress-class", default="", help="Override stress_class in emitted manifest")
    parser.add_argument("--env", default="", help="Override env in emitted manifest")
    parser.add_argument("--lane", default="real-fw-soak-import", help="Lane value for emitted manifest")
    return parser.parse_args()


def _optional_child(root: Path, name: str) -> Path | None:
    candidate = root / name
    return candidate if candidate.exists() else None


def resolve_source(path: Path) -> SourceBundle:
    source = path.resolve()
    if source.is_dir():
        bundle = SourceBundle(
            root_dir=source,
            manifest=_optional_child(source, "manifest.json"),
            metrics_jsonl=_optional_child(source, "metrics.jsonl"),
            panic_jsonl=_optional_child(source, "panic.jsonl"),
            serial_log=_optional_child(source, "serial.log"),
        )
    elif source.is_file():
        root_dir = source.parent
        manifest = _optional_child(root_dir, "manifest.json")
        metrics_jsonl = _optional_child(root_dir, "metrics.jsonl")
        panic_jsonl = _optional_child(root_dir, "panic.jsonl")
        serial_log = _optional_child(root_dir, "serial.log")
        if source.name == "metrics.jsonl":
            metrics_jsonl = source
        elif source.name == "panic.jsonl":
            panic_jsonl = source
        elif source.name == "serial.log":
            serial_log = source
        else:
            raise RuntimeError(
                "Input file must be one of metrics.jsonl, panic.jsonl, or serial.log; "
                f"got '{source.name}'"
            )
        bundle = SourceBundle(
            root_dir=root_dir,
            manifest=manifest,
            metrics_jsonl=metrics_jsonl,
            panic_jsonl=panic_jsonl,
            serial_log=serial_log,
        )
    else:
        raise RuntimeError(f"Could not resolve soak artifacts from '{path}'")

    if bundle.metrics_jsonl is None and bundle.panic_jsonl is None and bundle.serial_log is None:
        raise RuntimeError(f"Could not resolve soak artifacts from '{path}'")
    return bundle


def load_json(path: Path | None) -> dict[str, Any] | None:
    if path is None or not path.exists():
        return None
    payload = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(payload, dict):
        raise RuntimeError(f"Expected JSON object in {path}")
    return payload


def parse_kv_output(raw_text: str) -> dict[str, str]:
    payload: dict[str, str] = {}
    for line in raw_text.splitlines():
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        payload[key.strip()] = value.strip()
    return payload


def run_kv_parser(parser_path: Path, input_path: Path) -> tuple[dict[str, str], str]:
    proc = subprocess.run(
        [sys.executable, str(parser_path), str(input_path)],
        check=False,
        capture_output=True,
        text=True,
        cwd=ROOT,
    )
    if proc.returncode != 0:
        print(proc.stderr, file=sys.stderr, end="")
        raise RuntimeError(f"Parser failed for {input_path}")
    return parse_kv_output(proc.stdout), proc.stdout


def numeric(value: str) -> float | None:
    if value == "":
        return None
    try:
        return float(value)
    except ValueError:
        return None


def integer(value: str) -> int | None:
    if value == "":
        return None
    try:
        numeric_value = float(value)
    except ValueError:
        return None
    if abs(numeric_value - round(numeric_value)) > 1e-9:
        return None
    return int(round(numeric_value))


def select_wifi_peak_metric(kv: dict[str, str]) -> float | None:
    ok_samples = numeric(kv.get("ok_samples", "")) or numeric(kv.get("metrics_ok_samples", ""))
    warm_value = numeric(kv.get("wifi_max_peak_excluding_first", ""))
    if ok_samples is not None and ok_samples > 2 and warm_value is not None:
        return warm_value
    selected_value = numeric(kv.get("wifi_max_peak", ""))
    if selected_value is not None:
        return selected_value
    return numeric(kv.get("wifi_max_peak_us", ""))


def select_wifi_p95_metric(kv: dict[str, str]) -> float | None:
    sample_count_excluding_first = numeric(kv.get("wifi_sample_count_excluding_first", ""))
    warm_value = numeric(kv.get("wifi_p95_excluding_first", ""))
    if sample_count_excluding_first is not None and sample_count_excluding_first > 0 and warm_value is not None:
        return warm_value
    selected_value = numeric(kv.get("wifi_p95_raw", ""))
    if selected_value is not None:
        return selected_value
    return numeric(kv.get("wifi_p95_us", ""))


def parse_serial_log(path: Path | None) -> dict[str, Any]:
    summary = {
        "available": False,
        "bytes": 0,
        "line_count": 0,
        "reset_count": 0,
        "panic_signature_count": 0,
        "guru_count": 0,
    }
    if path is None or not path.exists():
        return summary

    summary["available"] = True
    summary["bytes"] = path.stat().st_size
    with path.open("r", encoding="utf-8", errors="replace") as handle:
        for raw in handle:
            summary["line_count"] += 1
            if RESET_RE.search(raw):
                summary["reset_count"] += 1
            if PANIC_SIGNATURE_RE.search(raw):
                summary["panic_signature_count"] += 1
            if GURU_RE.search(raw):
                summary["guru_count"] += 1
    return summary


def panic_runtime_crash_detected(kv: dict[str, str]) -> bool:
    first_was_crash = integer(kv.get("first_was_crash", ""))
    last_was_crash = integer(kv.get("last_was_crash", ""))
    state_change_count = integer(kv.get("state_change_count", ""))
    if first_was_crash is None or last_was_crash is None or state_change_count is None:
        return False
    return last_was_crash == 1 and (first_was_crash == 0 or state_change_count > 0)


def panic_preexisting_crash_state(kv: dict[str, str]) -> bool:
    first_was_crash = integer(kv.get("first_was_crash", ""))
    last_was_crash = integer(kv.get("last_was_crash", ""))
    state_change_count = integer(kv.get("state_change_count", ""))
    if first_was_crash is None or last_was_crash is None or state_change_count is None:
        return False
    return first_was_crash == 1 and last_was_crash == 1 and state_change_count == 0


def build_import_diagnostics(
    bundle: SourceBundle,
    metrics_kv: dict[str, str],
    panic_kv: dict[str, str],
    serial_summary: dict[str, Any],
    metric_count: int,
) -> dict[str, Any]:
    metrics_ok_samples = integer(metrics_kv.get("ok_samples", ""))
    if metrics_ok_samples is None:
        metrics_ok_samples = integer(metrics_kv.get("metrics_ok_samples", ""))
    panic_ok_samples = integer(panic_kv.get("ok_samples", ""))
    runtime_crash = panic_runtime_crash_detected(panic_kv)
    preexisting_panic_state = panic_preexisting_crash_state(panic_kv)

    signal_sources = 0
    if int(serial_summary.get("bytes", 0)) > 0:
        signal_sources += 1
    if metrics_ok_samples is not None and metrics_ok_samples > 0:
        signal_sources += 1
    if panic_ok_samples is not None and panic_ok_samples > 0:
        signal_sources += 1

    reboot_evidence_parts: list[str] = []
    if int(serial_summary.get("reset_count", 0)) > 0:
        reboot_evidence_parts.append(f"serial_rst={serial_summary['reset_count']}")
    if int(serial_summary.get("panic_signature_count", 0)) > 0:
        reboot_evidence_parts.append(f"serial_panic_signatures={serial_summary['panic_signature_count']}")
    if runtime_crash:
        reboot_evidence_parts.append(
            "panic_endpoint_runtime_crash="
            f"first:{panic_kv.get('first_was_crash', 'n/a')},"
            f"last:{panic_kv.get('last_was_crash', 'n/a')},"
            f"changes:{panic_kv.get('state_change_count', 'n/a')}"
        )

    reboot_evidence_detected = bool(reboot_evidence_parts)
    if reboot_evidence_detected:
        base_result = "FAIL"
        base_reason = "reboot_or_crash_evidence"
    elif metric_count == 0 or metrics_ok_samples is None or metrics_ok_samples <= 0:
        base_result = "INCONCLUSIVE"
        base_reason = "missing_runtime_metrics"
    elif preexisting_panic_state:
        base_result = "PASS_WITH_WARNINGS"
        base_reason = "preexisting_panic_state"
    else:
        base_result = "PASS"
        base_reason = "metrics_available"

    return {
        "source_files": {
            "root_dir": str(bundle.root_dir),
            "manifest": "" if bundle.manifest is None else str(bundle.manifest),
            "metrics_jsonl": "" if bundle.metrics_jsonl is None else str(bundle.metrics_jsonl),
            "panic_jsonl": "" if bundle.panic_jsonl is None else str(bundle.panic_jsonl),
            "serial_log": "" if bundle.serial_log is None else str(bundle.serial_log),
        },
        "source_coverage": {
            "signal_sources": signal_sources,
            "metrics_jsonl_present": bundle.metrics_jsonl is not None,
            "panic_jsonl_present": bundle.panic_jsonl is not None,
            "serial_log_present": bundle.serial_log is not None,
        },
        "metrics": {
            "metric_count": metric_count,
            "metrics_ok_samples": metrics_ok_samples,
        },
        "serial": serial_summary,
        "panic": {
            "ok_samples": panic_ok_samples,
            "was_crash_true": integer(panic_kv.get("was_crash_true", "")),
            "has_panic_file_true": integer(panic_kv.get("has_panic_file_true", "")),
            "first_was_crash": integer(panic_kv.get("first_was_crash", "")),
            "last_was_crash": integer(panic_kv.get("last_was_crash", "")),
            "first_has_panic_file": integer(panic_kv.get("first_has_panic_file", "")),
            "last_has_panic_file": integer(panic_kv.get("last_has_panic_file", "")),
            "first_reset_reason": panic_kv.get("first_reset_reason", ""),
            "last_reset_reason": panic_kv.get("last_reset_reason", ""),
            "state_change_count": integer(panic_kv.get("state_change_count", "")),
            "runtime_crash_detected": runtime_crash,
            "preexisting_crash_state": preexisting_panic_state,
        },
        "reboot_evidence_detected": reboot_evidence_detected,
        "reboot_evidence_detail": ", ".join(reboot_evidence_parts) if reboot_evidence_parts else "none",
        "base_result": base_result,
        "base_result_reason": base_reason,
    }


def append_import_diagnostics(text_path: Path, diagnostics: dict[str, Any]) -> None:
    source_files = diagnostics["source_files"]
    source_coverage = diagnostics["source_coverage"]
    serial = diagnostics["serial"]
    panic = diagnostics["panic"]
    lines = [
        "",
        "## Imported Signals",
        "",
        f"- Base result reason: `{diagnostics['base_result_reason']}`",
        f"- Metrics JSONL: {'present' if source_coverage['metrics_jsonl_present'] else 'missing'} `{source_files['metrics_jsonl'] or 'n/a'}`",
        f"- Panic JSONL: {'present' if source_coverage['panic_jsonl_present'] else 'missing'} `{source_files['panic_jsonl'] or 'n/a'}`",
        f"- Serial log: {'present' if source_coverage['serial_log_present'] else 'missing'} `{source_files['serial_log'] or 'n/a'}`",
        f"- Source signal count: {source_coverage['signal_sources']}",
        f"- Reboot evidence detected: {'yes' if diagnostics['reboot_evidence_detected'] else 'no'}",
        f"- Reboot evidence detail: {diagnostics['reboot_evidence_detail']}",
        f"- Serial reset count: {serial['reset_count']}",
        f"- Serial panic/WDT signature count: {serial['panic_signature_count']}",
        f"- Guru Meditation count: {serial['guru_count']}",
        f"- Panic ok samples: {panic['ok_samples'] if panic['ok_samples'] is not None else 'n/a'}",
        f"- Panic runtime crash detected: {'yes' if panic['runtime_crash_detected'] else 'no'}",
        f"- Panic preexisting crash state: {'yes' if panic['preexisting_crash_state'] else 'no'}",
    ]
    with text_path.open("a", encoding="utf-8") as handle:
        handle.write("\n".join(lines) + "\n")


def render_metrics_ndjson(
    out_path: Path,
    run_id: str,
    git_sha: str,
    suite_or_profile: str,
    kv: dict[str, str],
    *,
    emit_display_drive_activity: bool,
) -> int:
    count = 0
    with out_path.open("w", encoding="utf-8") as handle:
        for key, unit in SOAK_TREND_METRIC_UNITS.items():
            if key == "display_drive_activity_delta" and not emit_display_drive_activity:
                continue
            if key == "wifi_max_peak_us":
                value = select_wifi_peak_metric(kv)
            elif key == "wifi_p95_us":
                value = select_wifi_p95_metric(kv)
            elif key == "display_drive_activity_delta":
                value = numeric(kv.get("display_drive_activity_delta", ""))
                if value is None:
                    value = numeric(kv.get("display_updates_delta", ""))
            else:
                source_key = SOAK_TREND_METRIC_KV_ALIASES.get(key, key)
                value = numeric(kv.get(source_key, ""))
                if value is None and source_key != key:
                    value = numeric(kv.get(key, ""))
            if value is None:
                continue
            record = {
                "schema_version": 1,
                "run_id": run_id,
                "git_sha": git_sha,
                "run_kind": "real_fw_soak",
                "suite_or_profile": suite_or_profile,
                "metric": key,
                "sample": "value",
                "value": value,
                "unit": unit,
                "tags": {},
            }
            handle.write(json.dumps(record, sort_keys=True))
            handle.write("\n")
            count += 1
    return count


def exit_code_for_result(result: str) -> int:
    if result == "FAIL":
        return 2
    if result == "PASS_WITH_WARNINGS":
        return 1
    return 0


def main() -> int:
    args = parse_args()
    source_input = Path(args.input).resolve()
    out_dir = Path(args.out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    bundle = resolve_source(source_input)
    source_manifest = load_json(bundle.manifest)

    metrics_kv: dict[str, str] = {}
    panic_kv: dict[str, str] = {}
    if bundle.metrics_jsonl is not None:
        try:
            metrics_kv, raw_metrics_kv = run_kv_parser(SOAK_PARSE_METRICS, bundle.metrics_jsonl)
        except RuntimeError:
            return 1
        (out_dir / "parsed_metrics_kv.txt").write_text(raw_metrics_kv, encoding="utf-8")
    if bundle.panic_jsonl is not None:
        try:
            panic_kv, raw_panic_kv = run_kv_parser(SOAK_PARSE_PANIC, bundle.panic_jsonl)
        except RuntimeError:
            return 1
        (out_dir / "parsed_panic_kv.txt").write_text(raw_panic_kv, encoding="utf-8")

    serial_summary = parse_serial_log(bundle.serial_log)

    timestamp = datetime.now(timezone.utc).strftime("%Y%m%d_%H%M%S")
    git_sha = args.git_sha or str((source_manifest or {}).get("git_sha") or "unknown")
    git_ref = args.git_ref or str((source_manifest or {}).get("git_ref") or "unknown")
    board_id = args.board_id or str((source_manifest or {}).get("board_id") or "unknown")
    suite_or_profile = args.suite_or_profile or str((source_manifest or {}).get("suite_or_profile") or "drive_wifi_ap")
    stress_class = args.stress_class or str((source_manifest or {}).get("stress_class") or "core")
    env_name = args.env or str((source_manifest or {}).get("env") or "parsed-drive-log")
    run_id = f"parsed_drive_log_{timestamp}_{git_sha}"

    metrics_ndjson = out_dir / "metrics.ndjson"
    metric_count = render_metrics_ndjson(
        metrics_ndjson,
        run_id,
        git_sha,
        suite_or_profile,
        metrics_kv,
        emit_display_drive_activity=stress_class == "display_preview",
    )
    diagnostics = build_import_diagnostics(bundle, metrics_kv, panic_kv, serial_summary, metric_count)
    diagnostics_path = out_dir / "import_diagnostics.json"
    diagnostics_path.write_text(json.dumps(diagnostics, indent=2) + "\n", encoding="utf-8")
    base_result = str(diagnostics["base_result"])

    manifest_path = out_dir / "manifest.json"
    manifest = {
        "schema_version": 1,
        "run_id": run_id,
        "timestamp_utc": datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z"),
        "git_sha": git_sha,
        "git_ref": git_ref,
        "run_kind": "real_fw_soak",
        "board_id": board_id,
        "env": env_name,
        "lane": args.lane,
        "suite_or_profile": suite_or_profile,
        "stress_class": stress_class,
        "result": base_result,
        "base_result": base_result,
        "metrics_file": "metrics.ndjson",
        "scoring_file": "scoring.json",
        "tracks": [suite_or_profile] if metric_count > 0 else [],
        "source_input": str(source_input),
        "import_diagnostics_file": "import_diagnostics.json",
    }
    if bundle.manifest is not None:
        manifest["source_manifest"] = str(bundle.manifest)
    if bundle.metrics_jsonl is not None:
        manifest["source_metrics_jsonl"] = str(bundle.metrics_jsonl)
    if bundle.panic_jsonl is not None:
        manifest["source_panic_jsonl"] = str(bundle.panic_jsonl)
    if bundle.serial_log is not None:
        manifest["source_serial_log"] = str(bundle.serial_log)
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")

    baseline_paths = [Path(path).resolve() for path in args.compare_to if path]
    try:
        scored = score_hardware_run.score_run(manifest_path, CATALOG_PATH, baseline_paths)
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 3

    scoring_path = out_dir / "scoring.json"
    comparison_txt = out_dir / "comparison.txt"
    comparison_tsv = out_dir / "comparison.tsv"
    scoring_path.write_text(json.dumps(scored, indent=2) + "\n", encoding="utf-8")
    write_comparison_text(scored, comparison_txt)
    write_comparison_tsv(scored, comparison_tsv)
    append_import_diagnostics(comparison_txt, diagnostics)

    manifest["result"] = scored["result"]
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return exit_code_for_result(str(scored["result"]))


if __name__ == "__main__":
    raise SystemExit(main())

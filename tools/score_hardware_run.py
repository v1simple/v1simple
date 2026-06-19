#!/usr/bin/env python3
"""Score structured hardware run manifests against a canonical metric catalog."""

from __future__ import annotations

import argparse
import json
import math
import sys
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Optional


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_CATALOG_PATH = ROOT / "tools" / "hardware_metric_catalog.json"

# Packet/parse count gates prove that an external proxy/OBD data source was
# present. They are valid physical-coverage evidence, but they are not runtime
# safety failures when a lab run explicitly records "no external activity".
COVERAGE_ACTIVITY_METRICS = frozenset({
    "rx_packets_delta",
    "parse_successes_delta",
})
NO_ACTIVITY_COVERAGE_MARKERS = frozenset({
    "no_proxy_activity_observed",
    "no_obd_activity_observed",
    "no_proxy_coverage",
    "no_obd_coverage",
})


@dataclass(frozen=True)
class MetricPolicy:
    metric: str
    run_kind: str
    selector: dict[str, Any]
    unit: str
    aggregation: str
    direction: str
    score_level: str
    regression_score_level: str
    required: bool
    absolute_min: Optional[float]
    absolute_max: Optional[float]
    regress_abs: Optional[float]
    regress_pct: Optional[float]


@dataclass
class MetricAggregate:
    metric: str
    run_kind: str
    suite_or_profile: str
    value: float
    unit: str
    sample_count: int


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Score a hardware run manifest")
    parser.add_argument("manifest", help="Path to manifest.json")
    parser.add_argument(
        "--catalog",
        default=str(DEFAULT_CATALOG_PATH),
        help=f"Path to hardware metric catalog (default: {DEFAULT_CATALOG_PATH})",
    )
    parser.add_argument(
        "--compare-to",
        action="append",
        default=[],
        help="Optional baseline manifest for run-to-run or commit-to-commit comparison (repeat for a baseline window)",
    )
    parser.add_argument("--json", action="store_true", help="Emit JSON instead of human-readable text")
    return parser.parse_args()


def _load_json(path: Path) -> Any:
    with path.open("r", encoding="utf-8") as handle:
        return json.load(handle)


def _resolve_manifest_ref(manifest_path: Path, ref: str) -> Path:
    path = Path(ref)
    if path.is_absolute():
        return path
    return (manifest_path.parent / path).resolve()


def _coerce_optional_float(payload: dict[str, Any], key: str) -> Optional[float]:
    raw = payload.get(key)
    if raw is None:
        return None
    return float(raw)


def load_catalog(path: Path) -> list[MetricPolicy]:
    payload = _load_json(path)
    if not isinstance(payload, dict) or not isinstance(payload.get("metrics"), list):
        raise RuntimeError(f"Invalid catalog format in {path}")

    policies: list[MetricPolicy] = []
    seen: set[tuple[str, str, str]] = set()
    for idx, entry in enumerate(payload["metrics"]):
        if not isinstance(entry, dict):
            raise RuntimeError(f"Invalid catalog entry at index {idx}: expected object")
        selector = entry.get("selector") or {}
        if not isinstance(selector, dict):
            raise RuntimeError(f"Invalid selector in catalog entry {idx}")
        policy = MetricPolicy(
            metric=str(entry["metric"]),
            run_kind=str(entry["run_kind"]),
            selector=selector,
            unit=str(entry["unit"]),
            aggregation=str(entry["aggregation"]),
            direction=str(entry["direction"]),
            score_level=str(entry["score_level"]),
            regression_score_level=str(entry.get("regression_score_level") or entry["score_level"]),
            required=bool(entry["required"]),
            absolute_min=_coerce_optional_float(entry, "absolute_min"),
            absolute_max=_coerce_optional_float(entry, "absolute_max"),
            regress_abs=_coerce_optional_float(entry, "regress_abs"),
            regress_pct=_coerce_optional_float(entry, "regress_pct"),
        )
        for field_name, level in (
            ("score_level", policy.score_level),
            ("regression_score_level", policy.regression_score_level),
        ):
            if level not in {"hard", "advisory", "info"}:
                raise RuntimeError(
                    f"Invalid {field_name} '{level}' in catalog entry {idx} "
                    f"for {policy.run_kind}/{policy.metric}"
                )
        selector_key = json.dumps(selector, sort_keys=True)
        key = (policy.run_kind, policy.metric, selector_key)
        if key in seen:
            raise RuntimeError(f"Duplicate policy in catalog for {policy.run_kind}/{policy.metric} selector={selector_key}")
        seen.add(key)
        policies.append(policy)
    return policies


def load_manifest(path: Path) -> dict[str, Any]:
    payload = _load_json(path)
    required = [
        "schema_version",
        "run_id",
        "timestamp_utc",
        "git_sha",
        "git_ref",
        "run_kind",
        "board_id",
        "env",
        "lane",
        "suite_or_profile",
        "stress_class",
        "result",
        "metrics_file",
        "scoring_file",
    ]
    missing = [key for key in required if key not in payload]
    if missing:
        raise RuntimeError(f"Manifest missing required fields: {', '.join(missing)}")
    return payload


def load_metrics(manifest_path: Path, manifest: dict[str, Any]) -> list[dict[str, Any]]:
    metrics_path = _resolve_manifest_ref(manifest_path, str(manifest["metrics_file"]))
    records: list[dict[str, Any]] = []
    with metrics_path.open("r", encoding="utf-8") as handle:
        for lineno, raw_line in enumerate(handle, start=1):
            line = raw_line.strip()
            if not line:
                continue
            try:
                record = json.loads(line)
            except json.JSONDecodeError as exc:
                raise RuntimeError(f"Invalid metrics NDJSON line {lineno} in {metrics_path}: {exc}") from exc
            if not isinstance(record, dict):
                raise RuntimeError(f"Invalid metrics NDJSON line {lineno} in {metrics_path}: expected object")
            for field in [
                "schema_version",
                "run_id",
                "git_sha",
                "run_kind",
                "suite_or_profile",
                "metric",
                "sample",
                "value",
                "unit",
                "tags",
            ]:
                if field not in record:
                    raise RuntimeError(f"Metric line {lineno} in {metrics_path} missing '{field}'")
            if not isinstance(record.get("tags"), dict):
                raise RuntimeError(f"Metric line {lineno} in {metrics_path} has non-object tags")
            value = record.get("value")
            if isinstance(value, bool):
                value = int(value)
            if not isinstance(value, (int, float)):
                raise RuntimeError(f"Metric line {lineno} in {metrics_path} has non-numeric value")
            record["value"] = float(value)
            records.append(record)
    return records


def _selector_value(selector: dict[str, Any], key: str) -> Any:
    return selector.get(key)


def _selector_matches(policy: MetricPolicy, record: dict[str, Any], manifest: dict[str, Any]) -> bool:
    if policy.run_kind != record.get("run_kind"):
        return False
    if policy.metric != record.get("metric"):
        return False
    selector_suite = _selector_value(policy.selector, "suite_or_profile")
    if selector_suite is not None and selector_suite != record.get("suite_or_profile"):
        return False
    selector_stress = _selector_value(policy.selector, "stress_class")
    if selector_stress is not None and selector_stress != manifest.get("stress_class"):
        return False
    selector_lane = _selector_value(policy.selector, "lane")
    if selector_lane is not None and selector_lane != manifest.get("lane"):
        return False
    selector_source_type = _selector_value(policy.selector, "source_type")
    if selector_source_type is not None and selector_source_type != manifest.get("source_type"):
        return False
    return True


def _policy_applies_to_track(policy: MetricPolicy, manifest: dict[str, Any], executed_tracks: set[str]) -> bool:
    if not executed_tracks:
        return False
    if policy.run_kind != manifest.get("run_kind"):
        return False
    selector_stress = _selector_value(policy.selector, "stress_class")
    if selector_stress is not None and selector_stress != manifest.get("stress_class"):
        return False
    selector_lane = _selector_value(policy.selector, "lane")
    if selector_lane is not None and selector_lane != manifest.get("lane"):
        return False
    selector_source_type = _selector_value(policy.selector, "source_type")
    if selector_source_type is not None and selector_source_type != manifest.get("source_type"):
        return False
    selector_suite = _selector_value(policy.selector, "suite_or_profile")
    if selector_suite is None:
        return True
    return selector_suite in executed_tracks


def _unsupported_metric_names(manifest: dict[str, Any]) -> set[str]:
    raw = manifest.get("unsupported_metrics") or []
    if not isinstance(raw, list):
        raise RuntimeError("Manifest field 'unsupported_metrics' must be a list when present")
    return {str(item) for item in raw if str(item)}


def _percentile(values: list[float], pct: float) -> Optional[float]:
    if not values:
        return None
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    rank = (pct / 100.0) * (len(ordered) - 1)
    lo = int(math.floor(rank))
    hi = int(math.ceil(rank))
    if lo == hi:
        return ordered[lo]
    frac = rank - lo
    return ordered[lo] + (ordered[hi] - ordered[lo]) * frac


def aggregate_metric(policy: MetricPolicy, record_group: list[dict[str, Any]]) -> MetricAggregate:
    values = [float(item["value"]) for item in record_group]
    if not values:
        raise RuntimeError("Cannot aggregate empty metric group")
    if policy.aggregation == "last":
        value = values[-1]
    elif policy.aggregation == "min":
        value = min(values)
    elif policy.aggregation == "max":
        value = max(values)
    elif policy.aggregation == "delta":
        value = values[-1] - values[0] if len(values) > 1 else values[0]
    elif policy.aggregation == "p95":
        p95 = _percentile(values, 95.0)
        if p95 is None:
            raise RuntimeError(f"Cannot compute p95 for metric {policy.metric}")
        value = p95
    else:
        raise RuntimeError(f"Unsupported aggregation '{policy.aggregation}' for metric {policy.metric}")

    first = record_group[0]
    return MetricAggregate(
        metric=policy.metric,
        run_kind=policy.run_kind,
        suite_or_profile=str(first["suite_or_profile"]),
        value=value,
        unit=policy.unit,
        sample_count=len(values),
    )


def _format_num(value: Optional[float]) -> str:
    if value is None:
        return "n/a"
    if abs(value - round(value)) < 1e-9:
        return str(int(round(value)))
    return f"{value:.3f}"


def _median(values: Sequence[float]) -> Optional[float]:
    return _percentile(list(values), 50.0)


def _pct_delta(current: float, baseline: Optional[float]) -> Optional[float]:
    if baseline is None:
        return None
    if abs(baseline) < 1e-9:
        if abs(current) < 1e-9:
            return 0.0
        return None
    return ((current - baseline) / abs(baseline)) * 100.0


def _absolute_state(policy: MetricPolicy, value: float) -> tuple[str, Optional[str]]:
    if policy.absolute_min is None and policy.absolute_max is None:
        return "n/a", None
    if policy.absolute_min is not None and value < policy.absolute_min:
        return "fail", f"value {_format_num(value)} below min {_format_num(policy.absolute_min)}"
    if policy.absolute_max is not None and value > policy.absolute_max:
        return "fail", f"value {_format_num(value)} above max {_format_num(policy.absolute_max)}"
    return "pass", None


def _coverage_activity_metric_is_observation_only(policy: MetricPolicy, manifest: dict[str, Any]) -> bool:
    """Return True when an external-activity metric should not score as hard.

    Mode coverage is evaluated before SD CSV import in the hardware runner. If
    a lab run has no proxy/OBD hardware connected, `rx_packets_delta == 0` and
    `parse_successes_delta == 0` are expected observations, not firmware
    failures. Physical runs still fail via `mode_coverage.reasons`; this helper
    only prevents the packet-count catalog entries from double-counting missing
    physical coverage as a runtime/safety fault.
    """
    if policy.metric not in COVERAGE_ACTIVITY_METRICS:
        return False
    coverage = manifest.get("mode_coverage")
    if not isinstance(coverage, dict):
        return False
    markers = {
        str(item)
        for item in list(coverage.get("warnings") or []) + list(coverage.get("reasons") or [])
        if str(item)
    }
    return bool(markers & NO_ACTIVITY_COVERAGE_MARKERS)


def _regression_state(policy: MetricPolicy, current: float, baseline: Optional[float]) -> tuple[str, str, Optional[str]]:
    if baseline is None:
        return "n/a", "no_baseline", None
    if policy.direction in {"target", "range"}:
        return "n/a", "changed" if abs(current - baseline) > 1e-9 else "unchanged", None
    if policy.regress_abs is None and policy.regress_pct is None:
        if abs(current - baseline) <= 1e-9:
            return "pass", "unchanged", None
        if policy.direction == "lower_better":
            return "pass", "improved" if current < baseline else "changed", None
        return "pass", "improved" if current > baseline else "changed", None

    threshold = 0.0
    if policy.regress_abs is not None:
        threshold = max(threshold, policy.regress_abs)
    if policy.regress_pct is not None:
        threshold = max(threshold, abs(baseline) * policy.regress_pct)

    if policy.direction == "lower_better":
        if current > baseline + threshold:
            return "fail", "regressed", f"value {_format_num(current)} regressed above baseline {_format_num(baseline)} by more than {_format_num(threshold)}"
        if current < baseline - threshold:
            return "pass", "improved", None
        return "pass", "unchanged", None

    if policy.direction == "higher_better":
        if current < baseline - threshold:
            return "fail", "regressed", f"value {_format_num(current)} regressed below baseline {_format_num(baseline)} by more than {_format_num(threshold)}"
        if current > baseline + threshold:
            return "pass", "improved", None
        return "pass", "unchanged", None

    raise RuntimeError(f"Unsupported direction '{policy.direction}' for metric {policy.metric}")


def _track_key(manifest: dict[str, Any]) -> tuple[str, str, str, str]:
    return (
        str(manifest.get("run_kind", "")),
        str(manifest.get("board_id", "")),
        str(manifest.get("env", "")),
        str(manifest.get("stress_class", "")),
    )


def _normalize_baseline_paths(
    baseline_manifest_paths: Optional[Path | Sequence[Path]],
) -> list[Path]:
    if baseline_manifest_paths is None:
        return []
    if isinstance(baseline_manifest_paths, Path):
        return [baseline_manifest_paths]
    return [path for path in baseline_manifest_paths if isinstance(path, Path)]


# Precedence: read `base_result` before `result`. This mirrors the current
# manifest's result resolution near the end of `score_run`
# (`manifest.get("base_result", manifest.get("result", "PASS"))`). Keeping the
# two paths consistent means a baseline candidate's tier classification uses
# the same field semantics that produced its `result` in the first place.
def _baseline_result(candidate: dict[str, Any]) -> str:
    return str(candidate.get("base_result", candidate.get("result", "")))


def _baseline_strategy_label(candidate_count: int) -> str:
    if candidate_count <= 0:
        return "none"
    if candidate_count == 1:
        return "single_baseline"
    return "median_last_3_trustworthy"


def _build_metric_map(
    policies: list[MetricPolicy],
    records: list[dict[str, Any]],
    manifest: dict[str, Any],
    strict: bool = True,
) -> tuple[dict[tuple[str, str], MetricAggregate], dict[tuple[str, str], MetricPolicy]]:
    grouped: dict[tuple[str, str], list[dict[str, Any]]] = {}
    policy_for_key: dict[tuple[str, str], MetricPolicy] = {}

    for record in records:
        matches = [policy for policy in policies if _selector_matches(policy, record, manifest)]
        if not matches:
            if not strict:
                # Baseline run contains a metric removed from the catalog; skip it.
                continue
            raise RuntimeError(
                "Emitted metric not present in catalog: "
                f"{record.get('run_kind')}/{record.get('suite_or_profile')}/{record.get('metric')}"
            )
        if len(matches) > 1:
            raise RuntimeError(
                "Ambiguous catalog match for emitted metric: "
                f"{record.get('run_kind')}/{record.get('suite_or_profile')}/{record.get('metric')}"
            )
        policy = matches[0]
        key = (str(record["suite_or_profile"]), policy.metric)
        grouped.setdefault(key, []).append(record)
        policy_for_key[key] = policy

    aggregates = {
        key: aggregate_metric(policy_for_key[key], values)
        for key, values in grouped.items()
    }
    return aggregates, policy_for_key


def score_run(
    manifest_path: Path,
    catalog_path: Path,
    baseline_manifest_paths: Optional[Path | Sequence[Path]] = None,
) -> dict[str, Any]:
    manifest = load_manifest(manifest_path)
    policies = load_catalog(catalog_path)
    records = load_metrics(manifest_path, manifest)

    current_map, policy_map = _build_metric_map(policies, records, manifest)
    executed_tracks = set(str(item) for item in manifest.get("tracks") or [])
    if not executed_tracks:
        executed_tracks = {str(aggregate.suite_or_profile) for aggregate in current_map.values()}

    baseline_paths = _normalize_baseline_paths(baseline_manifest_paths)
    baseline_manifest: Optional[dict[str, Any]] = None
    baseline_values: dict[tuple[str, str], float] = {}
    comparison_kind = "no_baseline"
    baseline_candidates: list[dict[str, Any]] = []
    selected_candidates: list[dict[str, Any]] = []
    unsupported_metric_names = _unsupported_metric_names(manifest)
    for baseline_manifest_path in baseline_paths:
        if not baseline_manifest_path.exists():
            continue
        candidate = load_manifest(baseline_manifest_path)
        if _track_key(candidate) != _track_key(manifest):
            continue
        candidate_result = _baseline_result(candidate)
        baseline_records = load_metrics(baseline_manifest_path, candidate)
        baseline_map, _ = _build_metric_map(policies, baseline_records, candidate, strict=False)
        baseline_candidates.append(
            {
                "path": baseline_manifest_path,
                "manifest": candidate,
                "result": candidate_result,
                "metrics": baseline_map,
            }
        )

    if baseline_candidates:
        # Tiered-fallback baseline selection. PASS / PASS_WITH_WARNINGS
        # candidates are preferred (clean reference points). When *every*
        # available baseline failed (FAIL or INCONCLUSIVE), we fall back to
        # using them as anchors anyway — a delta against an already-broken
        # baseline is still meaningful signal ("regressed from already-bad"),
        # and silently degrading to NO_BASELINE in that case hides metric
        # regressions in soak runs that started over SLA. Do NOT re-add an
        # inline filter at the candidate-collection site upstream — that
        # would re-introduce the silent-degradation defect this fallback
        # exists to prevent. See `docs/plans/SCORING_FAILING_BASELINE_COMPARISON_20260422.md`.
        passing_candidates = [
            candidate
            for candidate in baseline_candidates
            if candidate["result"] in {"PASS", "PASS_WITH_WARNINGS"}
        ]
        selected_candidates = passing_candidates if passing_candidates else baseline_candidates
        baseline_manifest = selected_candidates[0]["manifest"]
        grouped_baselines: dict[tuple[str, str], list[float]] = {}
        for candidate in selected_candidates:
            for key, aggregate in candidate["metrics"].items():
                grouped_baselines.setdefault(key, []).append(aggregate.value)
        for key, values in grouped_baselines.items():
            median = _median(values)
            if median is not None:
                baseline_values[key] = median
        if selected_candidates:
            same_git = all(
                str(candidate["manifest"].get("git_sha")) == str(manifest.get("git_sha"))
                for candidate in selected_candidates
            )
            comparison_kind = "run_variance" if same_git else "commit_regression"

    metric_results: list[dict[str, Any]] = []
    hard_failures = 0
    advisory_failures = 0
    info_regressions = 0
    missing_required = 0
    missing_optional = 0
    unsupported_count = 0

    for key in sorted(current_map):
        current = current_map[key]
        policy = policy_map[key]
        baseline_value = baseline_values.get(key)
        if _coverage_activity_metric_is_observation_only(policy, manifest):
            metric_results.append(
                {
                    "metric": current.metric,
                    "run_kind": current.run_kind,
                    "suite_or_profile": current.suite_or_profile,
                    "unit": current.unit,
                    "score_level": "info",
                    "regression_score_level": "info",
                    "required": False,
                    "current_value": current.value,
                    "baseline_value": baseline_value,
                    "delta_abs": None if baseline_value is None else current.value - baseline_value,
                    "delta_pct": _pct_delta(current.value, baseline_value),
                    "sample_count": current.sample_count,
                    "classification": "coverage_observation",
                    "absolute_state": "n/a",
                    "regression_state": "n/a",
                    "score_status": "info",
                    "messages": [
                        "external activity absent by mode coverage; packet/parse count is coverage telemetry"
                    ],
                }
            )
            info_regressions += 1
            continue
        absolute_state, absolute_message = _absolute_state(policy, current.value)
        regression_state, classification, regression_message = _regression_state(
            policy,
            current.value,
            baseline_value,
        )
        score_status = "pass"
        messages = [msg for msg in [absolute_message, regression_message] if msg]

        if absolute_state == "fail":
            if policy.score_level == "hard":
                score_status = "fail"
                hard_failures += 1
            elif policy.score_level == "advisory":
                score_status = "warn"
                advisory_failures += 1
            else:
                score_status = "info"
                info_regressions += 1
        elif regression_state == "fail":
            if policy.regression_score_level == "hard":
                score_status = "fail"
                hard_failures += 1
            elif policy.regression_score_level == "advisory":
                score_status = "warn"
                advisory_failures += 1
            else:
                score_status = "info"
                info_regressions += 1
        elif classification == "regressed" and policy.score_level == "info":
            score_status = "info"
            info_regressions += 1

        metric_results.append(
            {
                "metric": current.metric,
                "run_kind": current.run_kind,
                "suite_or_profile": current.suite_or_profile,
                "unit": current.unit,
                "score_level": policy.score_level,
                "regression_score_level": policy.regression_score_level,
                "required": policy.required,
                "current_value": current.value,
                "baseline_value": baseline_value,
                "delta_abs": None if baseline_value is None else current.value - baseline_value,
                "delta_pct": _pct_delta(current.value, baseline_value),
                "sample_count": current.sample_count,
                "classification": classification,
                "absolute_state": absolute_state,
                "regression_state": regression_state,
                "score_status": score_status,
                "messages": messages,
            }
        )

    for policy in policies:
        if not _policy_applies_to_track(policy, manifest, executed_tracks):
            continue
        key = (_selector_value(policy.selector, "suite_or_profile") or manifest["suite_or_profile"], policy.metric)
        if key in current_map:
            continue
        if policy.metric in unsupported_metric_names:
            unsupported_count += 1
            source_type = str(manifest.get("source_type", "unknown"))
            source_schema = manifest.get("source_schema")
            coverage_status = str(manifest.get("coverage_status", "") or "")
            source_detail = f"{source_type} schema={source_schema}" if source_schema is not None else source_type
            message = f"metric unsupported by source ({source_detail})"
            if coverage_status:
                message += f"; coverage={coverage_status}"
            metric_results.append(
                {
                    "metric": policy.metric,
                    "run_kind": policy.run_kind,
                    "suite_or_profile": key[0],
                    "unit": policy.unit,
                    "score_level": policy.score_level,
                    "regression_score_level": policy.regression_score_level,
                    "required": policy.required,
                    "current_value": None,
                    "baseline_value": None,
                    "delta_abs": None,
                    "delta_pct": None,
                    "sample_count": 0,
                    "classification": "unsupported",
                    "absolute_state": "unsupported",
                    "regression_state": "unsupported",
                    "score_status": "unsupported",
                    "messages": [message],
                }
            )
            continue
        if policy.required:
            score_status = "fail"
        elif policy.score_level == "info":
            score_status = "info"
        else:
            score_status = "warn"
        message = f"metric missing from run output for applicable track ({key[0]})"
        if policy.required:
            hard_failures += 1
            missing_required += 1
        else:
            missing_optional += 1
            if policy.score_level == "info":
                info_regressions += 1
            else:
                advisory_failures += 1
        metric_results.append(
            {
                "metric": policy.metric,
                "run_kind": policy.run_kind,
                "suite_or_profile": key[0],
                "unit": policy.unit,
                "score_level": policy.score_level,
                "regression_score_level": policy.regression_score_level,
                "required": policy.required,
                "current_value": None,
                "baseline_value": None,
                "delta_abs": None,
                "delta_pct": None,
                "sample_count": 0,
                "classification": "missing",
                "absolute_state": "missing",
                "regression_state": "missing",
                "score_status": score_status,
                "messages": [message],
            }
        )

    base_result = str(manifest.get("base_result", manifest.get("result", "PASS")))
    final_result = "PASS"
    if base_result == "FAIL" or hard_failures > 0:
        final_result = "FAIL"
    elif base_result == "INCONCLUSIVE":
        final_result = "INCONCLUSIVE"
    elif advisory_failures > 0 or base_result == "PASS_WITH_WARNINGS":
        final_result = "PASS_WITH_WARNINGS"
    elif comparison_kind == "no_baseline":
        final_result = "NO_BASELINE"

    return {
        "schema_version": 1,
        "manifest": {
            "path": str(manifest_path),
            "run_id": manifest["run_id"],
            "git_sha": manifest["git_sha"],
            "git_ref": manifest["git_ref"],
            "run_kind": manifest["run_kind"],
            "board_id": manifest["board_id"],
            "env": manifest["env"],
            "lane": manifest["lane"],
            "suite_or_profile": manifest["suite_or_profile"],
            "stress_class": manifest["stress_class"],
            "base_result": base_result,
            "source_type": manifest.get("source_type", ""),
            "source_schema": manifest.get("source_schema"),
            "coverage_status": manifest.get("coverage_status", ""),
            "selected_segment": manifest.get("selected_segment"),
        },
        "baseline_manifest": None if baseline_manifest is None else {
            "path": str(selected_candidates[0]["path"]),
            "run_id": baseline_manifest["run_id"],
            "git_sha": baseline_manifest["git_sha"],
            "git_ref": baseline_manifest["git_ref"],
        },
        "baseline_window": {
            "strategy": _baseline_strategy_label(len(selected_candidates) if baseline_candidates else 0),
            "candidate_count": len(selected_candidates) if baseline_candidates else 0,
            "candidates": [
                {
                    "path": str(candidate["path"]),
                    "run_id": str(candidate["manifest"].get("run_id", "")),
                    "git_sha": str(candidate["manifest"].get("git_sha", "")),
                    "git_ref": str(candidate["manifest"].get("git_ref", "")),
                    "result": str(candidate["result"]),
                }
                for candidate in (selected_candidates if baseline_candidates else [])
            ],
        },
        "comparison_kind": comparison_kind,
        "result": final_result,
        "summary": {
            "metrics_scored": len(metric_results),
            "hard_failures": hard_failures,
            "advisory_failures": advisory_failures,
            "info_regressions": info_regressions,
            "missing_required": missing_required,
            "missing_optional": missing_optional,
            "unsupported_metrics": unsupported_count,
        },
        "unsupported_metrics": sorted(unsupported_metric_names),
        "metrics": sorted(
            metric_results,
            key=lambda item: (
                0 if item["score_status"] == "fail"
                else 1 if item["score_status"] == "warn"
                else 2 if item["score_status"] == "info"
                else 3 if item["score_status"] == "unsupported"
                else 4,
                str(item["suite_or_profile"]),
                str(item["metric"]),
            ),
        ),
    }


def render_human(result: dict[str, Any]) -> str:
    manifest = result["manifest"]
    baseline = result.get("baseline_manifest")
    baseline_window = result.get("baseline_window") or {}
    lines = [
        "# Hardware Run Score",
        "",
        f"- Result: **{result['result']}**",
        f"- Base result: `{manifest['base_result']}`",
        f"- Run kind: `{manifest['run_kind']}`",
        f"- Lane: `{manifest['lane']}`",
        f"- Track: `{manifest['suite_or_profile']}`",
        f"- Stress class: `{manifest['stress_class']}`",
        f"- Board: `{manifest['board_id']}`",
        f"- Git: `{manifest['git_sha']}` ({manifest['git_ref']})",
        f"- Comparison: `{result['comparison_kind']}`",
    ]
    if manifest.get("source_type"):
        lines.append(f"- Source type: `{manifest['source_type']}`")
    if manifest.get("source_schema") is not None:
        lines.append(f"- Source schema: `{manifest['source_schema']}`")
    if manifest.get("coverage_status"):
        lines.append(f"- Coverage status: `{manifest['coverage_status']}`")
    if manifest.get("selected_segment"):
        lines.append(f"- Selected segment: `{manifest['selected_segment']}`")
    if baseline:
        lines.append(f"- Baseline git: `{baseline['git_sha']}` ({baseline['git_ref']})")
    if baseline_window.get("strategy") and baseline_window.get("strategy") != "none":
        lines.append(f"- Baseline strategy: `{baseline_window['strategy']}`")
        lines.append(f"- Baseline candidates: {baseline_window.get('candidate_count', 0)}")

    summary = result["summary"]
    lines.extend(
        [
            "",
            "## Summary",
            "",
            f"- Metrics scored: {summary['metrics_scored']}",
            f"- Hard failures: {summary['hard_failures']}",
            f"- Advisory failures: {summary['advisory_failures']}",
            f"- Info regressions: {summary['info_regressions']}",
            f"- Missing required: {summary['missing_required']}",
            f"- Missing optional: {summary['missing_optional']}",
            f"- Unsupported metrics: {summary.get('unsupported_metrics', 0)}",
            "",
            "## Metrics",
            "",
            "| Status | Track | Metric | Current | Baseline | Delta | Delta % | Classification |",
            "|--------|-------|--------|--------:|---------:|------:|--------:|----------------|",
        ]
    )
    noteworthy: list[str] = []
    for metric in result["metrics"]:
        delta_pct = _format_num(metric["delta_pct"])
        if delta_pct != "n/a":
            delta_pct = f"{delta_pct}%"
        lines.append(
            "| "
            f"{metric['score_status'].upper()} | "
            f"`{metric['suite_or_profile']}` | "
            f"`{metric['metric']}` | "
            f"{_format_num(metric['current_value'])} | "
            f"{_format_num(metric['baseline_value'])} | "
            f"{_format_num(metric['delta_abs'])} | "
            f"{delta_pct} | "
            f"{metric['classification']} |"
        )
        for message in metric["messages"]:
            noteworthy.append(f"- `{metric['suite_or_profile']}` / `{metric['metric']}`: {message}")
    if noteworthy:
        lines.extend(["", "## Findings", ""])
        lines.extend(noteworthy)
    return "\n".join(lines) + "\n"


def main() -> int:
    args = parse_args()
    manifest_path = Path(args.manifest).resolve()
    catalog_path = Path(args.catalog).resolve()
    baseline_paths = [Path(path).resolve() for path in args.compare_to if path]

    try:
        result = score_run(manifest_path, catalog_path, baseline_paths)
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        # Emit minimal valid JSON to stdout so shell redirection never
        # leaves scoring.json as a 0-byte file (which breaks trend
        # comparison downstream).
        if args.json:
            error_payload = {
                "schema_version": 1,
                "result": "ERROR",
                "comparison_kind": "error",
                "summary": {"reason": str(exc)},
            }
            print(json.dumps(error_payload, indent=2))
        return 3

    if args.json:
        print(json.dumps(result, indent=2))
    else:
        print(render_human(result), end="")

    final_result = str(result["result"])
    if final_result == "FAIL":
        return 2
    if final_result == "PASS_WITH_WARNINGS":
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

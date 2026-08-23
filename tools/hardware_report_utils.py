#!/usr/bin/env python3
"""Shared rendering helpers for hardware scoring artifacts."""

from __future__ import annotations

import csv
from pathlib import Path
from typing import Any


def _safe_float(value: Any) -> float | None:
    if value is None or value == "":
        return None
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def _format_num(value: Any) -> str:
    numeric = _safe_float(value)
    if numeric is None:
        return "n/a"
    if abs(numeric - round(numeric)) < 1e-9:
        return str(int(round(numeric)))
    return f"{numeric:.3f}"


def write_comparison_tsv(scoring: dict[str, Any], out_path: Path) -> None:
    header = [
        "score_status",
        "suite_or_profile",
        "metric",
        "current_value",
        "baseline_value",
        "delta_abs",
        "delta_pct",
        "classification",
        "unit",
        "score_level",
        "required",
    ]
    with out_path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle, delimiter="\t")
        writer.writerow(header)
        for metric in scoring.get("metrics", []):
            writer.writerow(
                [
                    metric.get("score_status", ""),
                    metric.get("suite_or_profile", ""),
                    metric.get("metric", ""),
                    metric.get("current_value", ""),
                    metric.get("baseline_value", ""),
                    metric.get("delta_abs", ""),
                    metric.get("delta_pct", ""),
                    metric.get("classification", ""),
                    metric.get("unit", ""),
                    metric.get("score_level", ""),
                    metric.get("required", ""),
                ]
            )


def write_comparison_text(scoring: dict[str, Any], out_path: Path) -> None:
    manifest = scoring.get("manifest", {})
    baseline = scoring.get("baseline_manifest") or {}
    baseline_window = scoring.get("baseline_window") or {}
    summary = scoring.get("summary", {})
    metrics = scoring.get("metrics", [])

    rows = [
        [
            str(metric.get("score_status", "")).upper(),
            str(metric.get("suite_or_profile", "")),
            str(metric.get("metric", "")),
            _format_num(metric.get("current_value")),
            _format_num(metric.get("baseline_value")),
            _format_num(metric.get("delta_abs")),
            "n/a" if metric.get("delta_pct") is None else f"{_format_num(metric.get('delta_pct'))}%",
            str(metric.get("classification", "")),
        ]
        for metric in metrics
    ]
    widths = [len(label) for label in ["STATUS", "TRACK", "METRIC", "CURRENT", "PREVIOUS", "DELTA", "DELTA%", "CLASS"]]
    for row in rows:
        for idx, value in enumerate(row):
            widths[idx] = max(widths[idx], len(value))

    def fmt_row(values: list[str]) -> str:
        return "  ".join(value.ljust(widths[idx]) for idx, value in enumerate(values))

    lines = [
        f"track: {manifest.get('suite_or_profile', '')}",
        f"run_kind: {manifest.get('run_kind', '')}",
        f"board: {manifest.get('board_id', '')}",
        f"git: {manifest.get('git_sha', '')} ({manifest.get('git_ref', '')})",
        f"result: {scoring.get('result', '')}",
        f"comparison: {scoring.get('comparison_kind', '')}",
        f"baseline_git: {baseline.get('git_sha', 'n/a')}",
        f"baseline_strategy: {baseline_window.get('strategy', 'none')}",
        f"baseline_candidates: {baseline_window.get('candidate_count', 0)}",
        f"source_type: {manifest.get('source_type', '') or 'n/a'}",
        f"source_schema: {manifest.get('source_schema', 'n/a')}",
        f"coverage_status: {manifest.get('coverage_status', '') or 'n/a'}",
        f"metrics_scored: {summary.get('metrics_scored', 0)}",
        f"hard_failures: {summary.get('hard_failures', 0)}",
        f"advisory_failures: {summary.get('advisory_failures', 0)}",
        f"info_regressions: {summary.get('info_regressions', 0)}",
        f"unsupported_metrics: {summary.get('unsupported_metrics', 0)}",
        "",
        fmt_row(["STATUS", "TRACK", "METRIC", "CURRENT", "PREVIOUS", "DELTA", "DELTA%", "CLASS"]),
        fmt_row(["-" * widths[0], "-" * widths[1], "-" * widths[2], "-" * widths[3], "-" * widths[4], "-" * widths[5], "-" * widths[6], "-" * widths[7]]),
    ]
    if manifest.get("selected_segment"):
        lines.insert(8, f"selected_segment: {manifest.get('selected_segment')}")
    lines.extend(fmt_row(row) for row in rows)
    lines.append("")

    findings: list[str] = []
    for metric in metrics:
        for message in metric.get("messages", []):
            findings.append(f"{metric.get('suite_or_profile', '')}/{metric.get('metric', '')}: {message}")
    if findings:
        lines.append("findings:")
        lines.extend(f"  - {item}" for item in findings)
        lines.append("")

    out_path.write_text("\n".join(lines), encoding="utf-8")

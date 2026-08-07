#!/usr/bin/env python3
"""Regression tests for shared offline metric schema rules."""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))

import metric_schema  # type: ignore  # noqa: E402


def assert_true(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    legacy_unsupported = metric_schema.unsupported_metrics_for_perf_csv(12, {"millis", "rx"})
    assert_true(
        {"perf_drop_delta", "event_drop_delta", "samples_to_stable", "time_to_stable_ms"} <= legacy_unsupported,
        f"legacy CSV capability mismatch: {legacy_unsupported}",
    )
    assert_true(
        metric_schema.coverage_status_for_unsupported_metrics(legacy_unsupported) == "partial_legacy_import",
        f"legacy coverage status mismatch: {legacy_unsupported}",
    )

    schema13_unsupported = metric_schema.unsupported_metrics_for_perf_csv(13, {"perfDrop", "eventBusDrops", "millis"})
    assert_true(
        schema13_unsupported == {"samples_to_stable", "time_to_stable_ms"},
        f"schema13 capability mismatch: {schema13_unsupported}",
    )
    assert_true(
        metric_schema.coverage_status_for_unsupported_metrics(schema13_unsupported) == "full_runtime_gates",
        f"schema13 coverage status mismatch: {schema13_unsupported}",
    )

    assert_true(
        metric_schema.SOAK_TREND_METRIC_UNITS["dma_fragmentation_pct_p95"] == "percent",
        "canonical fragmentation unit must stay percent",
    )
    assert_true(
        metric_schema.kv_source_key("connect_burst_pre_ble_process_peak_us") == "connect_burst_pre_ble_process_peak",
        "connect-burst alias missing",
    )
    assert_true(
        metric_schema.kv_source_key("display_frequency_peak_us") == "display_frequency_peak",
        "display peak alias missing",
    )
    assert_true(
        metric_schema.kv_source_key("metrics_ok_samples") == "ok_samples",
        "metrics_ok_samples alias missing",
    )
    assert_true(
        all(metric in metric_schema.CANONICAL_METRIC_UNITS for metric in metric_schema.SOAK_TREND_METRIC_UNITS),
        "soak metric units must derive from canonical unit map",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

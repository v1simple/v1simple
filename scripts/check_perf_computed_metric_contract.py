#!/usr/bin/env python3
"""Validate that every computed SLO metric is implemented by score_perf_csv.py."""

from __future__ import annotations

import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TOOLS_DIR = ROOT / "tools"
sys.path.insert(0, str(TOOLS_DIR))

import score_perf_csv  # type: ignore  # noqa: E402


SLO_PATH = ROOT / "tools" / "perf_slo_thresholds.json"
COLUMN_CONTRACT_PATH = ROOT / "test" / "contracts" / "perf_csv_column_contract.txt"


def load_header_columns() -> list[str]:
    return [
        line.strip()
        for line in COLUMN_CONTRACT_PATH.read_text(encoding="utf-8").splitlines()
        if line.strip() and not line.startswith("#")
    ]


def build_rows() -> list[dict[str, int]]:
    columns = load_header_columns()

    start = {column: 0 for column in columns}
    end = {column: 0 for column in columns}

    start["millis"] = 0
    end["millis"] = 60000
    end["displayUpdates"] = 100
    end["displaySkips"] = 30
    end["cmdPaceNotYet"] = 5

    return [start, end]


def computed_metrics(config: score_perf_csv.ThresholdConfig) -> list[str]:
    metrics: set[str] = set()
    for metric, source, _op, _limit in config.hard_computed:
        if source == "computed":
            metrics.add(metric)
    for metric, source, _op, _limit in config.advisory:
        if source == "computed":
            metrics.add(metric)
    return sorted(metrics)


def main() -> int:
    try:
        config = score_perf_csv.load_threshold_config(SLO_PATH)
        rows = build_rows()
        metrics = computed_metrics(config)
        if not metrics:
            raise RuntimeError("No computed metrics found in SLO config")

        for metric in metrics:
            score_perf_csv.compute_value(rows, metric)
    except Exception as exc:
        print(f"[contract] perf-computed-metrics: {exc}")
        return 1

    print(
        "[contract] perf-computed-metrics supported by scorer "
        f"({len(metrics)} metrics: {', '.join(metrics)})"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())

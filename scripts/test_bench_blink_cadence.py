#!/usr/bin/env python3
"""Focused tests for the advisory replay blink-cadence decoder."""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from bench_blink_cadence import (  # noqa: E402
    _classify_orange_counts,
    summarize_boolean_timeline,
    summarize_camera_episode,
)


def assert_true(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def test_mean_is_authoritative_for_asymmetric_duty() -> None:
    intervals_ms = [115.0, 81.0, 115.0, 81.0, 115.0, 81.0, 115.0, 81.0, 115.0]
    samples = [(-0.010, True), (0.0, False)]
    time_s = 0.0
    state = False
    for duration_ms in intervals_ms:
        time_s += duration_ms / 1000.0
        state = not state
        samples.append((time_s, state))

    summary = summarize_boolean_timeline(samples, gap_limit_ms=None)
    expected_mean = sum(intervals_ms) / len(intervals_ms)
    assert_true(summary["status"] == "complete", f"asymmetric timeline failed: {summary}")
    assert_true(abs(summary["mean_half_period_ms"] - expected_mean) < 0.001, "mean changed")
    assert_true(summary["median_half_period_ms"] == 115.0, "fixture no longer exposes median bias")
    assert_true(
        abs(summary["mean_half_period_ms"] - summary["median_half_period_ms"]) > 10.0,
        "misleading bimodal median was hidden",
    )


def test_pts_gap_longer_than_half_period_forces_abstention() -> None:
    samples = [
        (0.000, False),
        (0.005, False),
        (0.120, True),
        (0.125, True),
        (0.200, False),
        (0.205, False),
    ]
    summary = summarize_boolean_timeline(samples, gap_limit_ms=96.0)
    assert_true(summary["status"] == "partial", f"unsafe PTS gap was counted: {summary}")
    assert_true(summary["reason"] == "pts_gap_exceeds_half_period", "wrong gap reason")
    assert_true(summary["maximum_sample_gap_ms"] == 115.0, "maximum PTS gap changed")
    assert_true("mean_half_period_ms" not in summary, "abstention still published a cadence")


def test_optical_response_tail_is_one_on_state() -> None:
    samples: list[tuple[float, int]] = []
    time_s = 0.0
    for _cycle in range(6):
        for _frame in range(16):
            samples.append((time_s, 200))
            time_s += 0.005
        for frame in range(23):
            samples.append((time_s, 2300 if frame < 4 else 1100))
            time_s += 0.005

    classified, calibration = _classify_orange_counts(samples)
    summary = summarize_boolean_timeline(classified, gap_limit_ms=96.0)
    assert_true(summary["status"] == "complete", f"trimodal optical signal failed: {summary}")
    assert_true(summary["transition_count"] == 11, f"response tail invented transitions: {summary}")
    assert_true(calibration["orange_low_center"] < calibration["off_threshold"], "off split changed")
    assert_true(
        calibration["on_threshold"] < calibration["orange_response_center"],
        "response tail is no longer classified as lit",
    )


def test_only_pts_gaps_intersecting_episode_force_abstention() -> None:
    regular = [(index * 0.05, bool((index // 2) % 2)) for index in range(21)]
    outside = [(-0.25, False), (-0.13, False), *regular]
    outside_summary = summarize_camera_episode(outside, start_s=0.0, end_s=1.0)
    assert_true(outside_summary["status"] == "complete", f"outside gap blocked episode: {outside_summary}")

    straddling = [(-0.08, False), (0.04, False), *regular[1:]]
    straddling_summary = summarize_camera_episode(straddling, start_s=0.0, end_s=1.0)
    assert_true(straddling_summary["status"] == "partial", "straddling gap was counted")
    assert_true(
        straddling_summary["reason"] == "pts_gap_exceeds_half_period",
        "straddling gap has the wrong abstention reason",
    )


def main() -> int:
    test_mean_is_authoritative_for_asymmetric_duty()
    test_pts_gap_longer_than_half_period_forces_abstention()
    test_optical_response_tail_is_one_on_state()
    test_only_pts_gaps_intersecting_episode_force_abstention()
    print("bench blink cadence tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

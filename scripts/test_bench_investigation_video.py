#!/usr/bin/env python3
"""Focused regressions for generic investigation video evidence."""

from __future__ import annotations

import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts" / "bench"))

from investigation_video import inspect_video  # noqa: E402


def assert_true(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def make_corner_change_video(path: Path, *, brief: bool = False) -> None:
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        raise AssertionError("ffmpeg is required for investigation video tests")
    subprocess.run(
        [
            ffmpeg,
            "-y",
            "-nostdin",
            "-hide_banner",
            "-loglevel",
            "error",
            "-f",
            "lavfi",
            "-i",
            (
                "color=c=black:s=96x64:r=30:d=2,"
                "drawbox=x=72:y=40:w=24:h=24:color=white:t=fill:enable='"
                + ("between(t,1.06,1.18)'" if brief else "gte(t,1)'")
            ),
            "-c:v",
            "ffv1",
            str(path),
        ],
        check=True,
    )


def test_full_frame_candidates_and_requested_interval_sheet() -> None:
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        video = root / "corner-change.mkv"
        output = root / "images"
        make_corner_change_video(video)
        report = inspect_video(
            video,
            output,
            requested_intervals=[
                {"start_seconds": 0.7, "end_seconds": 1.3, "sample_rate_hz": 10.0}
            ],
        )

        assert_true(report["status"] == "complete", f"video inspection failed: {report}")
        assert_true(report["temporal_scan"]["spatial_scope"] == "full_frame", "scan gained a crop")
        candidates = report["temporal_scan"]["change_candidates"]
        assert_true(candidates, f"corner-only temporal change was missed: {report['temporal_scan']}")
        closest = min(candidates, key=lambda item: abs(item["pts_seconds"] - 1.0))
        assert_true(abs(closest["pts_seconds"] - 1.0) <= 0.25, f"wrong change PTS: {closest}")
        assert_true(closest["change_score"] > 0, f"change candidate lost its score: {closest}")

        change_images = report["change_images"]
        assert_true(change_images, f"change candidates have no immediate visual evidence: {report}")
        closest_image = min(change_images, key=lambda item: abs(item["pts_seconds"] - 1.0))
        assert_true(closest_image["status"] == "complete", f"change image failed: {closest_image}")
        assert_true(
            closest_image["purpose"] == "temporal_change_candidate",
            f"change sheet purpose is ambiguous: {closest_image}",
        )
        assert_true(closest_image["frame_count"] <= 3, f"change sheet is unbounded: {closest_image}")
        assert_true(
            min(
                abs(cell["nominal_requested_pts_seconds"] - 1.0)
                for cell in closest_image["cells"]
            )
            <= 0.25,
            f"change sheet omitted its candidate PTS: {closest_image}",
        )
        assert_true(
            (output / closest_image["filename"]).stat().st_size > 0,
            f"change image is missing: {closest_image}",
        )

        overview = report["overview"]
        interval = report["requested_intervals"][0]
        assert_true(overview["status"] == "complete", f"overview failed: {overview}")
        assert_true(interval["status"] == "complete", f"interval failed: {interval}")
        assert_true(overview["purpose"] == "whole_video_overview", f"wrong overview purpose: {overview}")
        assert_true(interval["purpose"] == "requested_interval", f"wrong interval purpose: {interval}")
        assert_true(overview["frame_count"] <= 12, f"overview is unbounded: {overview}")
        assert_true(interval["frame_count"] <= 36, f"interval sheet is unbounded: {interval}")
        assert_true(
            interval["sample_rate_hz"] > overview["sample_rate_hz"],
            f"requested interval was not sampled at a higher rate: {interval}",
        )
        for item in (overview, closest_image, interval):
            image_path = Path(item["filename"])
            assert_true(not image_path.is_absolute() and ".." not in image_path.parts, f"unsafe image path: {item}")
            assert_true((output / image_path).stat().st_size > 0, f"missing image: {item}")
            assert_true("path" not in item, f"sheet retained an ambiguous path: {item}")
            assert_true(item["layout"]["cell_order"] == "row_major", f"cell order missing: {item}")
            assert_true(
                item["layout"]["columns"] * item["layout"]["rows"] >= item["frame_count"],
                f"layout cannot contain its cells: {item}",
            )
            expected_labels = [f"cell_{index + 1:03d}" for index in range(item["frame_count"])]
            assert_true(
                [cell["cell_label"] for cell in item["cells"]] == expected_labels,
                f"cell labels are not ordered: {item}",
            )
            for cell in item["cells"]:
                nominal = cell["nominal_requested_pts_seconds"]
                uncertainty = cell["pts_uncertainty_interval"]
                assert_true(cell["source_pts_measured"] is False, f"nominal time became source PTS: {cell}")
                assert_true("pts_seconds" not in cell, f"nominal time is mislabeled as measured PTS: {cell}")
                assert_true(
                    uncertainty == item["interval"],
                    f"cell uncertainty lost the extraction interval: {cell}",
                )
                assert_true(
                    uncertainty["start_pts_seconds"] <= nominal <= uncertainty["end_pts_seconds"],
                    f"nominal time is outside its uncertainty: {cell}",
                )
                assert_true(cell["pts_uncertainty_seconds"] >= 0, f"negative uncertainty: {cell}")

        coverage = report["coverage"]["higher_rate"]
        assert_true(
            coverage["sampled_intervals"] == [{"start_seconds": 0.7, "end_seconds": 1.3}],
            f"sampled interval was not recorded: {coverage}",
        )
        assert_true(
            coverage["unsampled_intervals"]
            == [
                {"start_seconds": 0.0, "end_seconds": 0.7},
                {"start_seconds": 1.3, "end_seconds": 2.0},
            ],
            f"unsampled intervals were not recorded: {coverage}",
        )
        scan_coverage = report["coverage"]["full_frame_scan"]
        sampled_pts = report["temporal_scan"]["sampled_pts_seconds"]
        assert_true(
            scan_coverage["temporal_coverage"] == "sample_points_only"
            and scan_coverage["continuous_coverage"] is False,
            f"periodic scan claimed continuous coverage: {scan_coverage}",
        )
        assert_true(
            scan_coverage["sampled_pts_seconds"] == sampled_pts,
            f"scan coverage lost sampled PTS points: {scan_coverage}",
        )
        assert_true(
            scan_coverage["nominal_cadence_seconds"] > 0,
            f"scan cadence is missing: {scan_coverage}",
        )
        gaps = scan_coverage["between_point_gaps"]
        assert_true(gaps["count"] == max(0, len(sampled_pts) - 1), f"wrong gap count: {gaps}")
        assert_true(gaps["all_interiors_unsampled"] is bool(gaps["count"]), f"gap truth missing: {gaps}")
        assert_true("sampled_intervals" not in scan_coverage, f"scan restored continuous coverage: {scan_coverage}")
        json.dumps(report, allow_nan=False)
        assert_true(str(root) not in json.dumps(report), "report leaked its temporary absolute path")


def test_extraction_failures_and_invalid_requests_remain_machine_readable() -> None:
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        video = root / "corner-change.mkv"
        output = root / "images"
        make_corner_change_video(video)
        failing_tool = shutil.which("false")
        if not failing_tool:
            raise AssertionError("false is required for failure-path testing")
        report = inspect_video(
            video,
            output,
            requested_intervals=[(0.2, 0.6), (0.9, 0.8), (3.0, 4.0)],
            ffmpeg=failing_tool,
        )

        assert_true(report["status"] == "partial", f"extraction failure became success: {report}")
        assert_true(report["temporal_scan"]["status"] == "failed", "failed scan looked complete")
        assert_true(report["overview"]["status"] == "failed", "failed overview looked complete")
        states = [item["status"] for item in report["requested_intervals"]]
        assert_true(states == ["failed", "not_sampled", "not_sampled"], f"wrong request states: {states}")
        codes = {(item["stage"], item["code"]) for item in report["errors"]}
        assert_true(("temporal_scan", "ffmpeg_failed") in codes, f"scan error missing: {codes}")
        assert_true(("overview", "ffmpeg_failed") in codes, f"overview error missing: {codes}")
        assert_true(
            ("requested_interval", "interval_range_invalid") in codes,
            f"invalid interval was hidden: {codes}",
        )
        assert_true(
            report["coverage"]["full_frame_scan"]["unsampled_edge_intervals"]
            == [
                {
                    "start_seconds": 0.0,
                    "end_seconds": 2.0,
                    "sampled_boundary": None,
                }
            ],
            f"failed scan claimed coverage: {report['coverage']}",
        )
        assert_true(
            report["coverage"]["full_frame_scan"]["temporal_coverage"] == "none",
            f"failed scan retained sample points: {report['coverage']}",
        )
        assert_true(
            report["coverage"]["higher_rate"]["unsampled_intervals"]
            == [{"start_seconds": 0.0, "end_seconds": 2.0}],
            f"failed interval extraction claimed coverage: {report['coverage']}",
        )
        serialized = json.dumps(report, allow_nan=False)
        assert_true(str(root) not in serialized, "failure report leaked its temporary absolute path")


def test_brief_off_overview_change_becomes_a_candidate() -> None:
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        video = root / "brief-change.mkv"
        make_corner_change_video(video, brief=True)
        report = inspect_video(video, root / "images")
        candidates = report["temporal_scan"]["change_candidates"]
        assert_true(
            any(1.0 <= item["pts_seconds"] <= 1.25 for item in candidates),
            f"brief full-frame change was missed: {candidates}",
        )


def main() -> int:
    test_full_frame_candidates_and_requested_interval_sheet()
    test_extraction_failures_and_invalid_requests_remain_machine_readable()
    test_brief_off_overview_change_becomes_a_candidate()
    print("bench investigation video tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

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

from investigation_video import CommandError, build_frame_index, inspect_video  # noqa: E402


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


def make_many_change_video(path: Path) -> None:
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
                "color=c=black:s=96x64:r=30:d=6,"
                "drawbox=x=12:y=8:w=72:h=48:color=white:t=fill:"
                "enable='gte(sin(10*PI*t),0)'"
            ),
            "-c:v",
            "ffv1",
            str(path),
        ],
        check=True,
    )


def make_single_frame_change_video(path: Path) -> None:
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
                "color=c=black:s=96x64:r=30:d=1,"
                "drawbox=x=72:y=40:w=24:h=24:color=white:t=fill:enable='eq(n,7)'"
            ),
            "-c:v",
            "ffv1",
            str(path),
        ],
        check=True,
    )


def image_dimensions(path: Path) -> tuple[int, int]:
    ffprobe = shutil.which("ffprobe")
    if not ffprobe:
        raise AssertionError("ffprobe is required for investigation video tests")
    process = subprocess.run(
        [
            ffprobe,
            "-v",
            "error",
            "-select_streams",
            "v:0",
            "-show_entries",
            "stream=width,height",
            "-of",
            "csv=p=0:s=x",
            str(path),
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    width, height = process.stdout.strip().split("x", 1)
    return int(width), int(height)


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
                if item["purpose"] == "whole_video_overview":
                    assert_true(
                        cell["source_pts_measured"] is False,
                        f"overview nominal time became measured PTS: {cell}",
                    )
                    assert_true(
                        uncertainty == item["interval"],
                        f"overview uncertainty lost its extraction interval: {cell}",
                    )
                else:
                    assert_true(
                        cell["source_pts_measured"] is True
                        and cell["source_pts_seconds"] == nominal
                        and isinstance(cell["source_pts_value"], int)
                        and cell["source_pts_time_base"]["numerator"] > 0
                        and cell["source_pts_time_base"]["denominator"] > 0
                        and isinstance(cell["source_frame_index"], int),
                        f"exact source position is missing: {cell}",
                    )
                    assert_true(
                        uncertainty
                        == {"start_pts_seconds": nominal, "end_pts_seconds": nominal}
                        and cell["pts_uncertainty_seconds"] == 0,
                        f"measured PTS retained synthetic uncertainty: {cell}",
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
        assert_true(
            scan_coverage["temporal_coverage"] == "exhaustive_frame_index"
            and scan_coverage["sampling"] == "every_decoded_source_frame"
            and scan_coverage["indexed_frame_count"] == 60
            and scan_coverage["frame_indices_contiguous"] is True
            and scan_coverage["semantic_review"] is False
            and scan_coverage["continuous_coverage"] is False
            and scan_coverage["unsampled_edge_intervals"] == [],
            f"native frame index coverage is inaccurate: {scan_coverage}",
        )
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
        assert_true(states == ["not_sampled", "not_sampled", "not_sampled"], f"wrong request states: {states}")
        codes = {(item["stage"], item["code"]) for item in report["errors"]}
        assert_true(("frame_index", "ffmpeg_failed") in codes, f"frame index error missing: {codes}")
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


def test_single_native_frame_change_and_hash_cache() -> None:
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        first = root / "first.mkv"
        second = root / "second.mkv"
        index = root / "investigation_index"
        make_single_frame_change_video(first)
        report = inspect_video(first, root / "images", index_dir=index)
        assert_true(
            report["temporal_scan"]["frame_count"] == 30
            and any(
                item["frame_index"] in {7, 8}
                for item in report["temporal_scan"]["change_candidates"]
            ),
            f"single native frame was not scored: {report['temporal_scan']}",
        )
        shutil.copyfile(first, second)
        digest = report["frame_index_summary"]["source_video_sha256"]
        failing_tool = shutil.which("false")
        if not failing_tool:
            raise AssertionError("false is required for cache-path testing")
        cached_summary, cached_rows = build_frame_index(
            second, index, video_sha256=digest, ffmpeg=failing_tool
        )
        assert_true(
            cached_summary["frame_count"] == len(cached_rows) == 30,
            "byte-identical video did not reuse its hash-keyed frame index",
        )
        second.write_bytes(second.read_bytes() + b"changed")
        try:
            build_frame_index(
                second,
                index,
                video_sha256=digest,
                ffmpeg=failing_tool,
            )
        except CommandError as error:
            assert_true(
                error.code == "video_hash_mismatch",
                f"caller-supplied stale hash was trusted: {error.code}",
            )
        else:
            raise AssertionError("changed video bytes reused a caller-selected stale index")


def test_cache_summary_integrity_symlink_guard_and_empty_interval() -> None:
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        video = root / "source.mkv"
        index = root / "index"
        make_single_frame_change_video(video)
        summary, _rows = build_frame_index(video, index)
        summary_path = index / f"{summary['source_video_sha256']}.summary.json"
        corrupted = json.loads(summary_path.read_text(encoding="utf-8"))
        corrupted["unexpected_private_text"] = "must not enter model context"
        summary_path.write_text(json.dumps(corrupted) + "\n", encoding="utf-8")
        failing_tool = shutil.which("false")
        if not failing_tool:
            raise AssertionError("false is required for cache-integrity testing")
        try:
            build_frame_index(video, index, ffmpeg=failing_tool)
        except CommandError as error:
            assert_true(
                error.code == "ffmpeg_failed",
                f"corrupt cache was accepted or failed incorrectly: {error.code}",
            )
        else:
            raise AssertionError("modified cache summary was accepted")

        target = root / "target"
        target.mkdir()
        linked = root / "linked-index"
        linked.symlink_to(target, target_is_directory=True)
        try:
            build_frame_index(video, linked)
        except CommandError as error:
            assert_true(
                error.code == "frame_index_directory_invalid",
                f"symlink index directory was not rejected: {error.code}",
            )
        else:
            raise AssertionError("symlink index directory was accepted")
        symlink_report = inspect_video(
            video, root / "symlink-sheets", index_dir=linked, scan_overview=False
        )
        assert_true(
            any(
                error["stage"] == "frame_index"
                and error["code"] == "frame_index_directory_invalid"
                for error in symlink_report["errors"]
            ),
            f"inspect_video bypassed the symlink index guard: {symlink_report}",
        )

        report = inspect_video(
            video,
            root / "sheets",
            [{"start_seconds": 0.001, "end_seconds": 0.002, "sample_rate_hz": 30}],
            index_dir=root / "fresh-index",
            scan_overview=False,
        )
        interval = report["requested_intervals"][0]
        assert_true(
            interval["status"] == "not_sampled"
            and interval["reason"] == "no_source_frame_in_interval",
            f"out-of-window frame was attached as complete evidence: {interval}",
        )


def test_all_ranked_changes_are_packed_into_one_lossless_atlas() -> None:
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        video = root / "many-changes.mkv"
        output = root / "images"
        make_many_change_video(video)
        report = inspect_video(video, output)

        candidates = report["temporal_scan"]["change_candidates"]
        assert_true(len(candidates) == 12, f"fixture did not fill the candidate bound: {candidates}")
        assert_true(report["status"] == "complete", f"atlas extraction failed: {report['errors']}")
        assert_true(len(report["change_images"]) == 1, "ranked changes still consume one image each")
        atlas = report["change_images"][0]
        assert_true(
            atlas["purpose"] == "temporal_change_candidates"
            and atlas["filename"].endswith(".png")
            and (output / atlas["filename"]).stat().st_size > 0,
            f"lossless change atlas is missing: {atlas}",
        )
        assert_true(
            atlas["layout"] == {"columns": 6, "rows": 8, "cell_order": "row_major"},
            f"visible frames are not mapped onto the physical atlas grid: {atlas['layout']}",
        )
        panel_width, panel_height = image_dimensions(output / "change_01.png")
        atlas_width, atlas_height = image_dimensions(output / atlas["filename"])
        assert_true(
            (atlas_width, atlas_height) == (panel_width * 3, panel_height * 4),
            f"candidate panels overlap in the rendered atlas: "
            f"{(atlas_width, atlas_height)} vs {(panel_width * 3, panel_height * 4)}",
        )
        assert_true(
            atlas["frame_count"] == len(atlas["cells"]) == 36,
            f"not every visible before/at/after frame has metadata: {atlas['cells']}",
        )
        cells_by_rank = {
            rank: [
                cell
                for cell in atlas["cells"]
                if cell["cell_label"].startswith(f"change_rank_{rank:02d}_")
            ]
            for rank in range(1, 13)
        }
        assert_true(
            all(
                [cell["cell_label"] for cell in cells_by_rank[rank]]
                == [f"change_rank_{rank:02d}_cell_{index:03d}" for index in range(1, 4)]
                for rank in range(1, 13)
            ),
            f"candidate subframe mapping was lost: {cells_by_rank}",
        )
        assert_true(
            all(
                any(
                    cell["nominal_requested_pts_seconds"] == candidate["pts_seconds"]
                    for cell in cells_by_rank[int(candidate["change_rank"])]
                )
                for candidate in candidates
            ),
            "candidate PTS values no longer resolve to visible atlas cells",
        )
        assert_true(
            [cell["cell_index"] for cell in atlas["cells"]]
            == sorted(cell["cell_index"] for cell in atlas["cells"]),
            f"atlas cells are not published in physical row-major order: {atlas['cells']}",
        )
        for cell in atlas["cells"]:
            interval = cell["pts_uncertainty_interval"]
            assert_true(
                interval["start_pts_seconds"]
                <= cell["nominal_requested_pts_seconds"]
                <= interval["end_pts_seconds"],
                f"atlas cell lost its candidate interval: {cell}",
            )


def main() -> int:
    test_full_frame_candidates_and_requested_interval_sheet()
    test_extraction_failures_and_invalid_requests_remain_machine_readable()
    test_brief_off_overview_change_becomes_a_candidate()
    test_single_native_frame_change_and_hash_cache()
    test_cache_summary_integrity_symlink_guard_and_empty_interval()
    test_all_ranked_changes_are_packed_into_one_lossless_atlas()
    print("bench investigation video tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

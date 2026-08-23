#!/usr/bin/env python3
"""Focused regressions for bounded bench evidence access."""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import bench_evidence as evidence  # noqa: E402
import bench_investigate as investigator  # noqa: E402


def assert_true(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def write_json(path: Path, payload: object) -> None:
    path.write_text(json.dumps(payload, sort_keys=True) + "\n", encoding="utf-8")


def clock_alignment(
    *,
    slope: int = 1000,
    segment: str = "fixture-segment",
    instance: int = 1,
    reused: bool = False,
) -> dict[str, object]:
    payload: dict[str, object] = {
        "schema_version": 1,
        "kind": "bench_clock_alignment",
        "model": "fixture",
        "segments": [
            {
                "schema_version": 1,
                "kind": "bench_clock_mapping",
                "clock_segment": segment,
                "segment_instance": instance,
                "mapping_id": f"{segment}:{instance}",
                "fit_status": "fitted",
                "fit_type": "affine",
                "fit_quality": "good",
                "poor_fit": False,
                "slope_ns_per_us": float(slope),
                "slope_numerator": slope,
                "slope_denominator": 1,
                "reference_dut_us": 0,
                "reference_host_ns": 0,
                "reference_host_ns_numerator": 0,
                "reference_host_ns_denominator": 1,
                "host_error_bounds_ns": {"earliest": -1000, "latest": 1000},
                "validity_dut_us": {"start": 0, "end": 10_000_000},
                "uncertainty_width_ns": 2000,
            }
        ],
    }
    if reused:
        duplicate = dict(payload["segments"][0])
        duplicate.update(
            {
                "segment_instance": instance + 1,
                "mapping_id": f"{segment}:{instance + 1}",
                "reference_host_ns": 5000000,
                "reference_host_ns_numerator": 5000000,
            }
        )
        payload["segments"].append(duplicate)
    return payload


def make_video(path: Path) -> None:
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        raise AssertionError("ffmpeg is required for bench evidence tests")
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
                "color=c=black:s=96x64:r=200:d=0.25,"
                "drawbox=x=64:y=32:w=24:h=24:color=white:t=fill:enable='eq(n,17)'"
            ),
            "-c:v",
            "ffv1",
            str(path),
        ],
        check=True,
    )


def make_run(run: Path) -> None:
    replay = run / "replay"
    replay.mkdir(parents=True)
    write_json(replay / "clock_alignment.json", clock_alignment(reused=True))
    timeline = [
        {
            "schema_version": 1,
            "kind": "dut_packet_parse",
            "record_id": "fixture-record",
            "raw_clock": "dut_monotonic_us",
            "raw_timestamp": 1000,
            "host_earliest_ns": 999000,
            "host_estimate_ns": 1000000,
            "host_latest_ns": 1001000,
            "source_artifact": "causal_trace_fixture.csv",
            "source_record": 3,
            "causal_identifiers": {
                "clock_segment": "fixture-segment",
                "event_seq": 7,
                "state_revision": 3,
            },
        },
        {
            "schema_version": 1,
            "kind": "display_commit",
            "record_id": "fixture-commit",
            "raw_clock": "dut_monotonic_us",
            "raw_timestamp": 1100,
            "host_earliest_ns": 1099000,
            "host_estimate_ns": 1100000,
            "host_latest_ns": 1101000,
            "source_artifact": "display_commits_fixture.csv",
            "source_record": 2,
            "causal_identifiers": {
                "clock_segment": "fixture-segment",
                "commit_seq": 4,
                "state_revision": 3,
            },
        },
    ]
    (replay / "aligned_timeline.ndjson").write_text(
        "".join(json.dumps(item, sort_keys=True) + "\n" for item in timeline),
        encoding="utf-8",
    )
    (replay / "causal_trace_fixture.csv").write_text(
        "# schema=fixture\n"
        "stage,stage_dut_micros,clock_segment,event_seq,state_revision\n"
        "PACKET_PARSE,1000,fixture-segment,7,3\n",
        encoding="utf-8",
    )
    (replay / "display_commits_fixture.csv").write_text(
        "seq,display_commit_dut_micros,clock_segment,state_revision,left_value,right_value\n"
        "4,1100,fixture-segment,3,3,2\n"
        "5,1200,fixture-segment,4,4,4\n",
        encoding="utf-8",
    )
    (replay / "replay_stimulus.ndjson").write_text(
        json.dumps(
            {
                "schemaVersion": 1,
                "stimulusSequence": 9,
                "replayOffsetSeconds": 0.1,
                "requestedHostMonotonicNs": 1200000,
                "notifications": [{"ordinal": 0, "kind": "fixture", "bytesHex": "0102"}],
                "expected": {"displayOn": True},
            },
            sort_keys=True,
        )
        + "\n",
        encoding="utf-8",
    )
    (replay / "metrics.ndjson").write_text(
        json.dumps(
            {
                "schema_version": 1,
                "record_id": "person@example.invalid",
                "metric": "fixture_count",
                "value": 1,
                "person@example.invalid": "bounded fixture value",
            }
        )
        + "\n",
        encoding="utf-8",
    )
    (replay / "bench_timeline.ndjson").write_text(
        json.dumps(
            {
                "schema_version": 1,
                "state": "notification_accepted",
                "host_monotonic_ns": 1300000,
            }
        )
        + "\n",
        encoding="utf-8",
    )
    (replay / "events.tsv").write_text(
        "state\thost_monotonic_ns\nfixture_event\t1400000\n",
        encoding="utf-8",
    )
    (replay / "perf_fixture.csv").write_text(
        "sample,dutMicros,clockSegment\n"
        "first,1500,fixture-segment\n"
        "# session boundary\n"
        "sample,dutMicros,clockSegment\n"
        "late,1600,fixture-segment\n",
        encoding="utf-8",
    )
    (replay / "run.log").write_text("fixture log line\n", encoding="utf-8")

    second = run / "suite_two"
    second.mkdir()
    write_json(
        second / "clock_alignment.json",
        clock_alignment(slope=2000, segment="second-segment"),
    )
    (second / "causal_trace_fixture.csv").write_text(
        "# schema=fixture\n"
        "stage,stage_dut_micros,clock_segment,event_seq\n"
        "SECOND_STAGE,1000,second-segment,1\n",
        encoding="utf-8",
    )
    (second / "aligned_timeline.ndjson").write_text(
        json.dumps(
            {
                "schema_version": 1,
                "kind": "dut_causal_event",
                "raw_clock": "dut_monotonic_us",
                "raw_timestamp": 1000,
                "host_earliest_ns": 1999000,
                "host_estimate_ns": 2000000,
                "host_latest_ns": 2001000,
                "source_artifact": "causal_trace_fixture.csv",
                "source_record": 3,
                "causal_identifiers": {"clock_segment": "second-segment"},
            }
        )
        + "\n",
        encoding="utf-8",
    )
    (second / "millisecond_events.csv").write_text(
        "stage,stage_dut_millis,clock_segment\n"
        "MILLISECOND_EVENT,1,second-segment\n",
        encoding="utf-8",
    )
    make_video(replay / "camera.mkv")


def run_cli(*arguments: str) -> dict[str, object]:
    process = subprocess.run(
        [sys.executable, str(ROOT / "tools" / "bench_evidence.py"), *arguments],
        cwd=ROOT,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    assert_true(process.returncode == 0, f"evidence CLI failed: {process.stderr}")
    assert_true(
        len(process.stdout.encode("utf-8")) <= evidence.MAX_STDOUT_BYTES,
        "evidence CLI exceeded its stdout bound",
    )
    return json.loads(process.stdout)


def run_cli_failure(*arguments: str) -> subprocess.CompletedProcess[str]:
    process = subprocess.run(
        [sys.executable, str(ROOT / "tools" / "bench_evidence.py"), *arguments],
        cwd=ROOT,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    assert_true(process.returncode == 2, f"evidence CLI unexpectedly succeeded: {process.stdout}")
    assert_true("Traceback" not in process.stderr, "evidence CLI emitted a traceback")
    assert_true(
        all(value not in process.stderr for value in arguments if value.startswith("/")),
        "evidence CLI leaked an absolute path",
    )
    assert_true(len(process.stderr.encode("utf-8")) < 512, "evidence CLI error was unbounded")
    return process


def snapshot_tree(root: Path) -> dict[str, tuple[int, str]]:
    return {
        path.relative_to(root).as_posix(): (path.stat().st_mtime_ns, evidence.sha256_file(path))
        for path in root.rglob("*")
        if path.is_file() and not path.is_symlink()
    }


def assert_selectors_resolve(run: Path, payload: dict[str, object]) -> None:
    for result in payload.get("results", []):
        selector = result["selector"]
        assert_true(
            investigator.resolve_artifact_selector(run, selector) is None,
            f"query selector does not resolve: {selector}",
        )


def test_synthetic_indexes_queries_and_native_sheets() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        run = root / "run"
        make_run(run)
        manifest = evidence.build_run_index(run)
        assert_true(manifest["videos"][0]["status"] == "complete", "video index failed")
        video_summary = manifest["videos"][0]["summary"]
        frames_path = run / evidence.INDEX_DIRECTORY / video_summary["frame_index_filename"]
        rows = [json.loads(line) for line in frames_path.read_text(encoding="utf-8").splitlines()]
        assert_true(
            video_summary["frame_count"] == len(rows) == 50,
            f"not every native frame was indexed: {video_summary}",
        )
        assert_true(
            [row["frame_index"] for row in rows] == list(range(50)),
            "frame index has an interior ordinal gap",
        )
        assert_true(
            any(window["frame_index"] in {17, 18} for window in video_summary["top_change_windows"]),
            "single-native-frame change was not indexed",
        )

        inspect_video = investigator.load_video_helper()
        output = root / "sheets"
        report = inspect_video(
            run / "replay" / "camera.mkv",
            output,
            [{"start_seconds": 0.07, "end_seconds": 0.14, "sample_rate_hz": 500.0}],
            index_dir=run / evidence.INDEX_DIRECTORY,
            video_sha256=evidence.sha256_file(run / "replay" / "camera.mkv"),
            scan_overview=False,
        )
        interval = report["requested_intervals"][0]
        assert_true(
            interval["status"] == "complete"
            and 2 <= interval["frame_count"] <= 36
            and interval["sample_rate_hz"] == 200.0,
            f"native-rate interval extraction failed: {interval}",
        )
        assert_true(
            all(
                cell["source_pts_measured"] is True
                and cell["pts_uncertainty_seconds"] == 0
                and isinstance(cell["source_pts_value"], int)
                and cell["source_pts_time_base"]["denominator"] > 0
                and isinstance(cell["source_frame_index"], int)
                for cell in interval["cells"]
            ),
            "native sheet cells lost measured source positions",
        )
        video_hash = evidence.sha256_file(run / "replay" / "camera.mkv")
        attachments = investigator.persist_attachment_manifest(
            run,
            [
                {
                    "file": output / interval["filename"],
                    "manifest": {
                        "pass": 2,
                        "source_video_path": "replay/camera.mkv",
                        "source_video_sha256": video_hash,
                        "purpose": interval["purpose"],
                        "interval": interval["interval"],
                        "layout": interval["layout"],
                        "cells": interval["cells"],
                    },
                }
            ],
        )
        cell_indices = [cell["cell_index"] for cell in interval["cells"]]
        frame_indices = [cell["source_frame_index"] for cell in interval["cells"]]
        pts_values = [cell["source_pts_seconds"] for cell in interval["cells"]]
        selector = {
            "kind": "video",
            "path": "replay/camera.mkv",
            "sha256": video_hash,
            "description": "Measured native-rate fixture frames",
            "start_pts_s": min(pts_values),
            "end_pts_s": max(pts_values),
            "start_frame": min(frame_indices),
            "end_frame": max(frame_indices),
            "attachment_index": 1,
            "cell_indices": cell_indices,
        }
        assert_true(
            investigator.resolve_artifact_selector(
                run, selector, {}, {1: attachments[0]}
            )
            is None,
            "native-rate sheet selector does not resolve",
        )

        before_queries = snapshot_tree(run)
        catalog = run_cli("list", str(run), "--kind", "dut_packet_parse")
        assert_true(
            "dut_packet_parse" in catalog["field_dictionary"],
            "per-kind field dictionary is unavailable",
        )
        privacy_catalog = run_cli("list", str(run))
        privacy_record = run_cli(
            "records", str(run), "--path", "replay/metrics.ndjson"
        )
        assert_true(
            "person@example.invalid"
            not in json.dumps({"catalog": privacy_catalog, "record": privacy_record}),
            "a dynamic private-shaped field key escaped the catalog sanitizer",
        )
        private_predicate = run_cli(
            "records",
            str(run),
            "--path",
            "replay/metrics.ndjson",
            "--where",
            "record_id=person@example.invalid",
        )
        assert_true(
            private_predicate["returned_count"] == 1
            and private_predicate["query"]["predicate_count"] == 1
            and private_predicate["query"]["predicate_fields"] == ["record_id"]
            and "person@example.invalid" not in json.dumps(private_predicate),
            "a private predicate value escaped in successful query output",
        )
        assert_selectors_resolve(run, private_predicate)
        malformed_private_predicate = run_cli_failure(
            "records",
            str(run),
            "--where",
            "person@example.invalid",
        )
        assert_true(
            "person@example.invalid" not in malformed_private_predicate.stderr,
            "a malformed private predicate value escaped in query errors",
        )
        queried_paths = {
            "replay/aligned_timeline.ndjson": "record_id=fixture-record",
            "replay/causal_trace_fixture.csv": "event_seq=7",
            "replay/display_commits_fixture.csv": "seq=4",
            "replay/replay_stimulus.ndjson": "stimulusSequence=9",
            "replay/metrics.ndjson": "metric=fixture_count",
            "replay/run.log": None,
            "replay/bench_timeline.ndjson": "state=notification_accepted",
            "replay/events.tsv": "state=fixture_event",
            "replay/perf_fixture.csv": "sample=first",
        }
        for path, expected_key in queried_paths.items():
            payload = run_cli("records", str(run), "--path", path, "--limit", "1")
            assert_true(payload["returned_count"] == 1, f"query returned no {path} record")
            assert_selectors_resolve(run, payload)
            selector = payload["results"][0]["selector"]
            assert_true(
                selector.get("keys") == ([expected_key] if expected_key else None),
                f"query did not emit the expected copy-ready key for {path}: {selector}",
            )
        aligned = run_cli(
            "records",
            str(run),
            "--path",
            "replay/aligned_timeline.ndjson",
            "--limit",
            "2",
        )
        assert_selectors_resolve(run, aligned)
        first_selector = dict(aligned["results"][0]["selector"])
        second_selector = aligned["results"][1]["selector"]
        combined = dict(first_selector)
        combined["line_end"] = second_selector["line_end"]
        combined["keys"] = [
            *first_selector["keys"],
            *second_selector["keys"],
        ]
        assert_true(
            "NDJSON keys do not resolve"
            in str(investigator.resolve_artifact_selector(run, combined)),
            "keys from different query results unexpectedly resolved as one selector",
        )
        comparison = run_cli(
            "records",
            str(run),
            "--path",
            "replay/display_commits_fixture.csv",
            "--compare",
            "left_value!=right_value",
            "--context",
            "1",
        )
        assert_true(
            comparison["returned_match_count"] == 1
            and comparison["returned_count"] == 2
            and comparison["results"][0]["record"]["seq"] == "4"
            and comparison["results"][0]["selection_role"] == "match"
            and comparison["results"][1]["record"]["seq"] == "5"
            and comparison["results"][1]["selection_role"] == "context"
            and comparison["query"]["field_comparisons"]
            == ["left_value!=right_value"]
            and comparison["query"]["adjacent_context_records"] == 1,
            f"generic same-record comparison did not isolate a mismatch: {comparison}",
        )
        assert_selectors_resolve(run, comparison)
        missing_context_path = run_cli_failure(
            "records", str(run), "--context", "1"
        )
        assert_true(
            "requires at least one --path" in missing_context_path.stderr,
            "adjacent context without an artifact path was not rejected",
        )
        notification = run_cli(
            "records",
            str(run),
            "--path",
            "replay/bench_timeline.ndjson",
            "--kind",
            "notification_accepted",
        )
        assert_true(
            notification["returned_count"] == 1,
            "raw host notification state was not queryable by kind",
        )
        assert_selectors_resolve(run, notification)
        late_csv = run_cli(
            "records",
            str(run),
            "--path",
            "replay/perf_fixture.csv",
            "--where",
            "sample=late",
        )
        assert_true(
            late_csv["returned_count"] == 1
            and late_csv["results"][0]["selector"]["row_start"] == 3
            and late_csv["results"][0]["physical_line_start"] == 5,
            f"late repeated-header CSV coordinates drifted: {late_csv}",
        )
        assert_selectors_resolve(run, late_csv)
        trace = run_cli(
            "records",
            str(run),
            "--path",
            "replay/causal_trace_fixture.csv",
            "--where",
            "stage=PACKET_PARSE",
            "--clock",
            "dut_us",
            "--clock-segment",
            "fixture-segment",
            "--segment-instance",
            "1",
            "--start",
            "1000",
            "--end",
            "1000",
        )
        assert_selectors_resolve(run, trace)
        conversion = trace["query"]["clock_conversion"]
        assert_true(
            conversion["method"] == "clock_alignment_affine"
            and conversion["uncertainty_ns"] > 0
            and conversion["evidence"],
            f"clock conversion lost uncertainty or mapping evidence: {conversion}",
        )
        assert_true(
            trace["results"][0]["host_time"]["estimate_ns"] == 1000000,
            "duplicate trace basename inherited another suite's host interval",
        )
        assert_true(
            investigator.resolve_artifact_selector(run, conversion["evidence"][0]) is None,
            "clock mapping selector does not resolve",
        )
        ambiguous = run_cli_failure(
            "records",
            str(run),
            "--path",
            "replay/causal_trace_fixture.csv",
            "--clock",
            "dut_us",
            "--clock-segment",
            "fixture-segment",
            "--start",
            "1000",
            "--end",
            "1000",
        )
        assert_true(
            "ambiguous" in ambiguous.stderr,
            f"reused DUT segment instance was guessed: {ambiguous.stderr}",
        )
        second_trace = run_cli(
            "records",
            str(run),
            "--path",
            "suite_two/causal_trace_fixture.csv",
            "--clock",
            "dut_us",
            "--clock-segment",
            "second-segment",
            "--start",
            "1000",
            "--end",
            "1000",
        )
        second_conversion = second_trace["query"]["clock_conversion"]
        assert_true(
            second_conversion["host_estimate_start_ns"] == 2000000
            and second_conversion["mapping_id"] == "second-segment:1",
            f"suite-local alignment was not selected: {second_conversion}",
        )
        assert_true(
            second_trace["results"][0]["host_time"]["estimate_ns"] == 2000000,
            "suite-local timeline join collided on a duplicate basename",
        )
        assert_selectors_resolve(run, second_trace)
        millisecond_record = run_cli(
            "records",
            str(run),
            "--path",
            "suite_two/millisecond_events.csv",
        )["results"][0]
        millisecond_host = millisecond_record["host_time"]
        assert_true(
            millisecond_host["latest_ns"] - millisecond_host["earliest_ns"]
            > 1_000_000
            and "source timestamp has millisecond precision"
            in millisecond_host["limitations"],
            f"millisecond timestamp was treated as a point: {millisecond_host}",
        )
        replay_clock = run_cli(
            "records",
            str(run),
            "--path",
            "replay/replay_stimulus.ndjson",
            "--clock",
            "replay_offset_s",
            "--start",
            "0.1",
            "--end",
            "0.1",
        )
        replay_conversion = replay_clock["query"]["clock_conversion"]
        assert_true(
            replay_conversion["method"] == "recorded_piecewise_linear"
            and replay_conversion["evidence"],
            f"recorded clock mapping lacks raw anchors: {replay_conversion}",
        )
        for selector in replay_conversion["evidence"]:
            assert_true(
                investigator.resolve_artifact_selector(run, selector) is None,
                f"recorded clock anchor does not resolve: {selector}",
            )
        frame_query = run_cli(
            "frames",
            str(run),
            "--path",
            "replay/camera.mkv",
            "--start",
            "0.08",
            "--end",
            "0.10",
        )
        assert_true(
            frame_query["finding_aid_only"] is True
            and frame_query["source_video_sha256"] == video_hash
            and frame_query["results"],
            "frame query lost its raw-video identity",
        )
        revision = subprocess.run(
            ["git", "rev-parse", "HEAD"],
            cwd=ROOT,
            check=True,
            text=True,
            capture_output=True,
        ).stdout.strip()
        source = run_cli(
            "source",
            "--revision",
            "HEAD",
            "--path",
            "AGENTS.md",
            "--line-start",
            "1",
            "--line-end",
            "2",
        )
        assert_true(
            source["code_selector_basis"]
            == {
                "revision": revision,
                "path": "AGENTS.md",
                "line_start": 1,
                "line_end": 2,
                "selection_sha256": source["selection_sha256"],
            },
            "source query did not return a copy-ready exact selector basis",
        )
        code_selector = {
            **source["code_selector_basis"],
            "symbol": "fixture_instructions",
            "description": "Exact-revision source fixture",
        }
        assert_true(
            investigator.resolve_code_selector(code_selector) is None,
            "source query did not produce a resolvable code selector",
        )
        assert_true(snapshot_tree(run) == before_queries, "a query subcommand wrote to the run")

        index_file = run / evidence.INDEX_DIRECTORY / evidence.MANIFEST_FILENAME
        index_selector = {
            "kind": "file",
            "path": index_file.relative_to(run).as_posix(),
            "sha256": evidence.sha256_file(index_file),
            "description": "Derived index must not be evidence",
        }
        error = investigator.resolve_artifact_selector(run, index_selector)
        assert_true(
            error is not None and "finding aid" in error,
            "derived evidence index became citable primary evidence",
        )

        timeline_path = run / "replay" / "aligned_timeline.ndjson"
        timeline_content = timeline_path.read_text(encoding="utf-8")
        timeline_path.write_text(timeline_content + "{}\n", encoding="utf-8")
        stale_dependency = run_cli_failure(
            "records",
            str(run),
            "--path",
            "replay/causal_trace_fixture.csv",
        )
        assert_true(
            "indexed artifact changed" in stale_dependency.stderr,
            f"stale timeline dependency was accepted: {stale_dependency.stderr}",
        )
        timeline_path.write_text(timeline_content, encoding="utf-8")

        log_path = run / "replay" / "run.log"
        log_content = log_path.read_text(encoding="utf-8")
        log_path.write_text(
            "fixture log line\nchanged\n", encoding="utf-8"
        )
        stale_log = run_cli_failure(
            "records", str(run), "--path", "replay/run.log"
        )
        assert_true(
            "indexed artifact changed" in stale_log.stderr,
            f"stale raw input was not rejected: {stale_log.stderr}",
        )
        log_path.write_text(log_content, encoding="utf-8")

        alignment_path = run / "replay" / "clock_alignment.json"
        alignment_path.write_text("{}\n", encoding="utf-8")
        stale = run_cli_failure(
            "records",
            str(run),
            "--path",
            "replay/causal_trace_fixture.csv",
            "--clock",
            "dut_us",
            "--clock-segment",
            "fixture-segment",
            "--start",
            "1000",
            "--end",
            "1000",
        )
        assert_true(
            "clock alignment changed" in stale.stderr,
            f"stale alignment was not identified safely: {stale.stderr}",
        )

        frames_path.write_text('{"unexpected":true}\n', encoding="utf-8")
        run_cli_failure(
            "frames",
            str(run),
            "--path",
            "replay/camera.mkv",
        )

    with tempfile.TemporaryDirectory() as temporary:
        broken = Path(temporary) / "broken"
        broken.mkdir()
        (broken / "camera.mkv").write_bytes(b"not a video")
        build = run_cli("build", str(broken))
        assert_true(
            build["status"] == "partial"
            and build["summary"]["videos"][0]["status"] == "failed",
            f"failed exhaustive video index was reported complete: {build}",
        )


def test_real_run_when_requested(run_value: str | None) -> None:
    if not run_value:
        return
    run = Path(run_value).resolve()
    manifest = evidence.build_run_index(run)
    assert_true(
        manifest["videos"] and all(item["status"] == "complete" for item in manifest["videos"]),
        "real-run exhaustive video indexing failed",
    )
    ffprobe = shutil.which("ffprobe")
    if not ffprobe:
        raise AssertionError("ffprobe is required for real-run verification")
    for item in manifest["videos"]:
        process = subprocess.run(
            [
                ffprobe,
                "-v",
                "error",
                "-select_streams",
                "v:0",
                "-count_frames",
                "-show_entries",
                "stream=nb_read_frames",
                "-of",
                "default=nw=1:nk=1",
                str(run / item["path"]),
            ],
            check=True,
            text=True,
            capture_output=True,
        )
        assert_true(
            int(process.stdout.strip()) == item["summary"]["frame_count"],
            "real-run frame index does not cover every decoded frame",
        )
    indexed_paths = [item["path"] for item in manifest["files"]]
    required_patterns = (
        "aligned_timeline.ndjson",
        "causal_trace_",
        "display_commits_",
        "replay_stimulus.ndjson",
        "metrics.ndjson",
        "run.log",
    )
    for pattern in required_patterns:
        path = next((value for value in indexed_paths if pattern in value), None)
        assert_true(path is not None, f"real-run index omitted {pattern}")
        args = argparse.Namespace(
            run_dir=str(run),
            where=[],
            compare=[],
            context=0,
            path=[path],
            kind=[],
            start=None,
            end=None,
            clock="host_monotonic_ns",
            clock_segment=None,
            segment_instance=None,
            offset=0,
            limit=1,
        )
        payload = evidence.query_records(args)
        assert_true(payload["returned_count"] == 1, f"real-run query returned no {pattern}")
        assert_selectors_resolve(run, payload)

    causal_path = next(value for value in indexed_paths if "causal_trace_" in value)
    causal_record = next(
        record
        for _row, _physical_start, _physical_end, record in evidence._csv_records(
            run / causal_path
        )
        if record.get("stage_dut_micros") not in (None, "", "0")
        and record.get("clock_segment") not in (None, "")
    )
    dut_time = float(causal_record["stage_dut_micros"])
    converted = evidence.convert_window(
        run,
        run / evidence.INDEX_DIRECTORY,
        manifest,
        clock="dut_monotonic_us",
        start=dut_time,
        end=dut_time,
        clock_segment=str(causal_record["clock_segment"]),
    )
    assert_true(
        converted["status"] == "mapped"
        and converted["uncertainty_ns"] > 0
        and converted["evidence"],
        "real-run DUT conversion lost uncertainty or evidence",
    )
    assert_true(
        investigator.resolve_artifact_selector(run, converted["evidence"][0]) is None,
        "real-run clock selector does not resolve",
    )
    identity_path = next(iter(sorted(run.rglob("identity.json"))), None)
    assert_true(identity_path is not None, "real run has no recorded identity")
    identity = json.loads(identity_path.read_text(encoding="utf-8"))
    revision = identity.get("traceability", {}).get("repository_sha")
    assert_true(
        isinstance(revision, str) and bool(revision),
        "real run identity has no recorded repository revision",
    )
    candidates = sorted(investigator.recorded_files(identity))
    source_path = next(
        (
            path
            for path in candidates
            if subprocess.run(
                ["git", "cat-file", "-e", f"{revision}:{path}"],
                cwd=ROOT,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                check=False,
            ).returncode
            == 0
        ),
        None,
    )
    assert_true(source_path is not None, "recorded source paths do not resolve at revision")
    args = argparse.Namespace(
        revision=revision,
        path=source_path,
        line_start=1,
        line_end=1,
    )
    result = evidence.source_lines(args)
    exact_lines = subprocess.run(
        ["git", "show", f"{revision}:{source_path}"],
        cwd=ROOT,
        check=True,
        text=True,
        capture_output=True,
    ).stdout.splitlines()
    assert_true(exact_lines, "recorded source file is unexpectedly empty")
    assert_true(
        result["lines"][0]["text"] == exact_lines[0],
        "recorded-revision source retrieval changed exact content",
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--real-run", default=os.environ.get("BENCH_EVIDENCE_REAL_RUN"))
    args = parser.parse_args()
    test_synthetic_indexes_queries_and_native_sheets()
    test_real_run_when_requested(args.real_run)
    print("bench evidence tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

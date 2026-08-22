#!/usr/bin/env python3
"""Focused tests for the model-led bench investigator runner."""

from __future__ import annotations

import hashlib
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import bench_investigate as investigator  # noqa: E402


def assert_true(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def sha256(content: bytes) -> str:
    return hashlib.sha256(content).hexdigest()


def assert_codex_transport_schema(schema: object) -> None:
    def check(node: object, path: str = "$") -> None:
        if isinstance(node, dict):
            assert_true("oneOf" not in node, f"unsupported oneOf remained at {path}")
            assert_true("const" not in node, f"unsupported const remained at {path}")
            if "enum" in node:
                assert_true("type" in node, f"enum lacks an explicit type at {path}")
            assert_true(
                not isinstance(node.get("type"), list),
                f"multi-type union was not lowered to anyOf at {path}",
            )
            properties = node.get("properties")
            if isinstance(properties, dict):
                assert_true(
                    set(node.get("required", [])) == set(properties),
                    f"Structured Outputs required fields are incomplete at {path}",
                )
                assert_true(
                    node.get("additionalProperties") is False,
                    f"Structured Outputs object is open at {path}",
                )
            for key, child in node.items():
                check(child, f"{path}.{key}")
        elif isinstance(node, list):
            for index, child in enumerate(node):
                check(child, f"{path}[{index}]")

    check(schema)


def source_precheck(basis: str = "exact") -> dict[str, object]:
    return {
        "current_head": "fixture-revision",
        "identities": [{"suggested_basis": basis}],
    }


def minimal_model_report(evidence: dict[str, object]) -> dict[str, object]:
    code_path = ROOT / "tools" / "bench_investigate.py"
    code_content = code_path.read_text(encoding="utf-8")
    code_lines = code_content.splitlines()
    line_start = next(
        index for index, line in enumerate(code_lines, 1) if line.startswith("def sha256_file(")
    )
    line_end = line_start + 5
    selection_hash = investigator.code_selection_sha256(
        code_content, line_start, line_end
    )
    assert selection_hash is not None
    return {
        "schema_version": 2,
        "kind": "bench_investigation",
        "generated_at_utc": "2026-08-20T00:00:00Z",
        "execution_status": {"state": "completed", "summary": "model", "errors": []},
        "source": {
            "basis": "exact",
            "summary": "fixture source",
            "recorded_revisions": [],
            "inspected_revision": "WORKTREE",
            "identity_evidence": [],
            "mismatches": [],
            "binary_identities": [],
            "limitations": [],
        },
        "coverage": {
            "artifacts": [],
            "code": [],
            "video_intervals": [],
            "clock_mappings": [],
            "notes": [],
        },
        "findings": [
            {
                "id": "fixture-correlation",
                "title": "Fixture correlation",
                "causal_status": "confirmed",
                "impact": "Fixture impact",
                "expected_behavior": "Expected record",
                "observed_behavior": "Observed record",
                "cause": "Fixture cause",
                "fix": "Fixture fix",
                "evidence": [evidence],
                "counterevidence": [],
                "code": [
                    {
                        "revision": "WORKTREE",
                        "path": "tools/bench_investigate.py",
                        "symbol": "sha256_file",
                        "line_start": line_start,
                        "line_end": line_end,
                        "selection_sha256": selection_hash,
                        "description": "Tight fixture code selector",
                    }
                ],
                "clock_mapping_ids": [],
                "remaining_unknowns": [],
            }
        ],
        "unresolved": [],
        "model": {
            "backend": "placeholder",
            "name": "placeholder",
            "tool_version": "placeholder",
            "prompt_sha256": "0" * 64,
            "instruction_hashes": [],
        },
        "video_requests": [],
    }


def test_discovery_accepts_unfamiliar_and_excludes_prior_output() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        run = Path(temporary)
        (run / "suite").mkdir()
        (run / "suite" / "future.trace").write_bytes(b"trace")
        (run / "investigation.json").write_text("old", encoding="utf-8")
        artifacts = investigator.discover_artifacts(run)
        paths = [item["path"] for item in artifacts]
        assert_true(paths == ["suite/future.trace"], f"unexpected discovery: {artifacts}")
        assert_true(artifacts[0]["kind"] == "binary", "unfamiliar input was reclassified")
        assert_true(artifacts[0]["sha256"] == sha256(b"trace"), "runner omitted artifact hash")
        assert_true(artifacts[0]["status"] == "readable", "readable artifact looked unreadable")


def test_stat_or_hash_failure_remains_visible_as_unreadable_coverage() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        run = Path(temporary)
        safe_content = b"safe\n"
        unreadable_content = b"cannot hash\n"
        stat_failure_content = b"cannot stat\n"
        (run / "safe.log").write_bytes(safe_content)
        (run / "unreadable.log").write_bytes(unreadable_content)
        (run / "stat-failure.log").write_bytes(stat_failure_content)
        real_sha256_file = investigator.sha256_file
        real_stat = Path.stat

        def fail_one_hash(path: Path) -> str:
            if path.name == "unreadable.log":
                raise OSError("fixture hash failure")
            return real_sha256_file(path)

        def fail_one_stat(path: Path, *args: object, **kwargs: object) -> os.stat_result:
            if path.name == "stat-failure.log":
                raise OSError("fixture stat failure")
            return real_stat(path, *args, **kwargs)

        safe_selector: dict[str, object] = {
            "kind": "log",
            "path": "safe.log",
            "sha256": sha256(safe_content),
            "description": "Safe line",
            "line_start": 1,
            "line_end": 1,
        }
        model_report = minimal_model_report(safe_selector)
        model_report["findings"] = []
        with (
            patch.object(Path, "stat", new=fail_one_stat),
            patch.object(investigator, "sha256_file", side_effect=fail_one_hash),
        ):
            inventory = investigator.discover_artifacts(run)
            finished = investigator.finish_report(
                model_report,
                run_dir=run,
                source_context_value=source_precheck(),
                model="fixture-model",
                backend="fixture-backend",
                tool_version="fixture-tool",
                prompt="fixture prompt",
                extra_errors=[],
            )

        by_path = {item["path"]: item for item in inventory}
        assert_true(
            by_path["unreadable.log"] == {
                "path": "unreadable.log",
                "sha256": None,
                "size_bytes": len(unreadable_content),
                "kind": "text",
                "status": "unreadable",
            },
            f"hash failure vanished from runner inventory: {inventory}",
        )
        assert_true(
            by_path["stat-failure.log"] == {
                "path": "stat-failure.log",
                "sha256": None,
                "size_bytes": None,
                "kind": "text",
                "status": "unreadable",
            },
            f"stat failure vanished from runner inventory: {inventory}",
        )
        coverage = {
            item["path"]: item for item in finished["coverage"]["artifacts"]
        }
        assert_true(
            coverage["unreadable.log"]["status"] == "unreadable"
            and coverage["unreadable.log"]["sha256"] is None
            and coverage["unreadable.log"]["size_bytes"] == len(unreadable_content),
            f"hash failure did not survive as unreadable coverage: {coverage}",
        )
        assert_true(
            coverage["stat-failure.log"]["status"] == "unreadable"
            and coverage["stat-failure.log"]["sha256"] is None
            and coverage["stat-failure.log"]["size_bytes"] is None,
            f"stat failure did not survive as unreadable coverage: {coverage}",
        )
        assert_true(
            investigator.validate_report_schema(finished) == [],
            "unreadable runner coverage violated the report contract",
        )


def test_invalid_primary_citations_are_stripped_or_omitted() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        run = Path(temporary)
        content = b'{"event":"observed"}\n'
        (run / "events.ndjson").write_bytes(content)
        selector: dict[str, object] = {
            "kind": "ndjson",
            "path": "events.ndjson",
            "sha256": "0" * 64,
            "description": "Observed fixture event",
            "line_start": 1,
            "line_end": 1,
            "keys": ["event=observed"],
        }
        model_report = minimal_model_report(selector)
        valid_selector = dict(selector)
        valid_selector["sha256"] = sha256(content)
        unaffected = minimal_model_report(valid_selector)["findings"][0]
        unaffected["id"] = "unaffected"
        code_unresolved = minimal_model_report(valid_selector)["findings"][0]
        code_unresolved["id"] = "code-unresolved"
        code_unresolved["code"][0]["path"] = "missing-owner.cpp"
        model_report["findings"].extend((unaffected, code_unresolved))
        report = investigator.finish_report(
            model_report,
            run_dir=run,
            source_context_value=source_precheck(),
            model="fixture-model",
            backend="fixture-backend",
            tool_version="fixture-tool",
            prompt="fixture prompt",
            extra_errors=[],
        )
        assert_true(report["execution_status"]["state"] == "partial", "bad hash looked complete")
        assert_true(
            [item["id"] for item in report["findings"]] == ["unaffected"],
            f"bad primary citation survived as an actionable finding: {report['findings']}",
        )
        assert_true(
            report["findings"][0]["causal_status"] == "confirmed",
            "unaffected finding was downgraded",
        )
        assert_true(
            [item["id"] for item in report["unresolved"]]
            == ["code-unresolved"],
            "zero-grounding lead survived or grounded observation was discarded",
        )
        assert_true(selector["sha256"] == "0" * 64, "runner rewrote the model's bad hash")
        assert_true(
            "Primary citation resolution failed" in report["unresolved"][0]["why_unknown"],
            "citation limitations were hidden",
        )
        assert_true(
            report["unresolved"][0]["code"] == []
            and report["unresolved"][0]["hypotheses"][0]["code"] == [],
            "invalid code selector survived in unresolved output",
        )
        errors = "\n".join(report["execution_status"]["errors"])
        assert_true(
            "unresolved_omitted_zero_grounding: fixture-correlation" in errors,
            "zero-grounding omission was hidden",
        )
        assert_true("finding_moved_unresolved: code-unresolved" in errors, "unknown conversion was hidden")
        assert_true(
            investigator.validate_report_schema(report) == [],
            "runner mutation produced a schema-invalid report",
        )
        invalid = dict(report)
        invalid["verdict"] = "PASS"
        assert_true(
            any("unexpected property verdict" in error for error in investigator.validate_report_schema(invalid)),
            "full schema validation accepted an added verdict",
        )


def test_existing_unresolved_without_primary_grounding_is_omitted() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        run = Path(temporary)
        content = b'{"event":"observed"}\n'
        (run / "events.ndjson").write_bytes(content)
        bad_evidence: dict[str, object] = {
            "kind": "ndjson",
            "path": "events.ndjson",
            "sha256": "0" * 64,
            "description": "Bad primary citation",
            "line_start": 1,
            "line_end": 1,
        }
        report = minimal_model_report(bad_evidence)
        finding = report["findings"].pop()
        report["unresolved"] = [
            {
                "id": "ungrounded-existing",
                "title": "Ungrounded existing lead",
                "causal_status": "unknown",
                "observation": "A model-reported observation",
                "why_unknown": "The evidence was not checked",
                "evidence": [bad_evidence],
                "counterevidence": [],
                "code": finding["code"],
                "clock_mapping_ids": [],
                "hypotheses": [
                    {
                        "rank": 1,
                        "description": "Fixture hypothesis",
                        "evidence": [bad_evidence],
                        "code": finding["code"],
                    }
                ],
                "next_observation": {
                    "description": "Read one exact event",
                    "distinguishes": ["present", "absent"],
                    "minimal_evidence": "One valid event selector",
                },
            }
        ]
        finished = investigator.finish_report(
            report,
            run_dir=run,
            source_context_value=source_precheck(),
            model="fixture-model",
            backend="fixture-backend",
            tool_version="fixture-tool",
            prompt="fixture prompt",
            extra_errors=[],
        )
        assert_true(finished["unresolved"] == [], "zero-grounding unresolved lead survived")
        assert_true(
            any(
                "unresolved_omitted_zero_grounding: ungrounded-existing" in error
                for error in finished["execution_status"]["errors"]
            ),
            "zero-grounding omission was not machine-visible",
        )


def test_source_precheck_limits_attribution_and_model_execution_state_is_preserved() -> None:
    assert_true(
        investigator.conservative_source_basis(
            {
                "current_head": "fixture-revision",
                "identities": [
                    {"suggested_basis": "exact"},
                    {"path": "broken/identity.json", "error": "JSONDecodeError"},
                ],
            }
        )
        == "current_only",
        "an unreadable identity was ignored when selecting exact source",
    )
    with tempfile.TemporaryDirectory() as temporary:
        run = Path(temporary)
        content = b'{"event":"observed"}\n'
        (run / "events.ndjson").write_bytes(content)
        evidence: dict[str, object] = {
            "kind": "ndjson",
            "path": "events.ndjson",
            "sha256": sha256(content),
            "description": "Observed fixture event",
            "line_start": 1,
            "line_end": 1,
            "keys": ["event=observed"],
        }

        source_limited = investigator.finish_report(
            minimal_model_report(evidence),
            run_dir=run,
            source_context_value=source_precheck("current_only"),
            model="fixture-model",
            backend="fixture-backend",
            tool_version="fixture-tool",
            prompt="fixture prompt",
            extra_errors=[],
        )
        assert_true(source_limited["source"]["basis"] == "current_only", "source overclaimed exact")
        assert_true(
            source_limited["findings"][0]["causal_status"] == "probable",
            "confirmed cause survived current-only source",
        )
        assert_true(
            source_limited["execution_status"]["state"] == "partial",
            "source limitation looked complete",
        )

        for state in ("partial", "failed"):
            model_report = minimal_model_report(evidence)
            model_report["findings"] = []
            model_report["execution_status"] = {
                "state": state,
                "summary": f"model reported {state}",
                "errors": [f"model_{state}"],
            }
            finished = investigator.finish_report(
                model_report,
                run_dir=run,
                source_context_value=source_precheck(),
                model="fixture-model",
                backend="fixture-backend",
                tool_version="fixture-tool",
                prompt="fixture prompt",
                extra_errors=[("runner_note", "runner limitation")],
            )
            assert_true(finished["execution_status"]["state"] == state, f"lost {state} state")
            errors = finished["execution_status"]["errors"]
            assert_true(f"model_{state}" in errors, f"lost model {state} error")
            assert_true(
                "runner_note: runner limitation" in errors,
                f"lost runner error for {state}",
            )

        inconsistent = minimal_model_report(evidence)
        inconsistent["findings"] = []
        inconsistent["execution_status"] = {
            "state": "completed",
            "summary": "completed with an error",
            "errors": ["model_error"],
        }
        finished = investigator.finish_report(
            inconsistent,
            run_dir=run,
            source_context_value=source_precheck(),
            model="fixture-model",
            backend="fixture-backend",
            tool_version="fixture-tool",
            prompt="fixture prompt",
            extra_errors=[],
        )
        assert_true(
            finished["execution_status"]["state"] == "partial",
            "completed state retained nonempty model errors",
        )


def test_code_selector_requires_exact_selected_line_hash() -> None:
    evidence = {
        "kind": "file",
        "path": "unused",
        "sha256": "0" * 64,
        "description": "unused fixture",
    }
    report = minimal_model_report(evidence)
    selector = report["findings"][0]["code"][0]
    assert_true(investigator.resolve_code_selector(selector) is None, "valid code slice failed")
    wrong_hash = dict(selector)
    wrong_hash["selection_sha256"] = "0" * 64
    assert_true(
        "selection hash does not match" in str(investigator.resolve_code_selector(wrong_hash)),
        "wrong selected-line digest resolved",
    )
    missing_hash_report = minimal_model_report(evidence)
    del missing_hash_report["findings"][0]["code"][0]["selection_sha256"]
    assert_true(
        any(
            "missing required property selection_sha256" in error
            for error in investigator.validate_report_schema(missing_hash_report)
        ),
        "schema accepted a code location without selected-line identity",
    )
    legacy_report = minimal_model_report(evidence)
    legacy_report["schema_version"] = 1
    assert_true(
        any(
            "schema_version" in error
            for error in investigator.validate_report_schema(legacy_report)
        ),
        "strengthened version-2 contract still accepted a version-1 report",
    )


def test_invalid_reviewed_coverage_becomes_runner_owned_skipped() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        run = Path(temporary)
        content = b"fixture evidence\n"
        (run / "evidence.log").write_bytes(content)
        selector: dict[str, object] = {
            "kind": "log",
            "path": "evidence.log",
            "sha256": sha256(content),
            "description": "One reviewed line",
            "line_start": 1,
            "line_end": 1,
        }
        report = minimal_model_report(selector)
        report["findings"] = []
        report["coverage"]["artifacts"] = [
            {
                "path": "evidence.log",
                "status": "reviewed",
                "sha256": "0" * 64,
                "size_bytes": len(content),
                "role": "fixture",
                "selectors": [selector],
                "notes": [],
            },
            {
                "path": "evidence.log",
                "status": "partially_reviewed",
                "sha256": None,
                "size_bytes": len(content),
                "role": "fixture",
                "selectors": [selector],
                "notes": [],
            },
            {
                "path": "evidence.log",
                "status": "reviewed",
                "sha256": sha256(content),
                "size_bytes": len(content),
                "role": "fixture",
                "notes": [],
            },
            {
                "path": "evidence.log",
                "status": "reviewed",
                "sha256": sha256(content),
                "size_bytes": len(content),
                "role": "fixture",
                "selectors": [selector],
                "notes": [],
            },
        ]
        finished = investigator.finish_report(
            report,
            run_dir=run,
            source_context_value=source_precheck(),
            model="fixture-model",
            backend="fixture-backend",
            tool_version="fixture-tool",
            prompt="fixture prompt",
            extra_errors=[],
        )
        coverage = finished["coverage"]["artifacts"]
        assert_true(
            len(coverage) == 1
            and coverage[0]["status"] == "partially_reviewed"
            and coverage[0]["sha256"] == sha256(content)
            and coverage[0]["selectors"] == [selector],
            f"contradictory duplicate coverage was not merged conservatively: {coverage}",
        )
        assert_true(
            sum(
                "artifact_coverage_downgraded" in error
                for error in finished["execution_status"]["errors"]
            )
            == 3,
            "coverage corrections were not explicit",
        )
        assert_true(
            sum(
                "artifact_coverage_duplicate: evidence.log" in error
                for error in finished["execution_status"]["errors"]
            )
            == 1,
            "duplicate coverage normalization was not machine-visible",
        )
        assert_true(
            investigator.validate_report_schema(finished) == [],
            "runner-owned skipped coverage broke the report schema",
        )


def test_duplicate_valid_coverage_is_unique() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        run = Path(temporary)
        content = b"fixture\n"
        (run / "evidence.log").write_bytes(content)
        selector: dict[str, object] = {
            "kind": "log",
            "path": "evidence.log",
            "sha256": sha256(content),
            "description": "Reviewed fixture line",
            "line_start": 1,
            "line_end": 1,
        }
        row = {
            "path": "evidence.log",
            "status": "reviewed",
            "sha256": sha256(content),
            "size_bytes": len(content),
            "role": "fixture",
            "selectors": [selector],
            "notes": [],
        }
        report = minimal_model_report(selector)
        report["findings"] = []
        report["coverage"]["artifacts"] = [dict(row), dict(row)]
        finished = investigator.finish_report(
            report,
            run_dir=run,
            source_context_value=source_precheck(),
            model="fixture-model",
            backend="fixture-backend",
            tool_version="fixture-tool",
            prompt="fixture prompt",
            extra_errors=[],
        )
        coverage = finished["coverage"]["artifacts"]
        assert_true(
            len(coverage) == 1
            and coverage[0]["path"] == "evidence.log"
            and coverage[0]["status"] == "reviewed"
            and coverage[0]["selectors"] == [selector],
            f"duplicate valid coverage survived or lost its selector: {coverage}",
        )
        assert_true(
            finished["execution_status"]["state"] == "partial"
            and any(
                "artifact_coverage_duplicate: evidence.log" in error
                for error in finished["execution_status"]["errors"]
            ),
            "duplicate valid coverage looked complete",
        )


def test_two_valid_coverage_selectors_are_retained_as_reviewed() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        run = Path(temporary)
        content = b"first reviewed line\nsecond reviewed line\n"
        (run / "evidence.log").write_bytes(content)
        first_selector: dict[str, object] = {
            "kind": "log",
            "path": "evidence.log",
            "sha256": sha256(content),
            "description": "First reviewed line",
            "line_start": 1,
            "line_end": 1,
        }
        second_selector = {
            **first_selector,
            "description": "Second reviewed line",
            "line_start": 2,
            "line_end": 2,
        }
        report = minimal_model_report(first_selector)
        report["findings"] = []
        report["coverage"]["artifacts"] = [
            {
                "path": "evidence.log",
                "status": "reviewed",
                "sha256": sha256(content),
                "size_bytes": len(content),
                "role": "fixture",
                "selectors": [first_selector, second_selector],
                "notes": [],
            }
        ]
        finished = investigator.finish_report(
            report,
            run_dir=run,
            source_context_value=source_precheck(),
            model="fixture-model",
            backend="fixture-backend",
            tool_version="fixture-tool",
            prompt="fixture prompt",
            extra_errors=[],
        )
        coverage = finished["coverage"]["artifacts"]
        assert_true(
            len(coverage) == 1
            and coverage[0]["status"] == "reviewed"
            and coverage[0]["selectors"] == [first_selector, second_selector],
            f"two valid selectors were duplicated, removed, or downgraded: {coverage}",
        )
        assert_true(
            investigator.validate_published_selectors(run, finished) == [],
            "published valid selectors did not re-resolve",
        )


def test_cross_artifact_coverage_selector_is_removed_and_downgraded() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        run = Path(temporary)
        reviewed = b"reviewed\n"
        other = b"other\n"
        (run / "reviewed.log").write_bytes(reviewed)
        (run / "other.log").write_bytes(other)
        matching_selector: dict[str, object] = {
            "kind": "log",
            "path": "reviewed.log",
            "sha256": sha256(reviewed),
            "description": "Reviewed line",
            "line_start": 1,
            "line_end": 1,
        }
        cross_artifact_selector: dict[str, object] = {
            "kind": "log",
            "path": "other.log",
            "sha256": sha256(other),
            "description": "A valid selector for a different artifact",
            "line_start": 1,
            "line_end": 1,
        }
        report = minimal_model_report(matching_selector)
        report["findings"] = []
        report["coverage"]["artifacts"] = [
            {
                "path": "reviewed.log",
                "status": "reviewed",
                "sha256": sha256(reviewed),
                "size_bytes": len(reviewed),
                "role": "fixture",
                "selectors": [matching_selector, cross_artifact_selector],
                "notes": [],
            }
        ]
        finished = investigator.finish_report(
            report,
            run_dir=run,
            source_context_value=source_precheck(),
            model="fixture-model",
            backend="fixture-backend",
            tool_version="fixture-tool",
            prompt="fixture prompt",
            extra_errors=[],
        )
        coverage = {
            item["path"]: item for item in finished["coverage"]["artifacts"]
        }
        assert_true(
            coverage["reviewed.log"]["status"] == "partially_reviewed"
            and coverage["reviewed.log"]["selectors"] == [matching_selector],
            f"cross-artifact selector survived or review stayed complete: {coverage}",
        )
        assert_true(
            any(
                "coverage selector targets another artifact: reviewed.log" in error
                for error in finished["execution_status"]["errors"]
            ),
            "cross-artifact selector correction was not machine-visible",
        )
        assert_true(
            investigator.validate_published_selectors(run, finished) == [],
            "corrected coverage retained an invalid selector",
        )


def test_private_artifact_path_is_redacted_and_loses_grounding() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        run = root / "run"
        run.mkdir()
        private_term = "SelectorPrivateFixture"
        artifact_name = f"capture-{private_term}.log"
        content = b"private-path fixture\n"
        safe_content = b"safe grounded fixture\n"
        (run / artifact_name).write_bytes(content)
        (run / "safe.log").write_bytes(safe_content)
        selector: dict[str, object] = {
            "kind": "log",
            "path": artifact_name,
            "sha256": sha256(content),
            "description": "Evidence whose filename is locally private",
            "line_start": 1,
            "line_end": 1,
        }
        safe_selector: dict[str, object] = {
            "kind": "log",
            "path": "safe.log",
            "sha256": sha256(safe_content),
            "description": "Safe evidence",
            "line_start": 1,
            "line_end": 1,
        }
        report = minimal_model_report(selector)
        safe_finding = minimal_model_report(safe_selector)["findings"][0]
        safe_finding["id"] = "safe-grounded"
        report["findings"].append(safe_finding)
        report["coverage"]["artifacts"] = [
            {
                "path": artifact_name,
                "status": "reviewed",
                "sha256": sha256(content),
                "size_bytes": len(content),
                "role": "fixture",
                "selectors": [selector],
                "notes": [],
            },
            {
                "path": "safe.log",
                "status": "reviewed",
                "sha256": sha256(safe_content),
                "size_bytes": len(safe_content),
                "role": "fixture",
                "selectors": [safe_selector],
                "notes": [],
            },
        ]
        terms_path = root / "privacy-terms.txt"
        terms_path.write_text(private_term.lower() + "\n", encoding="utf-8")
        previous_terms = os.environ.get("V1SIMPLE_PRIVACY_TERMS")
        os.environ["V1SIMPLE_PRIVACY_TERMS"] = str(terms_path)
        try:
            finished = investigator.finish_report(
                report,
                run_dir=run,
                source_context_value=source_precheck(),
                model="fixture-model",
                backend="fixture-backend",
                tool_version="fixture-tool",
                prompt="fixture prompt",
                extra_errors=[],
            )
        finally:
            if previous_terms is None:
                os.environ.pop("V1SIMPLE_PRIVACY_TERMS", None)
            else:
                os.environ["V1SIMPLE_PRIVACY_TERMS"] = previous_terms

        serialized = json.dumps(finished)
        assert_true(
            private_term.casefold() not in serialized.casefold()
            and "<redacted-private-term>" in serialized,
            "local private filename survived report sanitization",
        )
        assert_true(
            [finding["id"] for finding in finished["findings"]]
            == ["safe-grounded"]
            and finished["unresolved"] == [],
            "private evidence survived or the independent safe finding was discarded",
        )
        coverage = {
            item["path"]: item for item in finished["coverage"]["artifacts"]
        }
        private_coverage = next(
            item
            for path, item in coverage.items()
            if "<redacted-private-term>" in path
        )
        assert_true(
            private_coverage["status"] == "skipped"
            and private_coverage["selectors"] == []
            and coverage["safe.log"]["status"] == "reviewed"
            and coverage["safe.log"]["selectors"] == [safe_selector],
            f"privacy-mutated coverage still claimed review: {coverage}",
        )
        assert_true(
            investigator.validate_published_selectors(run, finished) == [],
            "privacy-safe report still contained an unresolvable selector",
        )


def test_published_selector_validation_covers_all_diagnostic_owners() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        run = Path(temporary)
        content = b"evidence\n"
        (run / "evidence.log").write_bytes(content)
        evidence: dict[str, object] = {
            "kind": "log",
            "path": "evidence.log",
            "sha256": sha256(content),
            "description": "One exact line",
            "line_start": 1,
            "line_end": 1,
        }
        report = minimal_model_report(evidence)
        finding = report["findings"][0]
        code = finding["code"][0]
        finding["counterevidence"] = [dict(evidence)]
        report["unresolved"] = [
            {
                "id": "fixture-unresolved",
                "title": "Fixture unresolved",
                "causal_status": "unknown",
                "observation": "Observed fixture",
                "why_unknown": "Cause is not established",
                "evidence": [dict(evidence)],
                "counterevidence": [dict(evidence)],
                "code": [dict(code)],
                "clock_mapping_ids": [],
                "hypotheses": [
                    {
                        "rank": 1,
                        "description": "Fixture hypothesis",
                        "evidence": [dict(evidence)],
                        "code": [dict(code)],
                    }
                ],
                "next_observation": {
                    "description": "Read the fixture again",
                    "distinguishes": ["present", "absent"],
                    "minimal_evidence": "One exact line",
                },
            }
        ]
        report["coverage"]["artifacts"] = [
            {
                "path": "evidence.log",
                "status": "reviewed",
                "sha256": sha256(content),
                "size_bytes": len(content),
                "role": "fixture",
                "selectors": [dict(evidence)],
                "notes": [],
            }
        ]
        report["coverage"]["code"] = [dict(code)]
        finished = investigator.finish_report(
            report,
            run_dir=run,
            source_context_value=source_precheck(),
            model="fixture-model",
            backend="fixture-backend",
            tool_version="fixture-tool",
            prompt="fixture prompt",
            extra_errors=[],
        )
        assert_true(
            investigator.validate_published_selectors(run, finished) == [],
            "valid selector owners failed post-publication resolution",
        )

        invalid = json.loads(json.dumps(finished))
        artifact_selectors = [
            invalid["coverage"]["artifacts"][0]["selectors"][0],
            invalid["findings"][0]["evidence"][0],
            invalid["findings"][0]["counterevidence"][0],
            invalid["unresolved"][0]["evidence"][0],
            invalid["unresolved"][0]["counterevidence"][0],
            invalid["unresolved"][0]["hypotheses"][0]["evidence"][0],
        ]
        code_selectors = [
            invalid["coverage"]["code"][0],
            invalid["findings"][0]["code"][0],
            invalid["unresolved"][0]["code"][0],
            invalid["unresolved"][0]["hypotheses"][0]["code"][0],
        ]
        for selector in artifact_selectors:
            selector["path"] = "missing-evidence.log"
        for selector in code_selectors:
            selector["path"] = "missing-owner.cpp"
        errors = "\n".join(
            investigator.validate_published_selectors(run, invalid)
        )
        for owner in (
            "coverage.artifacts[1].selectors[1]",
            "coverage.code[1]",
            "findings[1].evidence[1]",
            "findings[1].counterevidence[1]",
            "findings[1].code[1]",
            "unresolved[1].evidence[1]",
            "unresolved[1].counterevidence[1]",
            "unresolved[1].code[1]",
            "unresolved[1].hypotheses[1].evidence[1]",
            "unresolved[1].hypotheses[1].code[1]",
        ):
            assert_true(owner in errors, f"post-publication validation missed {owner}")


def test_artifact_identity_is_runner_owned_and_nonexistent_paths_are_omitted() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        run = Path(temporary)
        reviewed_content = b"reviewed\n"
        skipped_content = b"skipped\n"
        (run / "reviewed.log").write_bytes(reviewed_content)
        (run / "skipped.log").write_bytes(skipped_content)
        selector: dict[str, object] = {
            "kind": "log",
            "path": "reviewed.log",
            "sha256": sha256(reviewed_content),
            "description": "Reviewed fixture line",
            "line_start": 1,
            "line_end": 1,
        }
        report = minimal_model_report(selector)
        report["findings"] = []
        report["coverage"]["artifacts"] = [
            {
                "path": "reviewed.log",
                "status": "reviewed",
                "sha256": sha256(reviewed_content),
                "size_bytes": 999999,
                "role": "invented-role",
                "selectors": [selector],
                "notes": ["semantic reviewed note"],
            },
            {
                "path": "skipped.log",
                "status": "skipped",
                "sha256": "0" * 64,
                "size_bytes": 888888,
                "role": "invented-role",
                "selectors": [],
                "notes": ["semantic skipped note"],
            },
            {
                "path": "ghost.log",
                "status": "skipped",
                "sha256": None,
                "size_bytes": None,
                "role": "invented-role",
                "selectors": [],
                "notes": ["must not survive"],
            },
        ]
        finished = investigator.finish_report(
            report,
            run_dir=run,
            source_context_value=source_precheck(),
            model="fixture-model",
            backend="fixture-backend",
            tool_version="fixture-tool",
            prompt="fixture prompt",
            extra_errors=[],
        )
        coverage = {item["path"]: item for item in finished["coverage"]["artifacts"]}
        assert_true(set(coverage) == {"reviewed.log", "skipped.log"}, f"nonexistent path survived: {coverage}")
        assert_true(
            coverage["reviewed.log"]
            == {
                "path": "reviewed.log",
                "status": "reviewed",
                "sha256": sha256(reviewed_content),
                "size_bytes": len(reviewed_content),
                "role": "text",
                "selectors": [selector],
                "notes": ["semantic reviewed note"],
            },
            f"single reviewed row retained model identity metadata: {coverage['reviewed.log']}",
        )
        assert_true(
            coverage["skipped.log"]["status"] == "skipped"
            and coverage["skipped.log"]["sha256"] == sha256(skipped_content)
            and coverage["skipped.log"]["size_bytes"] == len(skipped_content)
            and coverage["skipped.log"]["role"] == "text"
            and coverage["skipped.log"]["notes"] == ["semantic skipped note"],
            f"skipped row did not use runner identity: {coverage['skipped.log']}",
        )
        assert_true(
            finished["execution_status"]["state"] == "partial"
            and any(
                "artifact_coverage_nonexistent: ghost.log" in error
                for error in finished["execution_status"]["errors"]
            ),
            "nonexistent coverage omission was not machine-visible",
        )


def test_runner_inventory_retains_model_omissions() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        run = Path(temporary)
        first = b"reviewed\n"
        second = b"omitted\n"
        (run / "reviewed.log").write_bytes(first)
        (run / "omitted.log").write_bytes(second)
        selector: dict[str, object] = {
            "kind": "log",
            "path": "reviewed.log",
            "sha256": sha256(first),
            "description": "Reviewed fixture line",
            "line_start": 1,
            "line_end": 1,
        }
        report = minimal_model_report(selector)
        report["findings"] = []
        report["coverage"]["artifacts"] = [
            {
                "path": "reviewed.log",
                "status": "reviewed",
                "sha256": sha256(first),
                "size_bytes": len(first),
                "role": "fixture",
                "selectors": [selector],
                "notes": [],
            }
        ]
        finished = investigator.finish_report(
            report,
            run_dir=run,
            source_context_value=source_precheck(),
            model="fixture-model",
            backend="fixture-backend",
            tool_version="fixture-tool",
            prompt="fixture prompt",
            extra_errors=[],
        )
        coverage = {item["path"]: item for item in finished["coverage"]["artifacts"]}
        assert_true(
            coverage["omitted.log"]["status"] == "skipped"
            and coverage["omitted.log"]["sha256"] == sha256(second),
            "runner-owned inventory lost or falsely reviewed a model-omitted artifact",
        )
        assert_true(
            finished["execution_status"]["state"] == "partial"
            and any(
                "artifact_coverage_missing: omitted.log" in error
                for error in finished["execution_status"]["errors"]
            ),
            "silent artifact omission looked complete",
        )


def test_nested_csv_selector_keys_are_resolved() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        run = Path(temporary)
        content = b"event,value\nalpha,1\nbeta,2\n"
        (run / "metrics.csv").write_bytes(content)
        selector: dict[str, object] = {
            "kind": "csv",
            "path": "metrics.csv",
            "sha256": sha256(content),
            "description": "Wrong stable key for the selected first row",
            "row_start": 1,
            "row_end": 1,
            "keys": ["event=beta"],
        }
        report = minimal_model_report(selector)
        report["findings"] = []
        report["coverage"]["clock_mappings"] = [
            {
                "id": "fixture-clock",
                "from_clock": "a",
                "to_clock": "b",
                "status": "measured",
                "method": "fixture",
                "uncertainty_s": 0.1,
                "evidence": [selector],
                "limitations": [],
            }
        ]
        finished = investigator.finish_report(
            report,
            run_dir=run,
            source_context_value=source_precheck(),
            model="fixture-model",
            backend="fixture-backend",
            tool_version="fixture-tool",
            prompt="fixture prompt",
            extra_errors=[],
        )
        assert_true(finished["execution_status"]["state"] == "partial", "bad CSV key resolved")
        assert_true(
            any("CSV keys do not resolve" in note for note in finished["coverage"]["notes"]),
            "nested selector error was not recorded",
        )


def test_clock_mapping_failures_downgrade_only_referencing_findings() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        run = Path(temporary)
        content = b'{"event":"anchor"}\n'
        (run / "clock.ndjson").write_bytes(content)
        evidence: dict[str, object] = {
            "kind": "ndjson",
            "path": "clock.ndjson",
            "sha256": sha256(content),
            "description": "Fixture anchor",
            "line_start": 1,
            "line_end": 1,
            "keys": ["event=anchor"],
        }
        bad_evidence = dict(evidence)
        bad_evidence["sha256"] = "0" * 64
        report = minimal_model_report(evidence)
        affected = report["findings"][0]
        affected["clock_mapping_ids"] = ["bad-clock", "duplicate-clock", "missing-clock"]
        unaffected = minimal_model_report(evidence)["findings"][0]
        unaffected["id"] = "unaffected-clock"
        report["findings"].append(unaffected)

        def mapping(mapping_id: str, selector: dict[str, object]) -> dict[str, object]:
            return {
                "id": mapping_id,
                "from_clock": "host",
                "to_clock": "video",
                "status": "measured",
                "method": "fixture",
                "uncertainty_s": 0.1,
                "evidence": [selector],
                "limitations": [],
            }

        report["coverage"]["clock_mappings"] = [
            mapping("bad-clock", bad_evidence),
            mapping("duplicate-clock", evidence),
            mapping("duplicate-clock", evidence),
        ]
        report["source"]["binary_identities"] = [
            {
                "name": "invalid-only",
                "basis": "device_attested",
                "sha256": "1" * 64,
                "evidence": [bad_evidence],
                "limitations": [],
            },
            {
                "name": "mixed",
                "basis": "device_attested",
                "sha256": sha256(content),
                "evidence": [evidence, bad_evidence],
                "limitations": [],
            },
        ]
        finished = investigator.finish_report(
            report,
            run_dir=run,
            source_context_value=source_precheck(),
            model="fixture-model",
            backend="fixture-backend",
            tool_version="fixture-tool",
            prompt="fixture prompt",
            extra_errors=[],
        )
        findings = {item["id"]: item for item in finished["findings"]}
        assert_true(findings["fixture-correlation"]["causal_status"] == "probable", "bad clock supported confirmation")
        assert_true(findings["unaffected-clock"]["causal_status"] == "confirmed", "unrelated finding was downgraded")
        notes = "\n".join(finished["coverage"]["notes"])
        assert_true("clock mapping has no valid evidence: bad-clock" in notes, "invalid clock evidence was hidden")
        assert_true("clock mapping id is not unique: duplicate-clock" in notes, "duplicate clock id resolved")
        assert_true("clock mapping id does not resolve: missing-clock" in notes, "missing clock id resolved")
        bad_mapping = finished["coverage"]["clock_mappings"][0]
        identities = {item["name"]: item for item in finished["source"]["binary_identities"]}
        assert_true(
            bad_mapping["status"] == "unavailable"
            and bad_mapping["uncertainty_s"] is None,
            "clock mapping retained a measured claim with zero evidence",
        )
        assert_true(
            identities["invalid-only"]["basis"] == "unavailable"
            and identities["invalid-only"]["sha256"] is None
            and identities["mixed"]["basis"] == "device_attested"
            and len(identities["mixed"]["evidence"]) == 1,
            "binary identity grounding was either retained falsely or stripped too broadly",
        )


def test_default_is_local_and_hosted_requires_explicit_choice() -> None:
    names = (
        "BENCH_INVESTIGATOR_MODEL",
        "BENCH_INVESTIGATOR_LOCAL_PROVIDER",
        "BENCH_INVESTIGATOR_OSS",
        "BENCH_INVESTIGATOR_HOSTED",
    )
    previous = {name: os.environ.get(name) for name in names}
    try:
        for name in names:
            os.environ.pop(name, None)
        os.environ["BENCH_INVESTIGATOR_HOSTED"] = "1"
        default = investigator.parse_args(["run"])
        assert_true(
            not default.hosted
            and default.local_provider == "ollama"
            and default.model == "qwen3-vl:8b"
            and default.timeout_seconds == 3600,
            f"automatic execution crossed the local boundary: {default}",
        )
        os.environ["BENCH_INVESTIGATOR_MODEL"] = "gpt-5.6-sol"
        configured_model = investigator.parse_args(["run"])
        assert_true(
            not configured_model.hosted and configured_model.local_provider == "ollama",
            "a model environment variable silently enabled hosted inference",
        )
        os.environ.pop("BENCH_INVESTIGATOR_MODEL")
        for cloud_model in ("glm-4.7:cloud", "gpt-oss:120b-cloud"):
            try:
                with redirect_stderr(io.StringIO()):
                    investigator.parse_args(["run", "--model", cloud_model])
            except SystemExit as exc:
                assert_true(exc.code == 2, f"cloud-tag rejection exited {exc.code}")
            else:
                raise AssertionError(
                    f"local mode accepted Ollama cloud model {cloud_model}"
                )
        for argv in (
            ["run", "--oss"],
            ["run", "--hosted", "--local-provider", "ollama"],
            ["run", "--hosted", "--local-provider=ollama"],
        ):
            try:
                with redirect_stderr(io.StringIO()):
                    investigator.parse_args(argv)
            except SystemExit as exc:
                assert_true(exc.code == 2, f"wrong argument failure for {argv}")
            else:
                raise AssertionError(f"unsafe local arguments were accepted: {argv}")
        local = investigator.parse_args(
            [
                "run",
                "--local-provider",
                "lmstudio",
                "--model",
                "vision-local",
            ]
        )
        assert_true(not local.hosted and local.local_provider == "lmstudio", "local mode changed")
        assert_true(local.model == "vision-local", "explicit local model was lost")
        hosted = investigator.parse_args(["run", "--hosted"])
        assert_true(hosted.hosted and hosted.local_provider is None, "hosted choice was not explicit")
        assert_true(hosted.model == "gpt-5.6-sol", "hosted default model changed")
    finally:
        for name, value in previous.items():
            if value is None:
                os.environ.pop(name, None)
            else:
                os.environ[name] = value


def test_codex_executable_resolution_uses_override_path_and_app_bundle() -> None:
    name = "BENCH_INVESTIGATOR_CODEX"
    previous = os.environ.get(name)
    try:
        os.environ[name] = "/configured/codex"
        with patch.object(investigator.shutil, "which", return_value="/path/codex"):
            assert_true(
                investigator.resolve_codex_executable() == "/configured/codex",
                "explicit Codex executable lost precedence",
            )

        os.environ.pop(name, None)
        with patch.object(investigator.shutil, "which", return_value="/path/codex"):
            assert_true(
                investigator.resolve_codex_executable() == "/path/codex",
                "PATH Codex executable was not selected",
            )

        with tempfile.TemporaryDirectory() as temporary:
            bundled = Path(temporary) / "codex"
            bundled.write_text("#!/bin/sh\n", encoding="utf-8")
            bundled.chmod(0o700)
            with (
                patch.object(investigator.shutil, "which", return_value=None),
                patch.object(investigator, "BUNDLED_CODEX_EXECUTABLES", (bundled,)),
            ):
                assert_true(
                    investigator.resolve_codex_executable() == str(bundled),
                    "executable app-bundled Codex was not discovered",
                )
    finally:
        if previous is None:
            os.environ.pop(name, None)
        else:
            os.environ[name] = previous


def test_provider_command_and_local_environment_do_not_fall_back() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        backend = root / "fake-codex"
        output = root / "report.json"
        capture = root / "capture.json"
        report = minimal_model_report(
            {
                "kind": "file",
                "path": "unused",
                "sha256": "0" * 64,
                "description": "unused fixture",
            }
        )
        report["findings"] = []
        backend.write_text(
            "#!/usr/bin/env python3\n"
            "import json,os,sys\n"
            "from pathlib import Path\n"
            "output=Path(sys.argv[sys.argv.index('--output-last-message')+1])\n"
            f"output.write_text({json.dumps(json.dumps(report))})\n"
            f"Path({str(capture)!r}).write_text(json.dumps({{\n"
            "  'argv':sys.argv[1:],\n"
            "  'output_schema':json.loads(Path(sys.argv[sys.argv.index('--output-schema')+1]).read_text()),\n"
            "  'codex_oss_base_url':os.environ.get('CODEX_OSS_BASE_URL'),\n"
            "  'ollama_host':os.environ.get('OLLAMA_HOST'),\n"
            "  'openai_key':os.environ.get('OPENAI_API_KEY'),\n"
            "  'https_proxy':os.environ.get('HTTPS_PROXY'),\n"
            "  'no_proxy':os.environ.get('NO_PROXY'),\n"
            "  'path_present':bool(os.environ.get('PATH')),\n"
            "}))\n",
            encoding="utf-8",
        )
        os.chmod(backend, 0o700)
        with patch.dict(
            os.environ,
            {
                "OPENAI_API_KEY": "must-not-reach-local-model",
                "CODEX_OSS_BASE_URL": "https://remote-provider.example/v1",
                "OLLAMA_HOST": "remote.example:11434",
                "HTTPS_PROXY": "https://private-proxy.example",
            },
        ):
            investigator.invoke_codex(
                executable=str(backend),
                model=investigator.LOCAL_DEFAULT_MODEL,
                oss=True,
                local_provider="ollama",
                prompt="fixture",
                images=[],
                output_path=output,
                timeout_seconds=10,
            )
        captured = json.loads(capture.read_text(encoding="utf-8"))
        argv = captured["argv"]
        assert_true("--oss" in argv, "local invocation omitted --oss")
        local_schema_path = argv[argv.index("--output-schema") + 1]
        assert_true(
            local_schema_path == str(investigator.SCHEMA_PATH),
            "local invocation no longer uses the canonical schema",
        )
        assert_true(
            captured["output_schema"] == investigator.load_report_schema(),
            "local invocation schema content changed",
        )
        config_values = [argv[index + 1] for index, value in enumerate(argv[:-1]) if value == "-c"]
        assert_true(
            'shell_environment_policy.inherit="core"' in config_values
            and "shell_environment_policy.ignore_default_excludes=false" in config_values,
            f"shell environment privacy overrides were lost: {config_values}",
        )
        assert_true("analytics.enabled=false" in config_values, "local analytics opt-out was lost")
        assert_true(
            "check_for_update_on_startup=false" in config_values
            and 'web_search="disabled"' in config_values,
            f"local non-model egress overrides were lost: {config_values}",
        )
        assert_true(
            "project_doc_max_bytes=0" in config_values,
            "local investigator loaded general repository workflow instructions",
        )
        assert_true(
            "skills.include_instructions=false" in config_values,
            "local investigator loaded general repository skills",
        )
        assert_true(
            f"model_context_window={investigator.LOCAL_DEFAULT_CONTEXT_WINDOW}"
            in config_values
            and (
                "model_auto_compact_token_limit="
                f"{investigator.LOCAL_AUTO_COMPACT_TOKEN_LIMIT}"
            )
            in config_values
            and f"tool_output_token_limit={investigator.LOCAL_TOOL_OUTPUT_TOKEN_LIMIT}"
            in config_values,
            f"default local context bounds were lost: {config_values}",
        )
        provider_index = argv.index("--local-provider")
        assert_true(argv[provider_index + 1] == "ollama", "local provider changed")
        model_index = argv.index("--model")
        assert_true(argv[model_index + 1] == "qwen3-vl:8b", "local model changed")
        assert_true(
            captured["codex_oss_base_url"] == "http://127.0.0.1:11434/v1",
            "Codex OSS provider endpoint was not loopback-only",
        )
        assert_true(captured["ollama_host"] == "127.0.0.1:11434", "Ollama was not loopback-only")
        assert_true(captured["openai_key"] is None, "hosted credential reached local execution")
        assert_true(captured["https_proxy"] is None, "remote proxy reached local execution")
        assert_true(captured["no_proxy"] == "localhost,127.0.0.1,::1", "loopback proxy bypass changed")
        assert_true(captured["path_present"], "required subprocess PATH was removed")
        lmstudio_environment = investigator.codex_environment("lmstudio")
        assert_true(
            lmstudio_environment["CODEX_OSS_BASE_URL"] == "http://127.0.0.1:1234/v1"
            and lmstudio_environment["LMS_SERVER_HOST"] == "127.0.0.1",
            "LM Studio was not loopback-only",
        )

        investigator.invoke_codex(
            executable=str(backend),
            model=investigator.HOSTED_DEFAULT_MODEL,
            oss=False,
            local_provider=None,
            prompt="explicit hosted fixture",
            images=[],
            output_path=output,
            timeout_seconds=10,
        )
        hosted_capture = json.loads(capture.read_text(encoding="utf-8"))
        hosted_argv = hosted_capture["argv"]
        assert_true("--oss" not in hosted_argv, "explicit hosted invocation stayed local")
        assert_true("--local-provider" not in hosted_argv, "hosted invocation retained a local provider")
        hosted_schema_path = hosted_argv[hosted_argv.index("--output-schema") + 1]
        assert_true(
            hosted_schema_path != str(investigator.SCHEMA_PATH),
            "hosted invocation bypassed its compatible transport schema",
        )
        assert_true(
            not Path(hosted_schema_path).exists(),
            "hosted transport schema was not removed after execution",
        )
        assert_codex_transport_schema(hosted_capture["output_schema"])
        hosted_config_values = [
            hosted_argv[index + 1]
            for index, value in enumerate(hosted_argv[:-1])
            if value == "-c"
        ]
        assert_true(
            'shell_environment_policy.inherit="core"' in hosted_config_values
            and "shell_environment_policy.ignore_default_excludes=false" in hosted_config_values
            and "analytics.enabled=false" in hosted_config_values,
            f"hosted privacy overrides were lost: {hosted_config_values}",
        )
        assert_true(
            "check_for_update_on_startup=false" in hosted_config_values
            and 'web_search="disabled"' in hosted_config_values,
            f"hosted non-model egress overrides were lost: {hosted_config_values}",
        )
        assert_true(
            "project_doc_max_bytes=0" in hosted_config_values,
            "hosted investigator loaded general repository workflow instructions",
        )
        assert_true(
            "skills.include_instructions=false" in hosted_config_values,
            "hosted investigator loaded general repository skills",
        )
        assert_true(
            not any(
                value.startswith(
                    (
                        "model_context_window=",
                        "model_auto_compact_token_limit=",
                        "tool_output_token_limit=",
                    )
                )
                for value in hosted_config_values
            ),
            f"default-local context bounds leaked into hosted execution: {hosted_config_values}",
        )
        hosted_model_index = hosted_argv.index("--model")
        assert_true(hosted_argv[hosted_model_index + 1] == "gpt-5.6-sol", "hosted model changed")

        for local_provider, model in (
            ("ollama", "custom-local-model"),
            ("lmstudio", investigator.LOCAL_DEFAULT_MODEL),
        ):
            investigator.invoke_codex(
                executable=str(backend),
                model=model,
                oss=True,
                local_provider=local_provider,
                prompt="non-default local fixture",
                images=[],
                output_path=output,
                timeout_seconds=10,
            )
            local_argv = json.loads(capture.read_text(encoding="utf-8"))["argv"]
            local_config_values = [
                local_argv[index + 1]
                for index, value in enumerate(local_argv[:-1])
                if value == "-c"
            ]
            assert_true(
                not any(
                    value.startswith(
                        (
                            "model_context_window=",
                            "model_auto_compact_token_limit=",
                            "tool_output_token_limit=",
                        )
                    )
                    for value in local_config_values
                ),
                f"default-model bounds leaked into {local_provider}/{model}: "
                f"{local_config_values}",
            )


def test_missing_run_error_does_not_print_resolved_user_path() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        missing = Path(temporary) / "SensitiveLocalFixture" / "missing-run"
        stderr = io.StringIO()
        argv = ["bench_investigate.py", str(missing)]
        with patch.object(sys, "argv", argv), redirect_stderr(stderr):
            result = investigator.main()
        message = stderr.getvalue()
        assert_true(result == 3, f"missing run exit changed: {result}")
        assert_true(
            str(missing) not in message
            and "SensitiveLocalFixture" not in message
            and "<private>" in message,
            f"missing run error exposed its resolved input path: {message}",
        )


def test_atomic_report_replaces_old_content() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        target = Path(temporary) / "investigation.json"
        target.write_text("old\n", encoding="utf-8")
        investigator.atomic_write_json(target, {"new": True})
        assert_true(json.loads(target.read_text(encoding="utf-8")) == {"new": True}, "replace failed")
        leftovers = list(target.parent.glob(".investigation.json.*"))
        assert_true(not leftovers, f"atomic temporary file leaked: {leftovers}")


def test_raw_model_json_is_validated_before_postprocessing() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        backend = root / "fake-codex"
        output = root / "report.json"
        backend.write_text(
            "#!/usr/bin/env python3\n"
            "import json,sys\n"
            "from pathlib import Path\n"
            "output=Path(sys.argv[sys.argv.index('--output-last-message')+1])\n"
            "output.write_text(json.dumps({'verdict':'PASS'}))\n",
            encoding="utf-8",
        )
        os.chmod(backend, 0o700)
        try:
            investigator.invoke_codex(
                executable=str(backend),
                model="fixture-model",
                oss=False,
                local_provider=None,
                prompt="fixture",
                images=[],
                output_path=output,
                timeout_seconds=10,
            )
        except investigator.InvestigationError as exc:
            assert_true(exc.code == "model_output_invalid", f"wrong raw schema error: {exc.code}")
            assert_true("Raw model report violates schema" in str(exc), "raw validation was deferred")
        else:
            raise AssertionError("schema-invalid raw model JSON reached postprocessing")


def test_codex_output_schema_requires_every_property() -> None:
    assert_codex_transport_schema(investigator.codex_output_schema())


def test_transport_nulls_are_removed_before_canonical_validation() -> None:
    evidence = {
        "kind": "file",
        "path": "unused",
        "sha256": "0" * 64,
        "description": "fixture evidence",
    }
    optional_fields = (
        "json_pointer",
        "line_start",
        "line_end",
        "row_start",
        "row_end",
        "keys",
        "start_pts_s",
        "end_pts_s",
        "start_frame",
        "end_frame",
        "attachment_index",
        "cell_indices",
    )
    evidence.update({name: None for name in optional_fields})
    report = minimal_model_report(evidence)
    report["coverage"]["attachments"] = []
    report["coverage"]["artifacts"] = [
        {
            "path": "unused",
            "status": "skipped",
            "sha256": "0" * 64,
            "size_bytes": 0,
            "role": "text",
            "selectors": None,
            "notes": [],
        }
    ]
    assert_true(
        bool(investigator.validate_report_schema(report)),
        "canonical v2 unexpectedly accepted transport-only nulls",
    )
    investigator.remove_transport_nulls(report)
    normalized = report["findings"][0]["evidence"][0]
    assert_true(
        all(name not in normalized for name in optional_fields),
        "transport-only selector fields reached the canonical report",
    )
    assert_true(
        "selectors" not in report["coverage"]["artifacts"][0],
        "transport-only artifact selectors reached the canonical report",
    )
    assert_true(
        not investigator.validate_report_schema(report),
        "normalized report no longer matches canonical v2",
    )


def test_codex_failure_uses_sanitized_jsonl_terminal_error() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        output = root / "report.json"
        nested = json.dumps(
            {
                "type": "error",
                "error": {
                    "message": f"exact hosted failure at {root}",
                },
            }
        )
        stdout = "\n".join(
            (
                "not-json",
                json.dumps(
                    {
                        "type": "error",
                        "message": "earlier stream error",
                    }
                ),
                json.dumps(
                    {
                        "type": "item.completed",
                        "item": {"type": "error", "message": "nonfatal warning"},
                    }
                ),
                json.dumps(
                    {
                        "type": "turn.failed",
                        "error": {"message": nested},
                    }
                ),
            )
        )
        completed = subprocess.CompletedProcess(
            args=["fixture-codex"],
            returncode=1,
            stdout=stdout,
            stderr="stderr fallback",
        )
        original_unlink = Path.unlink

        def reject_transport_schema_cleanup(path: Path, *args: object, **kwargs: object) -> None:
            if path.name.startswith(".codex-output-schema-"):
                raise PermissionError("fixture cleanup denied")
            original_unlink(path, *args, **kwargs)

        with (
            patch.object(investigator.subprocess, "run", return_value=completed),
            patch.object(Path, "unlink", reject_transport_schema_cleanup),
        ):
            try:
                investigator.invoke_codex(
                    executable="fixture-codex",
                    model="fixture-model",
                    oss=False,
                    local_provider=None,
                    prompt="fixture",
                    images=[],
                    output_path=output,
                    timeout_seconds=10,
                    private_paths=(root,),
                )
            except investigator.InvestigationError as exc:
                message = str(exc)
                assert_true(exc.code == "backend_failed", f"wrong failure code: {exc.code}")
                assert_true("exact hosted failure" in message, "terminal JSONL error was lost")
                assert_true("earlier stream error" not in message, "stream error won over terminal")
                assert_true("stderr fallback" not in message, "stderr won over JSONL error")
                assert_true("cleanup denied" not in message, "cleanup failure masked backend error")
                assert_true(str(root) not in message and "<private>" in message, "error leaked path")
            else:
                raise AssertionError("nonzero Codex result did not fail")


def test_image_attachment_limit_is_global_across_passes() -> None:
    def attachments(prefix: str, count: int) -> list[dict[str, object]]:
        return [
            {"file": Path(f"{prefix}-{index}.jpg"), "manifest": {"purpose": prefix}}
            for index in range(count)
        ]

    attached: list[dict[str, object]] = []
    omitted = investigator.append_bounded_attachments(attached, attachments("pass-1", 30))
    omitted += investigator.append_bounded_attachments(attached, attachments("pass-2", 30))
    omitted += investigator.append_bounded_attachments(attached, attachments("pass-3", 5))
    assert_true(
        len(attached) == investigator.MAX_ATTACHED_IMAGES,
        f"global image attachment limit was exceeded: {len(attached)}",
    )
    assert_true(
        omitted == 65 - investigator.MAX_ATTACHED_IMAGES,
        f"wrong omitted-image count across passes: {omitted}",
    )

    initial: list[dict[str, object]] = []
    initial_omitted = investigator.append_bounded_attachments(
        initial,
        attachments("initial", 30),
        limit=investigator.MAX_INITIAL_IMAGES,
    )
    assert_true(
        len(initial) == investigator.MAX_INITIAL_IMAGES and initial_omitted == 22,
        "initial pass did not preserve room for requested intervals",
    )
    investigator.append_bounded_attachments(initial, attachments("follow-up", 30))
    assert_true(
        len(initial) == investigator.MAX_ATTACHED_IMAGES,
        "follow-up intervals could not use the reserved attachment capacity",
    )


def test_video_limits_apply_before_extraction_and_manifest_preserves_order() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        run = root / "run"
        run.mkdir()
        artifacts = []
        for index in range(10):
            path = run / f"video-{index:02d}.mp4"
            path.write_bytes(b"fixture")
            artifacts.append(
                {
                    "path": path.name,
                    "sha256": sha256(b"fixture"),
                    "kind": "video",
                    "size_bytes": 7,
                }
            )
        calls: list[str] = []

        def inspect_video(video: Path, output: Path, _requests: object, *, scan_overview: bool) -> dict[str, object]:
            calls.append(video.name)
            output.mkdir(parents=True)
            for name in ("overview.jpg", "change.jpg", "interval.jpg"):
                (output / name).write_bytes(name.encode())

            def sheet(filename: str, purpose: str) -> dict[str, object]:
                return {
                    "status": "complete",
                    "filename": filename,
                    "purpose": purpose,
                    "interval": {"start_pts_seconds": 0.0, "end_pts_seconds": 1.0},
                    "layout": {"columns": 1, "rows": 1, "cell_order": "row_major"},
                    "cells": [
                        {
                            "cell_index": 0,
                            "cell_label": "cell_001",
                            "nominal_requested_pts_seconds": 0.0,
                            "source_pts_measured": False,
                            "pts_uncertainty_seconds": 1.0,
                            "pts_uncertainty_interval": {
                                "start_pts_seconds": 0.0,
                                "end_pts_seconds": 1.0,
                            },
                        }
                    ],
                }

            return {
                "status": "complete",
                "overview": sheet("overview.jpg", "whole_video_overview"),
                "change_images": [sheet("change.jpg", "temporal_change_candidate")],
                "requested_intervals": [sheet("interval.jpg", "requested_interval")],
            }

        with patch.object(investigator, "load_video_helper", return_value=inspect_video):
            video, candidates, processed, omitted = investigator.extract_video_evidence(
                run,
                artifacts,
                root / "images",
                [],
                scan_overview=True,
                pass_number=2,
                remaining_run_budget=5,
            )
        assert_true(processed == 5 and len(calls) == 5, "run budget was applied after extraction")
        assert_true(omitted == [f"video-{index:02d}.mp4" for index in range(5, 10)], "omitted videos were not recorded")
        assert_true(sum(item["status"] == "not_processed" for item in video) == 5, "model metadata hid omitted videos")
        purposes = [item["manifest"]["purpose"] for item in candidates]
        assert_true(
            purposes
            == (["whole_video_overview"] * 5)
            + (["requested_interval"] * 5)
            + (["temporal_change_candidate"] * 5),
            "representative overview/request/change ordering was lost",
        )
        manifest = investigator.persist_attachment_manifest(run, candidates[:3])
        assert_true([item["attachment_index"] for item in manifest] == [1, 2, 3], "manifest is not ordered")
        assert_true(all(item["pass"] == 2 for item in manifest), "manifest lost its pass")
        assert_true(
            all(item["source_video_sha256"] == sha256(b"fixture") for item in manifest),
            "manifest lost its canonical source digest",
        )
        assert_true(
            all((run / item["sheet_path"]).is_file() for item in manifest)
            and all("filename" not in item for item in manifest),
            "manifest retained temporary names or lost durable sheets",
        )
        selected, per_pass_omitted = investigator.bounded_video_paths(
            [f"video-{index}" for index in range(20)], 100
        )
        assert_true(len(selected) == investigator.MAX_VIDEOS_PER_PASS, "per-pass video bound changed")
        assert_true(len(per_pass_omitted) == 12, "per-pass omissions were lost")


def test_attachment_retention_is_contained_and_reconciled() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        run = root / "run"
        outside = root / "outside"
        source = root / "sheet.jpg"
        run.mkdir()
        outside.mkdir()
        source_bytes = next(
            payload
            for index in range(10000)
            if "ab" in sha256(payload := f"private-frame-{index}".encode("utf-8"))
        )
        source.write_bytes(source_bytes)
        video_bytes = b"fixture source video"
        (run / "camera.mkv").write_bytes(video_bytes)

        metadata = {
            "pass": 1,
            "source_video_path": "camera.mkv",
            "source_video_sha256": sha256(video_bytes),
            "purpose": "whole_video_overview",
            "interval": {"start_pts_seconds": 0.0, "end_pts_seconds": 1.0},
            "layout": {"columns": 1, "rows": 1, "cell_order": "row_major"},
            "cells": [
                {
                    "cell_index": 0,
                    "cell_label": "cell_001",
                    "nominal_requested_pts_seconds": 0.0,
                    "source_pts_measured": False,
                    "pts_uncertainty_seconds": 1.0,
                    "pts_uncertainty_interval": {
                        "start_pts_seconds": 0.0,
                        "end_pts_seconds": 1.0,
                    },
                }
            ],
        }
        (run / investigator.ATTACHMENT_DIRECTORY).symlink_to(
            outside, target_is_directory=True
        )
        try:
            investigator.persist_attachment_manifest(
                run, [{"file": source, "manifest": metadata}]
            )
        except investigator.InvestigationError as exc:
            assert_true(
                exc.code == "attachment_directory_invalid",
                f"wrong containment error: {exc.code}",
            )
        else:
            raise AssertionError("symlinked attachment directory accepted")
        assert_true(list(outside.iterdir()) == [], "contact sheet escaped the run directory")

        (run / investigator.ATTACHMENT_DIRECTORY).unlink()
        first = investigator.persist_attachment_manifest(
            run, [{"file": source, "manifest": metadata}]
        )
        assert_true("ab" in first[0]["sheet_sha256"], "fixture missed digest collision")
        terms_path = root / "privacy-terms.txt"
        terms_path.write_text("ab\n", encoding="utf-8")
        previous_terms = os.environ.get("V1SIMPLE_PRIVACY_TERMS")
        os.environ["V1SIMPLE_PRIVACY_TERMS"] = str(terms_path)
        try:
            sanitized = investigator.sanitize_investigation_report(
                {"coverage": {"attachments": first}}, run
            )
        finally:
            if previous_terms is None:
                os.environ.pop("V1SIMPLE_PRIVACY_TERMS", None)
            else:
                os.environ["V1SIMPLE_PRIVACY_TERMS"] = previous_terms
        assert_true(
            sanitized["coverage"]["attachments"][0]["sheet_path"]
            == first[0]["sheet_path"]
            and investigator.validate_published_attachments(run, sanitized) == [],
            f"privacy transform broke durable attachment resolution: {sanitized}",
        )
        source.write_bytes(b"replacement-frame")
        second = investigator.persist_attachment_manifest(
            run, [{"file": source, "manifest": metadata}]
        )
        investigator.prune_attachment_files(run, second)
        assert_true(
            not (run / first[0]["sheet_path"]).exists()
            and (run / second[0]["sheet_path"]).is_file(),
            "stale retained sheet survived reconciliation",
        )


def test_model_context_summarizes_periodic_video_points() -> None:
    points = [index / 12 for index in range(3600)]
    video = [
        {
            "path": "camera.mov",
            "temporal_scan": {
                "sampled_pts_seconds": points,
                "change_candidates": [{"pts_seconds": 10.0}],
            },
            "coverage": {
                "full_frame_scan": {
                    "sampled_pts_seconds": points,
                    "continuous_coverage": False,
                }
            },
        }
    ]
    context = json.loads(
        investigator.compact_context(Path("run"), [], {}, video, None, 1, {})
    )
    compact = context["video_extraction"][0]
    assert_true(
        "sampled_pts_seconds" not in compact["temporal_scan"]
        and compact["temporal_scan"]["sampled_pts_summary"]["count"] == 3600,
        "temporal scan points were copied into the model prompt",
    )
    assert_true(
        "sampled_pts_seconds" not in compact["coverage"]["full_frame_scan"]
        and compact["coverage"]["full_frame_scan"]["continuous_coverage"] is False,
        "coverage summary lost the sampled-only limitation",
    )
    assert_true(
        compact["temporal_scan"]["change_candidates"] == [{"pts_seconds": 10.0}],
        "candidate timestamps were removed with periodic scan points",
    )

    irregular = [index / 12 for index in range(120)]
    irregular.extend(20 + index / 12 for index in range(120))
    video[0]["temporal_scan"]["sample_rate_hz"] = 12.0
    video[0]["temporal_scan"]["sampled_pts_seconds"] = irregular
    anomaly_context = json.loads(
        investigator.compact_context(Path("run"), [], {}, video, None, 1, {})
    )
    anomaly_summary = anomaly_context["video_extraction"][0]["temporal_scan"][
        "sampled_pts_summary"
    ]
    assert_true(anomaly_summary["gap_anomaly_count"] == 1, "gap anomaly was hidden")
    assert_true(
        anomaly_summary["gap_anomalies"]
        == [{"start_seconds": 9.916667, "end_seconds": 20.0, "gap_seconds": 10.083333}],
        f"gap location was not retained: {anomaly_summary}",
    )


def test_first_pass_requires_a_grounded_checkpoint_before_breadth() -> None:
    prompt = investigator.build_prompt("{}", 2, 1)
    instructions = investigator.INSTRUCTION_PATH.read_text(encoding="utf-8")
    combined = f"{prompt}\n{instructions}"
    normalized_instructions = " ".join(instructions.split())
    assert_true(
        "first-pass lead checkpoint" in prompt
        and "Before broad exploration" in combined
        and "schema-valid" in combined,
        "first pass no longer requires a results-producing checkpoint",
    )
    assert_true(
        "Read AGENTS.md" not in prompt
        and "Read tools/bench_investigator_prompt.md completely" in prompt
        and "purpose-specific read-only evidence session" in prompt
        and "Do not modify files or run repository setup" in prompt,
        "purpose-specific investigator regressed into repository workflow execution",
    )
    assert_true(
        "runner will add every model-omitted artifact as skipped" in prompt.casefold()
        and "adds every model-omitted inventory item as `skipped`"
        in normalized_instructions,
        "runner-owned skipped coverage was not explained to the model",
    )
    assert_true(
        "Emit exactly one coverage row per artifact" not in instructions
        and "do not omit inventory entries" not in instructions
        and "Inventory all readable files" not in instructions,
        "the prompt still commands exhaustive artifact transcription",
    )
    for required in (
        "raw evidence",
        "owning code",
        "counterevidence",
        "timing uncertainty",
        "semantically",
        "Review video semantically",
        "Interpret displayed meaning",
    ):
        assert_true(
            required in combined,
            f"lead-first prompt lost required investigation behavior: {required}",
        )

    follow_up = investigator.build_prompt("{}", 1, 2)
    assert_true(
        "synthesis pass" in follow_up
        and "Do not stop at the first lead" in follow_up
        and "other high-signal defects" in follow_up
        and "code, logs, metrics, traces, and supplied video" in follow_up,
        "synthesis prompt no longer expands beyond the grounded checkpoint",
    )


def test_attachment_allocation_is_reported_honestly() -> None:
    assert_true(
        investigator.attachment_limit_for_pass(1, 1)
        == investigator.MAX_ATTACHED_IMAGES,
        "single-pass review unnecessarily reserved unreachable follow-up capacity",
    )
    assert_true(
        investigator.attachment_limit_for_pass(1, 2)
        == investigator.MAX_INITIAL_IMAGES
        and investigator.attachment_limit_for_pass(2, 2)
        == investigator.MAX_ATTACHED_IMAGES,
        "multi-pass attachment reserve changed",
    )
    limitations = investigator.image_attachment_limitations(5, 0, 8)
    assert_true(
        limitations[0][0] == "initial_image_allocation"
        and "4 reserved slot(s) remained unused" in limitations[0][1]
        and "global" not in limitations[0][1],
        f"unused reserve was mislabeled as global exhaustion: {limitations}",
    )


def test_first_pass_keeps_every_main_video_change_candidate_under_the_image_cap() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        run = root / "run"
        run.mkdir()
        artifacts = []
        evidence_paths = []
        for suite in ("core", "display", "replay"):
            for filename in (".camera_preflight.mov", "evidence.mov"):
                relative = f"{suite}/camera/{filename}"
                path = run / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b"fixture")
                if filename == "evidence.mov":
                    evidence_paths.append(relative)
                artifacts.append(
                    {
                        "path": relative,
                        "sha256": sha256(b"fixture"),
                        "kind": "video",
                        "size_bytes": 7,
                    }
                )

        calls = []

        def inspect_video(
            video: Path, output: Path, _requests: object, *, scan_overview: bool
        ) -> dict[str, object]:
            calls.append(video.relative_to(run.resolve()).as_posix())
            output.mkdir(parents=True)
            (output / "overview.jpg").write_bytes(b"overview")
            (output / "change_candidates.png").write_bytes(b"packed changes")

            def cell(index: int, label: str) -> dict[str, object]:
                return {
                    "cell_index": index,
                    "cell_label": label,
                    "nominal_requested_pts_seconds": index + 0.5,
                    "source_pts_measured": False,
                    "pts_uncertainty_seconds": 0.5,
                    "pts_uncertainty_interval": {
                        "start_pts_seconds": float(index),
                        "end_pts_seconds": float(index + 1),
                    },
                }

            candidate_cells = []
            for rank_index in range(12):
                outer_row, outer_column = divmod(rank_index, 3)
                for inner_index in range(3):
                    inner_row, inner_column = divmod(inner_index, 2)
                    atlas_index = (
                        (outer_row * 2 + inner_row) * 6
                        + outer_column * 2
                        + inner_column
                    )
                    candidate_cells.append(
                        cell(
                            atlas_index,
                            f"change_rank_{rank_index + 1:02d}_cell_{inner_index + 1:03d}",
                        )
                    )
            candidate_cells.sort(key=lambda item: item["cell_index"])

            return {
                "status": "complete",
                "overview": {
                    "status": "complete",
                    "filename": "overview.jpg",
                    "purpose": "whole_video_overview",
                    "interval": {"start_pts_seconds": 0.0, "end_pts_seconds": 12.0},
                    "layout": {"columns": 1, "rows": 1, "cell_order": "row_major"},
                    "cells": [cell(0, "whole_video")],
                },
                "change_images": [
                    {
                        "status": "complete",
                        "filename": "change_candidates.png",
                        "purpose": "temporal_change_candidates",
                        "interval": {
                            "start_pts_seconds": 0.0,
                            "end_pts_seconds": 12.0,
                        },
                        "layout": {
                            "columns": 6,
                            "rows": 8,
                            "cell_order": "row_major",
                        },
                        "cells": candidate_cells,
                    }
                ],
                "requested_intervals": [],
            }

        with patch.object(investigator, "load_video_helper", return_value=inspect_video):
            video, candidates, processed, omitted = investigator.extract_video_evidence(
                run,
                artifacts,
                root / "images",
                [],
                scan_overview=True,
                pass_number=1,
                remaining_run_budget=8,
            )
        attached: list[dict[str, object]] = []
        omitted_images = investigator.append_bounded_attachments(
            attached, candidates, limit=investigator.MAX_INITIAL_IMAGES
        )
        assert_true(
            calls == evidence_paths and processed == 3 and omitted == [],
            f"hidden camera preflight consumed semantic video work: calls={calls}, video={video}",
        )
        assert_true(
            sum(item.get("error") == "hidden_auxiliary_video" for item in video) == 3,
            f"skipped preflight videos were not reported honestly: {video}",
        )
        assert_true(
            omitted_images == 0 and len(attached) == 6,
            f"packed main-video evidence still exceeded the first-pass cap: {len(attached)}",
        )
        paths = [item["manifest"]["source_video_path"] for item in attached]
        assert_true(
            paths == evidence_paths + evidence_paths,
            f"main video overview/change pairing was lost: {paths}",
        )
        atlases = [
            item["manifest"]
            for item in attached
            if item["manifest"]["purpose"] == "temporal_change_candidates"
        ]
        assert_true(
            len(atlases) == 3
            and all(len(item["cells"]) == 36 for item in atlases)
            and all(
                any(
                    cell["cell_label"].startswith("change_rank_12_")
                    for cell in item["cells"]
                )
                for item in atlases
            ),
            f"a late-ranked firmware-defect candidate was withheld: {atlases}",
        )


def test_pts_video_selector_does_not_decode_the_whole_video_for_bounds() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        run = Path(temporary)
        video = run / "long-camera.mov"
        video.write_bytes(b"fixture video")
        commands = []

        def ffprobe(command: list[str], **_kwargs: object) -> subprocess.CompletedProcess[str]:
            commands.append(command)
            stream = {"duration": "340.365", "nb_frames": "999999"}
            if "-count_frames" in command:
                stream["nb_read_frames"] = "67895"
            return subprocess.CompletedProcess(
                command,
                0,
                stdout=json.dumps(
                    {
                        "streams": [stream],
                        "format": {"duration": "340.365"},
                    }
                ),
                stderr="",
            )

        selector = {
            "kind": "video",
            "path": video.name,
            "sha256": sha256(video.read_bytes()),
            "description": "Timestamp-only long-video evidence",
            "start_pts_s": 38.858337,
            "end_pts_s": 39.047429,
        }
        bounds_cache = {}
        with patch.object(investigator.subprocess, "run", side_effect=ffprobe):
            error = investigator.resolve_artifact_selector(
                run,
                selector,
                bounds_cache,
                require_video_attachment=False,
            )
            assert_true(error is None, f"timestamp-only selector decoded the whole video: {error}")
            assert_true(
                len(commands) == 1 and "-count_frames" not in commands[0],
                f"video bounds still force a full frame decode: {commands}",
            )

            frame_selector = dict(selector)
            frame_selector["start_frame"] = 67894
            frame_selector["end_frame"] = 67895
            frame_error = investigator.resolve_artifact_selector(
                run,
                frame_selector,
                bounds_cache,
                require_video_attachment=False,
            )
        assert_true(
            frame_error is not None and "frame range does not resolve" in frame_error,
            f"out-of-range frame selector bypassed lazy bounds: {frame_error}",
        )
        assert_true(
            len(commands) == 2
            and "-count_frames" in commands[1]
            and bounds_cache[video.resolve()][1] == 67895,
            f"frame selector did not upgrade cached bounds exactly once: {commands}",
        )

        failed_commands = []

        def unavailable_count(
            command: list[str], **_kwargs: object
        ) -> subprocess.CompletedProcess[str]:
            failed_commands.append(command)
            if "-count_frames" in command:
                raise subprocess.TimeoutExpired(command, 30)
            return subprocess.CompletedProcess(
                command,
                0,
                stdout=json.dumps(
                    {
                        "streams": [{"duration": "340.365", "nb_frames": "N/A"}],
                        "format": {"duration": "340.365"},
                    }
                ),
                stderr="",
            )

        failed_cache = {}
        with patch.object(investigator.subprocess, "run", side_effect=unavailable_count):
            assert_true(
                investigator.resolve_artifact_selector(
                    run,
                    selector,
                    failed_cache,
                    require_video_attachment=False,
                )
                is None,
                "failed frame counting poisoned timestamp-only bounds",
            )
            failed_errors = [
                investigator.resolve_artifact_selector(
                    run,
                    frame_selector,
                    failed_cache,
                    require_video_attachment=False,
                )
                for _index in range(2)
            ]
        assert_true(
            all("frame bounds are unavailable" in str(error) for error in failed_errors)
            and sum("-count_frames" in command for command in failed_commands) == 1,
            f"failed frame count was retried or hid usable PTS bounds: "
            f"{failed_errors}, {failed_commands}",
        )


def test_durable_attachment_manifest_controls_video_citations() -> None:
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        raise AssertionError("ffmpeg is required for investigator video tests")
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        run = root / "run"
        run.mkdir()
        video = run / "camera.mkv"
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
                "color=c=black:s=64x48:r=8:d=1",
                "-c:v",
                "ffv1",
                str(video),
            ],
            check=True,
        )
        video_hash = sha256(video.read_bytes())

        def attachment(parent: str, content: bytes, start: float, end: float) -> dict[str, object]:
            directory = root / parent
            directory.mkdir()
            image = directory / "overview.jpg"
            image.write_bytes(content)
            return {
                "file": image,
                "manifest": {
                    "pass": 1,
                    "source_video_path": "camera.mkv",
                    "source_video_sha256": video_hash,
                    "purpose": "whole_video_overview",
                    "interval": {
                        "start_pts_seconds": start,
                        "end_pts_seconds": end,
                    },
                    "layout": {"columns": 1, "rows": 1, "cell_order": "row_major"},
                    "cells": [
                        {
                            "cell_index": 0,
                            "cell_label": "cell_001",
                            "nominal_requested_pts_seconds": start,
                            "source_pts_measured": False,
                            "pts_uncertainty_seconds": end - start,
                            "pts_uncertainty_interval": {
                                "start_pts_seconds": start,
                                "end_pts_seconds": end,
                            },
                        }
                    ],
                },
            }

        manifest = investigator.persist_attachment_manifest(
            run,
            [
                attachment("first", b"first-sheet", 0.1, 0.4),
                attachment("second", b"second-sheet", 0.5, 0.8),
            ],
        )
        by_index = {item["attachment_index"]: item for item in manifest}
        assert_true(
            manifest[0]["sheet_path"] != manifest[1]["sheet_path"]
            and all((run / item["sheet_path"]).is_file() for item in manifest)
            and all("filename" not in item for item in manifest),
            "duplicate temporary names were not replaced by durable content identity",
        )
        assert_true(
            [item["path"] for item in investigator.discover_artifacts(run)] == ["camera.mkv"],
            "derived sheets leaked into the raw artifact inventory",
        )

        selector: dict[str, object] = {
            "kind": "video",
            "path": "camera.mkv",
            "sha256": video_hash,
            "description": "The first supplied cell",
            "start_pts_s": 0.1,
            "end_pts_s": 0.4,
            "attachment_index": 1,
            "cell_indices": [0],
        }
        assert_true(
            investigator.resolve_artifact_selector(run, selector, {}, by_index) is None,
            "attached video cell did not resolve",
        )
        unattached = dict(selector)
        unattached["start_pts_s"], unattached["end_pts_s"] = 0.5, 0.6
        assert_true(
            "outside attached sheet"
            in str(investigator.resolve_artifact_selector(run, unattached, {}, by_index)),
            "in-bounds but unattached video interval resolved",
        )
        narrowed = dict(selector)
        narrowed["start_pts_s"], narrowed["end_pts_s"] = 0.2, 0.2
        assert_true(
            "narrows attached PTS uncertainty"
            in str(investigator.resolve_artifact_selector(run, narrowed, {}, by_index)),
            "nominal cell timestamp was treated as exact",
        )
        missing_attachment = dict(selector)
        del missing_attachment["attachment_index"]
        assert_true(
            "video attachment does not resolve"
            in str(
                investigator.resolve_artifact_selector(
                    run, missing_attachment, {}, by_index
                )
            ),
            "video citation without an attached sheet resolved",
        )
        sheet_as_file = {
            "kind": "file",
            "path": manifest[0]["sheet_path"],
            "sha256": manifest[0]["sheet_sha256"],
            "description": "Attempt to bypass video cell grounding",
        }
        assert_true(
            "requires a video selector"
            in str(investigator.resolve_artifact_selector(run, sheet_as_file)),
            "derived sheet bypassed source-video and cell grounding",
        )
        raw_video_as_file = {
            "kind": "file",
            "path": "camera.mkv",
            "sha256": video_hash,
            "description": "Attempt to bypass attached visual evidence",
        }
        assert_true(
            "requires an attached video selector"
            in str(investigator.resolve_artifact_selector(run, raw_video_as_file)),
            "raw video bytes bypassed attached-cell grounding",
        )

        report = minimal_model_report(
            {
                "kind": "file",
                "path": "camera.mkv",
                "sha256": video_hash,
                "description": "Fixture video bytes",
            }
        )
        report["findings"] = []
        unsampled = dict(missing_attachment)
        del unsampled["cell_indices"]
        report["coverage"]["video_intervals"] = [
            {"status": "reviewed", "selector": dict(selector), "notes": []},
            {"status": "reviewed", "selector": dict(missing_attachment), "notes": []},
            {
                "status": "partially_reviewed",
                "selector": dict(missing_attachment),
                "notes": [],
            },
            {"status": "unsampled", "selector": unsampled, "notes": []},
        ]
        finished = investigator.finish_report(
            report,
            run_dir=run,
            source_context_value=source_precheck(),
            model="fixture-model",
            backend="fixture-backend",
            tool_version="fixture-tool",
            prompt="fixture prompt",
            extra_errors=[],
            attachment_manifest=manifest,
        )
        assert_true(
            finished["coverage"]["attachments"] == manifest
            and investigator.validate_report_schema(finished) == [],
            "runner-owned attachment manifest was not retained schema-validly",
        )
        assert_true(
            [item["status"] for item in finished["coverage"]["video_intervals"]]
            == ["reviewed", "unsampled"],
            "unattached reviewed or partially reviewed video coverage survived",
        )
        assert_true(
            sum(
                "video_coverage_omitted" in error
                for error in finished["execution_status"]["errors"]
            )
            == 2,
            "reviewed video coverage omissions were not explicit",
        )


def test_backend_failure_still_publishes_honest_report() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        run = root / "run"
        run.mkdir()
        (run / "partial.log").write_text("partial evidence\n", encoding="utf-8")
        backend = root / "fake-codex"
        failure_device = "/dev/cu." + "failure"
        failure_home = "/Users/" + "failure-person/data"
        backend.write_text(
            "#!/bin/sh\n"
            "if [ \"$1\" = \"--version\" ]; then echo fake-codex; exit 0; fi\n"
            f"echo \"backend unavailable run={run} AA:BB:CC:DD:EE:FF "
            f"ssid='FailureNetwork' profile='FailureProfile' {failure_device} "
            f"{failure_home}\" >&2\n"
            "echo \"Starting BLE scan for V1 (proxy: fixture, name: FailureDeviceName)\" >&2\n"
            "exit 9\n",
            encoding="utf-8",
        )
        os.chmod(backend, 0o700)
        process = subprocess.run(
            [
                sys.executable,
                str(ROOT / "tools" / "bench_investigate.py"),
                str(run),
                "--codex-executable",
                str(backend),
                "--max-video-passes",
                "1",
            ],
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        report = json.loads((run / "investigation.json").read_text(encoding="utf-8"))
        assert_true(process.returncode == 2, f"backend failure exit changed: {process.returncode}")
        assert_true(report["execution_status"]["state"] == "failed", "failure looked successful")
        assert_true(report["schema_version"] == 2, "failure report used the old contract")
        assert_true(not report["findings"], "backend failure fabricated findings")
        assert_true(
            report["coverage"]["artifacts"][0]["sha256"]
            == sha256(b"partial evidence\n"),
            "failure report lost the runner-owned artifact digest",
        )
        assert_true(
            report["model"]["backend"] == "codex exec --oss --local-provider ollama"
            and report["model"]["name"] == "qwen3-vl:8b",
            f"automatic backend crossed the local boundary: {report['model']}",
        )
        assert_true(
            {item["path"] for item in report["model"]["instruction_hashes"]}
            == {
                "tools/bench_investigator_prompt.md",
                "tools/bench_investigation.schema.json",
            },
            "failure report attributed unloaded repository instructions",
        )
        assert_true(
            "Reason: The investigation backend did not produce usable results."
            in process.stderr
            and "Detail: backend_failed:" in process.stderr,
            "CLI hid the recorded backend failure reason",
        )
        assert_true((run / "partial.log").read_text() == "partial evidence\n", "input was changed")
        serialized = json.dumps(report)
        console = process.stdout + process.stderr
        assert_true(str(run) not in serialized, "failure report leaked the private run path")
        assert_true("/var/folders/" not in serialized, "failure report leaked a temporary path")
        assert_true("/private/var/folders/" not in serialized, "failure report leaked a temporary path")
        for private_value in (
            "AA:BB:CC:DD:EE:FF",
            "FailureNetwork",
            "FailureProfile",
            "FailureDeviceName",
            failure_device,
            "/Users/" + "failure-person",
        ):
            assert_true(private_value not in serialized, f"failure report leaked {private_value}")
            assert_true(private_value not in console, f"failure console leaked {private_value}")


def test_missing_backend_does_not_publish_executable_path() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        run = root / "run"
        run.mkdir()
        private_executable = root / "private" / "missing-codex"
        process = subprocess.run(
            [
                sys.executable,
                str(ROOT / "tools" / "bench_investigate.py"),
                str(run),
                "--codex-executable",
                str(private_executable),
                "--max-video-passes",
                "1",
            ],
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        report = json.loads((run / "investigation.json").read_text(encoding="utf-8"))
        serialized = json.dumps(report)
        assert_true(process.returncode == 2, "missing backend exit changed")
        assert_true("backend_missing: Codex executable is unavailable" in serialized, "missing backend was not identified")
        assert_true(str(private_executable) not in serialized, "missing backend leaked its configured path")


def test_success_report_is_recursively_redacted_and_debug_is_structured() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        run = root / "run"
        run.mkdir()
        artifact_bytes = next(
            payload
            for index in range(10000)
            if "ab" in sha256(payload := f"fixture-{index}\n".encode("utf-8"))
        )
        artifact_sha256 = sha256(artifact_bytes)
        (run / "artifact.log").write_bytes(artifact_bytes)
        backend = root / "fake-codex"
        selector = {
            "kind": "file",
            "path": "artifact.log",
            "sha256": artifact_sha256,
            "description": "fixture",
        }
        template = minimal_model_report(selector)
        template["findings"] = []
        template["source"]["basis"] = "current_only"
        template["coverage"]["artifacts"] = [
            {
                "path": "artifact.log",
                "status": "reviewed",
                "sha256": artifact_sha256,
                "size_bytes": len(artifact_bytes),
                "role": "fixture",
                "selectors": [selector],
                "notes": [],
            }
        ]
        blocklisted_term = "Unlabelled" + "PrivateTerm"
        terms_path = root / "privacy-terms.txt"
        terms_path.write_text(
            f"# local fixture\nab\n{blocklisted_term.lower()}\n",
            encoding="utf-8",
        )
        private_home = "/Users/" + "private-person/project"
        private_device = "/dev/cu." + "private"
        private_summary = (
            f"private {run} {ROOT} {Path.home()} {private_home} "
            "AA:BB:CC:DD:EE:FF ssid='PrivateNetwork' profile='PrivateProfile' "
            f"{private_device} Network {blocklisted_term} failed "
        )
        backend.write_text(
            "#!/usr/bin/env python3\n"
            "import json,sys\n"
            "from pathlib import Path\n"
            "output=Path(sys.argv[sys.argv.index('--output-last-message')+1])\n"
            f"report=json.loads({json.dumps(json.dumps(template))})\n"
            f"report['source']['summary']={private_summary!r}+str(output.parent)+'\\nStarting BLE scan for V1 (proxy: fixture, name: PrivateDeviceName)'\n"
            "print('RAW_MODEL_TRANSCRIPT '+str(output.parent))\n"
            "print('RAW_MODEL_STDERR '+str(output.parent),file=sys.stderr)\n"
            "output.write_text(json.dumps(report))\n",
            encoding="utf-8",
        )
        os.chmod(backend, 0o700)
        process = subprocess.run(
            [
                sys.executable,
                str(ROOT / "tools" / "bench_investigate.py"),
                str(run),
                "--codex-executable",
                str(backend),
                "--max-video-passes",
                "1",
                "--debug-transcript",
            ],
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
            env={**os.environ, "V1SIMPLE_PRIVACY_TERMS": str(terms_path)},
        )
        assert_true(process.returncode == 0, f"successful redaction fixture failed: {process.stderr}")
        report_text = (run / "investigation.json").read_text(encoding="utf-8")
        published = json.loads(report_text)
        debug = json.loads((run / "investigation_debug.json").read_text(encoding="utf-8"))
        assert_true(
            {item["path"] for item in published["model"]["instruction_hashes"]}
            == {
                "tools/bench_investigator_prompt.md",
                "tools/bench_investigation.schema.json",
            },
            "success report attributed unloaded repository instructions",
        )
        for private in (str(run), str(ROOT), str(Path.home()), str(root)):
            assert_true(private not in report_text, f"successful report leaked {private}")
        assert_true(
            "<run>" in report_text and "<repo>" in report_text,
            f"redaction markers were lost: {json.loads(report_text)['source']['summary']}",
        )
        for private_value in (
            "AA:BB:CC:DD:EE:FF",
            "PrivateNetwork",
            "PrivateProfile",
            "PrivateDeviceName",
            blocklisted_term,
            private_device,
            "/Users/" + "private-person",
        ):
            assert_true(private_value not in report_text, f"model output leaked {private_value}")
        for marker in (
            "<redacted-mac>",
            "<redacted-network>",
            "<redacted-profile>",
            "<redacted-name>",
            "<redacted-private-term>",
            "/dev/" + "<redacted-device>",
            "/Users/" + "<redacted-user>",
        ):
            assert_true(marker in report_text, f"shared privacy marker was lost: {marker}")
        published = json.loads(report_text)
        assert_true(
            published["coverage"]["artifacts"][0]["selectors"] == [selector],
            "recursive artifact sanitization changed a resolvable selector",
        )
        debug_text = json.dumps(debug)
        assert_true("RAW_MODEL_TRANSCRIPT" not in debug_text, "debug retained raw stdout")
        assert_true("RAW_MODEL_STDERR" not in debug_text, "debug retained raw stderr")
        event = debug["passes"][0]
        assert_true("stdout" not in event and "stderr" not in event, "debug retained transcript fields")
        assert_true(event["stdout_bytes"] > 0 and event["stderr_bytes"] > 0, "structured diagnostics were lost")


def test_fresh_source_precheck_limits_final_attribution() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        run = Path(temporary) / "run"
        run.mkdir()
        report = minimal_model_report(
            {
                "kind": "file",
                "path": "missing-is-unused",
                "sha256": "0" * 64,
                "description": "unused fixture",
            }
        )
        report["findings"] = []
        calls = 0
        order: list[str] = []

        def changing_source(_run: Path, _artifacts: object) -> dict[str, object]:
            nonlocal calls
            calls += 1
            order.append("source")
            return source_precheck("exact" if calls == 1 else "current_only")

        def model_pass(**_kwargs: object) -> tuple[dict[str, object], str, str]:
            order.append("model")
            return report, "", ""

        argv = ["bench_investigate.py", str(run), "--codex-executable", "fixture", "--max-video-passes", "1"]
        with (
            patch.object(sys, "argv", argv),
            patch.object(investigator, "source_context", side_effect=changing_source),
            patch.object(
                investigator,
                "extract_video_evidence",
                return_value=(
                    [{"path": "omitted.mp4", "status": "not_processed", "error": "video_limit"}],
                    [],
                    0,
                    ["omitted.mp4"],
                ),
            ) as extract_video,
            patch.object(investigator, "invoke_codex", side_effect=model_pass),
            patch.object(investigator, "codex_version", return_value="fixture"),
            redirect_stdout(io.StringIO()),
        ):
            result = investigator.main()
        finished = json.loads((run / "investigation.json").read_text(encoding="utf-8"))
        assert_true(
            result == 0
            and order == ["source", "model", "model", "source"]
            and extract_video.call_count == 1,
            f"source was not refreshed after model execution: {order}",
        )
        assert_true(finished["source"]["basis"] == "current_only", "stale exact source survived")
        assert_true(finished["execution_status"]["state"] == "partial", "source race looked complete")
        assert_true(
            any("video_file_limit" in error and "omitted.mp4" in error for error in finished["execution_status"]["errors"]),
            "pre-extraction video omission was not retained in the report",
        )


def test_unexpected_postprocessing_error_publishes_valid_sanitized_failure() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        run = root / "run"
        run.mkdir()
        report = minimal_model_report(
            {
                "kind": "file",
                "path": "unused",
                "sha256": "0" * 64,
                "description": "unused fixture",
            }
        )
        report["findings"] = []

        def explode(*_args: object, **_kwargs: object) -> dict[str, object]:
            raise RuntimeError(f"private {run} {ROOT} {Path.home()} {tempfile.gettempdir()}")

        argv = ["bench_investigate.py", str(run), "--codex-executable", "fixture", "--max-video-passes", "1"]
        with (
            patch.object(sys, "argv", argv),
            patch.object(investigator, "source_context", return_value=source_precheck("current_only")),
            patch.object(investigator, "extract_video_evidence", return_value=([], [], 0, [])),
            patch.object(investigator, "invoke_codex", return_value=(report, "", "")),
            patch.object(investigator, "finish_report", side_effect=explode),
            patch.object(investigator, "codex_version", return_value="fixture"),
            redirect_stdout(io.StringIO()),
            redirect_stderr(io.StringIO()),
        ):
            result = investigator.main()
        failure = json.loads((run / "investigation.json").read_text(encoding="utf-8"))
        serialized = json.dumps(failure)
        assert_true(result == 2 and failure["execution_status"]["state"] == "failed", "unexpected error escaped")
        assert_true(investigator.validate_report_schema(failure) == [], "unexpected-error report is invalid")
        for private in (str(run), str(ROOT), str(Path.home()), tempfile.gettempdir()):
            assert_true(private not in serialized, f"unexpected-error report leaked {private}")


def test_no_video_request_still_runs_synthesis_pass() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        run = Path(temporary) / "run"
        run.mkdir()
        report = minimal_model_report(
            {
                "kind": "file",
                "path": "unused",
                "sha256": "0" * 64,
                "description": "unused fixture",
            }
        )
        report["findings"] = []
        prompts: list[str] = []

        def model_pass(**kwargs: object) -> tuple[dict[str, object], str, str]:
            prompts.append(str(kwargs["prompt"]))
            report["execution_status"]["summary"] = f"model pass {len(prompts)}"
            return report, "", ""

        argv = [
            "bench_investigate.py",
            str(run),
            "--codex-executable",
            "fixture",
            "--max-video-passes",
            "1",
        ]
        with (
            patch.object(sys, "argv", argv),
            patch.object(investigator, "source_context", return_value=source_precheck()),
            patch.object(
                investigator,
                "extract_video_evidence",
                return_value=([], [], 0, []),
            ) as extract_video,
            patch.object(investigator, "invoke_codex", side_effect=model_pass),
            patch.object(investigator, "codex_version", return_value="fixture"),
            redirect_stdout(io.StringIO()),
        ):
            result = investigator.main()

        finished = json.loads((run / "investigation.json").read_text(encoding="utf-8"))
        assert_true(
            result == 0 and len(prompts) == 2 and extract_video.call_count == 1,
            "one video pass bypassed the required synthesis model pass",
        )
        assert_true(
            "first-pass lead checkpoint" in prompts[0]
            and "synthesis pass" in prompts[1]
            and "prior_report_to_recheck_and_improve" in prompts[1],
            "second pass did not synthesize the first checkpoint",
        )
        assert_true(
            finished["execution_status"]["summary"].startswith("model pass 2")
            and finished["model"]["prompt_sha256"]
            == investigator.sha256_bytes(prompts[1].encode("utf-8")),
            "published report did not come from the synthesis pass",
        )


def test_unserved_video_request_remains_a_limitation() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        run = Path(temporary) / "run"
        run.mkdir()
        selector = {
            "kind": "file",
            "path": "unused",
            "sha256": "0" * 64,
            "description": "unused fixture",
        }
        checkpoint = minimal_model_report(selector)
        checkpoint["findings"] = []
        checkpoint["video_requests"] = [
            {
                "path": "camera.mp4",
                "start_pts_s": 1.0,
                "end_pts_s": 2.0,
                "sample_fps": 10.0,
                "reason": "Inspect the display transition",
            }
        ]
        synthesis = minimal_model_report(selector)
        synthesis["findings"] = []
        synthesis["video_requests"] = []
        prompts: list[str] = []

        def model_pass(**kwargs: object) -> tuple[dict[str, object], str, str]:
            prompts.append(str(kwargs["prompt"]))
            return (checkpoint if len(prompts) == 1 else synthesis), "", ""

        argv = [
            "bench_investigate.py",
            str(run),
            "--codex-executable",
            "fixture",
            "--max-video-passes",
            "1",
        ]
        with (
            patch.object(sys, "argv", argv),
            patch.object(investigator, "source_context", return_value=source_precheck()),
            patch.object(
                investigator,
                "extract_video_evidence",
                return_value=([], [], 0, []),
            ) as extract_video,
            patch.object(investigator, "invoke_codex", side_effect=model_pass),
            patch.object(investigator, "codex_version", return_value="fixture"),
            redirect_stdout(io.StringIO()),
        ):
            result = investigator.main()

        finished = json.loads((run / "investigation.json").read_text(encoding="utf-8"))
        errors = "\n".join(finished["execution_status"]["errors"])
        assert_true(
            result == 0
            and len(prompts) == 2
            and extract_video.call_count == 1
            and finished["execution_status"]["state"] == "partial",
            "unserved video request did not leave an honest partial investigation",
        )
        assert_true(
            "video_request_limit" in errors
            and "1 video request(s) from pass 1 were not extracted" in errors,
            "synthesis erased the unserved first-pass video request",
        )


def test_failed_video_followup_preserves_prior_grounded_report() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        run = Path(temporary) / "run"
        run.mkdir()
        content = b"grounded first pass\n"
        (run / "evidence.log").write_bytes(content)
        selector: dict[str, object] = {
            "kind": "log",
            "path": "evidence.log",
            "sha256": sha256(content),
            "description": "Grounded first-pass line",
            "line_start": 1,
            "line_end": 1,
        }
        prior = minimal_model_report(selector)
        prior["video_requests"] = [
            {
                "path": "camera.mkv",
                "start_pts_s": 0.1,
                "end_pts_s": 0.4,
                "sample_fps": 8.0,
                "reason": "Inspect one bounded interval",
            }
        ]
        prompts: list[str] = []

        def two_pass_backend(**kwargs: object) -> tuple[dict[str, object], str, str]:
            prompts.append(str(kwargs["prompt"]))
            if len(prompts) == 1:
                return prior, "first pass", ""
            raise investigator.InvestigationError(
                "backend_timeout", f"follow-up timed out in {run}"
            )

        argv = [
            "bench_investigate.py",
            str(run),
            "--codex-executable",
            "fixture",
            "--max-video-passes",
            "2",
        ]
        with (
            patch.object(sys, "argv", argv),
            patch.object(
                investigator,
                "source_context",
                return_value=source_precheck(),
            ),
            patch.object(
                investigator,
                "extract_video_evidence",
                return_value=([], [], 0, []),
            ),
            patch.object(investigator, "invoke_codex", side_effect=two_pass_backend),
            patch.object(investigator, "codex_version", return_value="fixture"),
            redirect_stdout(io.StringIO()),
        ):
            result = investigator.main()

        finished = json.loads(
            (run / "investigation.json").read_text(encoding="utf-8")
        )
        errors = "\n".join(finished["execution_status"]["errors"])
        assert_true(result == 0 and len(prompts) == 2, "follow-up failure ended the usable run")
        assert_true(
            [finding["id"] for finding in finished["findings"]]
            == ["fixture-correlation"],
            "failed follow-up erased the grounded first-pass finding",
        )
        assert_true(
            finished["execution_status"]["state"] == "partial"
            and "followup_backend_failed" in errors
            and "backend_timeout" in errors,
            f"failed follow-up was not recorded as a partial limitation: {finished}",
        )
        assert_true(str(run) not in json.dumps(finished), "follow-up error leaked the run path")
        assert_true(
            finished["model"]["prompt_sha256"]
            == investigator.sha256_bytes(prompts[0].encode("utf-8")),
            "retained report was attributed to the failed follow-up prompt",
        )


def test_video_request_runs_a_fresh_follow_up() -> None:
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        raise AssertionError("ffmpeg is required for investigator video tests")
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        run = root / "run"
        run.mkdir()
        video = run / "camera.mkv"
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
                "color=c=black:s=64x48:r=8:d=1",
                "-c:v",
                "ffv1",
                str(video),
            ],
            check=True,
        )
        video_selector = {
            "kind": "video",
            "path": "camera.mkv",
            "sha256": sha256(video.read_bytes()),
            "description": "Out-of-bounds fixture interval",
            "start_pts_s": 0.8,
            "end_pts_s": 2.0,
        }
        range_error = investigator.resolve_artifact_selector(run, video_selector)
        assert_true(
            range_error is not None and "exceeds duration" in range_error,
            f"out-of-bounds video citation resolved: {range_error}",
        )
        backend = root / "fake-codex"
        state = root / "pass-count"
        template = minimal_model_report(
            {
                "kind": "file",
                "path": "camera.mkv",
                "sha256": sha256(video.read_bytes()),
                "description": "Fixture video",
            }
        )
        template["findings"] = []
        template["source"]["basis"] = "current_only"
        backend.write_text(
            "#!/usr/bin/env python3\n"
            "import json,sys\n"
            "from pathlib import Path\n"
            f"state=Path({str(state)!r})\n"
            "if len(sys.argv)>1 and sys.argv[1]=='--version': print('fake-codex'); raise SystemExit\n"
            "count=int(state.read_text())+1 if state.exists() else 1\n"
            "state.write_text(str(count))\n"
            "output=Path(sys.argv[sys.argv.index('--output-last-message')+1])\n"
            f"report=json.loads({json.dumps(json.dumps(template))})\n"
            "report['video_requests']=([{'path':'camera.mkv','start_pts_s':0.1,'end_pts_s':0.4,'sample_fps':8.0,'reason':'inspect fixture interval'}] if count==1 else [])\n"
            "output.write_text(json.dumps(report))\n",
            encoding="utf-8",
        )
        os.chmod(backend, 0o700)
        process = subprocess.run(
            [
                sys.executable,
                str(ROOT / "tools" / "bench_investigate.py"),
                str(run),
                "--codex-executable",
                str(backend),
                "--max-video-passes",
                "2",
            ],
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        report = json.loads((run / "investigation.json").read_text(encoding="utf-8"))
        assert_true(process.returncode == 0, f"follow-up failed: {process.stderr}")
        assert_true(state.read_text() == "2", "runner did not start a fresh second pass")
        assert_true(
            report["execution_status"]["state"] == "partial"
            and any(
                "artifact_coverage_missing: camera.mkv" in error
                for error in report["execution_status"]["errors"]
            ),
            f"model-omitted video coverage looked complete: {report}",
        )
        assert_true(report["video_requests"] == [], "fulfilled request remained pending")


def main() -> int:
    test_discovery_accepts_unfamiliar_and_excludes_prior_output()
    test_stat_or_hash_failure_remains_visible_as_unreadable_coverage()
    test_invalid_primary_citations_are_stripped_or_omitted()
    test_existing_unresolved_without_primary_grounding_is_omitted()
    test_source_precheck_limits_attribution_and_model_execution_state_is_preserved()
    test_code_selector_requires_exact_selected_line_hash()
    test_invalid_reviewed_coverage_becomes_runner_owned_skipped()
    test_duplicate_valid_coverage_is_unique()
    test_two_valid_coverage_selectors_are_retained_as_reviewed()
    test_cross_artifact_coverage_selector_is_removed_and_downgraded()
    test_private_artifact_path_is_redacted_and_loses_grounding()
    test_published_selector_validation_covers_all_diagnostic_owners()
    test_artifact_identity_is_runner_owned_and_nonexistent_paths_are_omitted()
    test_runner_inventory_retains_model_omissions()
    test_nested_csv_selector_keys_are_resolved()
    test_clock_mapping_failures_downgrade_only_referencing_findings()
    test_default_is_local_and_hosted_requires_explicit_choice()
    test_codex_executable_resolution_uses_override_path_and_app_bundle()
    test_provider_command_and_local_environment_do_not_fall_back()
    test_missing_run_error_does_not_print_resolved_user_path()
    test_atomic_report_replaces_old_content()
    test_raw_model_json_is_validated_before_postprocessing()
    test_codex_output_schema_requires_every_property()
    test_transport_nulls_are_removed_before_canonical_validation()
    test_codex_failure_uses_sanitized_jsonl_terminal_error()
    test_image_attachment_limit_is_global_across_passes()
    test_video_limits_apply_before_extraction_and_manifest_preserves_order()
    test_attachment_retention_is_contained_and_reconciled()
    test_model_context_summarizes_periodic_video_points()
    test_first_pass_requires_a_grounded_checkpoint_before_breadth()
    test_attachment_allocation_is_reported_honestly()
    test_first_pass_keeps_every_main_video_change_candidate_under_the_image_cap()
    test_pts_video_selector_does_not_decode_the_whole_video_for_bounds()
    test_durable_attachment_manifest_controls_video_citations()
    test_backend_failure_still_publishes_honest_report()
    test_missing_backend_does_not_publish_executable_path()
    test_success_report_is_recursively_redacted_and_debug_is_structured()
    test_fresh_source_precheck_limits_final_attribution()
    test_unexpected_postprocessing_error_publishes_valid_sanitized_failure()
    test_no_video_request_still_runs_synthesis_pass()
    test_unserved_video_request_remains_a_limitation()
    test_failed_video_followup_preserves_prior_grounded_report()
    test_video_request_runs_a_fresh_follow_up()
    print("bench investigate tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

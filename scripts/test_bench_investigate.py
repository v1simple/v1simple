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


def source_precheck(basis: str = "exact") -> dict[str, object]:
    return {
        "current_head": "fixture-revision",
        "identities": [{"suggested_basis": basis}],
    }


def minimal_model_report(evidence: dict[str, object]) -> dict[str, object]:
    return {
        "schema_version": 1,
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
                        "line_start": 42,
                        "line_end": 47,
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


def test_unresolved_primary_citations_are_preserved_as_unknown() -> None:
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
            == ["fixture-correlation", "code-unresolved"],
            "leads with unresolved evidence or code were not preserved as unknown",
        )
        assert_true(selector["sha256"] == "0" * 64, "runner rewrote the model's bad hash")
        assert_true(
            all(
                "Primary citation resolution failed" in item["why_unknown"]
                for item in report["unresolved"]
            ),
            "citation limitations were hidden",
        )
        errors = "\n".join(report["execution_status"]["errors"])
        assert_true(
            "finding_moved_unresolved: fixture-correlation" in errors,
            "unresolved artifact lead conversion was hidden",
        )
        assert_true("finding_moved_unresolved: code-unresolved" in errors, "unknown conversion was hidden")
        assert_true("finding_omitted:" not in errors, "schema-valid lead was discarded")
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
            and default.model == "qwen3-vl:8b",
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
        hosted_argv = json.loads(capture.read_text(encoding="utf-8"))["argv"]
        assert_true("--oss" not in hosted_argv, "explicit hosted invocation stayed local")
        assert_true("--local-provider" not in hosted_argv, "hosted invocation retained a local provider")
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
        hosted_model_index = hosted_argv.index("--model")
        assert_true(hosted_argv[hosted_model_index + 1] == "gpt-5.6-sol", "hosted model changed")


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
            artifacts.append({"path": path.name, "kind": "video", "size_bytes": 7})
        calls: list[str] = []

        def inspect_video(video: Path, output: Path, _requests: object, *, scan_overview: bool) -> dict[str, object]:
            calls.append(video.name)
            output.mkdir(parents=True)
            for name in ("overview.jpg", "change.jpg", "interval.jpg"):
                (output / name).write_bytes(name.encode())

            def sheet(filename: str, purpose: str) -> dict[str, object]:
                return {"status": "complete", "filename": filename, "purpose": purpose}

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
        manifest = investigator.ordered_attachment_manifest(candidates[:3])
        assert_true([item["attachment_index"] for item in manifest] == [1, 2, 3], "manifest is not ordered")
        assert_true(all(item["pass"] == 2 for item in manifest), "manifest lost its pass")
        selected, per_pass_omitted = investigator.bounded_video_paths(
            [f"video-{index}" for index in range(20)], 100
        )
        assert_true(len(selected) == investigator.MAX_VIDEOS_PER_PASS, "per-pass video bound changed")
        assert_true(len(per_pass_omitted) == 12, "per-pass omissions were lost")


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


def test_attachment_cap_represents_each_video_before_extra_sheets() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        run = root / "run"
        run.mkdir()
        artifacts = []
        for name in ("preflight.mov", "replay.mov"):
            (run / name).write_bytes(b"fixture")
            artifacts.append({"path": name, "kind": "video", "size_bytes": 7})

        def inspect_video(
            video: Path, output: Path, _requests: object, *, scan_overview: bool
        ) -> dict[str, object]:
            output.mkdir(parents=True)
            (output / "overview.jpg").write_bytes(b"overview")
            changes = []
            for index in range(12):
                filename = f"change-{index:02d}.jpg"
                (output / filename).write_bytes(filename.encode())
                changes.append(
                    {
                        "status": "complete",
                        "filename": filename,
                        "purpose": "temporal_change_candidate",
                    }
                )
            return {
                "status": "complete",
                "overview": {
                    "status": "complete",
                    "filename": "overview.jpg",
                    "purpose": "whole_video_overview",
                },
                "change_images": changes,
                "requested_intervals": [],
            }

        with patch.object(investigator, "load_video_helper", return_value=inspect_video):
            _video, candidates, _processed, _omitted = investigator.extract_video_evidence(
                run,
                artifacts,
                root / "images",
                [],
                scan_overview=True,
                pass_number=1,
                remaining_run_budget=2,
            )
        attached: list[dict[str, object]] = []
        investigator.append_bounded_attachments(
            attached, candidates, limit=investigator.MAX_INITIAL_IMAGES
        )
        paths = [item["manifest"]["video_path"] for item in attached]
        assert_true(paths[:2] == ["preflight.mov", "replay.mov"], "overview order changed")
        assert_true(
            paths[2:] == ["preflight.mov", "replay.mov"] * 3,
            f"one video starved the bounded change-sheet selection: {paths}",
        )


def test_backend_failure_still_publishes_honest_report() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        run = root / "run"
        run.mkdir()
        (run / "partial.log").write_text("partial evidence\n", encoding="utf-8")
        backend = root / "fake-codex"
        backend.write_text(
            "#!/bin/sh\n"
            "if [ \"$1\" = \"--version\" ]; then echo fake-codex; exit 0; fi\n"
            f"echo backend unavailable run={run} argv=\"$*\" >&2\n"
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
        assert_true(not report["findings"], "backend failure fabricated findings")
        assert_true(
            report["model"]["backend"] == "codex exec --oss --local-provider ollama"
            and report["model"]["name"] == "qwen3-vl:8b",
            f"automatic backend crossed the local boundary: {report['model']}",
        )
        assert_true((run / "partial.log").read_text() == "partial evidence\n", "input was changed")
        serialized = json.dumps(report)
        assert_true(str(run) not in serialized, "failure report leaked the private run path")
        assert_true("/var/folders/" not in serialized, "failure report leaked a temporary path")
        assert_true("/private/var/folders/" not in serialized, "failure report leaked a temporary path")


def test_success_report_is_recursively_redacted_and_debug_is_structured() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        run = root / "run"
        run.mkdir()
        (run / "artifact.log").write_text("fixture\n", encoding="utf-8")
        backend = root / "fake-codex"
        template = minimal_model_report(
            {
                "kind": "file",
                "path": "artifact.log",
                "sha256": sha256(b"fixture\n"),
                "description": "fixture",
            }
        )
        template["findings"] = []
        template["source"]["basis"] = "current_only"
        backend.write_text(
            "#!/usr/bin/env python3\n"
            "import json,sys\n"
            "from pathlib import Path\n"
            "output=Path(sys.argv[sys.argv.index('--output-last-message')+1])\n"
            f"report=json.loads({json.dumps(json.dumps(template))})\n"
            f"report['source']['summary']='private {run} {ROOT} {Path.home()} '+str(output.parent)\n"
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
        )
        assert_true(process.returncode == 0, f"successful redaction fixture failed: {process.stderr}")
        report_text = (run / "investigation.json").read_text(encoding="utf-8")
        debug = json.loads((run / "investigation_debug.json").read_text(encoding="utf-8"))
        for private in (str(run), str(ROOT), str(Path.home()), str(root)):
            assert_true(private not in report_text, f"successful report leaked {private}")
        assert_true(
            "<run>" in report_text and "<repo>" in report_text,
            f"redaction markers were lost: {json.loads(report_text)['source']['summary']}",
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
            ),
            patch.object(investigator, "invoke_codex", side_effect=model_pass),
            patch.object(investigator, "codex_version", return_value="fixture"),
            redirect_stdout(io.StringIO()),
        ):
            result = investigator.main()
        finished = json.loads((run / "investigation.json").read_text(encoding="utf-8"))
        assert_true(
            result == 0 and order == ["source", "model", "source"],
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
        ):
            result = investigator.main()
        failure = json.loads((run / "investigation.json").read_text(encoding="utf-8"))
        serialized = json.dumps(failure)
        assert_true(result == 2 and failure["execution_status"]["state"] == "failed", "unexpected error escaped")
        assert_true(investigator.validate_report_schema(failure) == [], "unexpected-error report is invalid")
        for private in (str(run), str(ROOT), str(Path.home()), tempfile.gettempdir()):
            assert_true(private not in serialized, f"unexpected-error report leaked {private}")


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
        assert_true(report["execution_status"]["state"] == "completed", f"wrong state: {report}")
        assert_true(report["video_requests"] == [], "fulfilled request remained pending")


def main() -> int:
    test_discovery_accepts_unfamiliar_and_excludes_prior_output()
    test_unresolved_primary_citations_are_preserved_as_unknown()
    test_source_precheck_limits_attribution_and_model_execution_state_is_preserved()
    test_nested_csv_selector_keys_are_resolved()
    test_clock_mapping_failures_downgrade_only_referencing_findings()
    test_default_is_local_and_hosted_requires_explicit_choice()
    test_provider_command_and_local_environment_do_not_fall_back()
    test_atomic_report_replaces_old_content()
    test_raw_model_json_is_validated_before_postprocessing()
    test_image_attachment_limit_is_global_across_passes()
    test_video_limits_apply_before_extraction_and_manifest_preserves_order()
    test_model_context_summarizes_periodic_video_points()
    test_attachment_allocation_is_reported_honestly()
    test_attachment_cap_represents_each_video_before_extra_sheets()
    test_backend_failure_still_publishes_honest_report()
    test_success_report_is_recursively_redacted_and_debug_is_structured()
    test_fresh_source_precheck_limits_final_attribution()
    test_unexpected_postprocessing_error_publishes_valid_sanitized_failure()
    test_video_request_runs_a_fresh_follow_up()
    print("bench investigate tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Focused regression tests for behavior identities and bench baseline ownership."""

from __future__ import annotations

import hashlib
import json
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts" / "bench"))

from bench_identity import (  # noqa: E402
    baseline_directory,
    build_identity_manifest,
    canonical_bytes,
    write_identity_manifest,
)


TRACE_A = {
    "repository_sha": "1" * 40,
    "repository_ref": "main",
    "worktree_clean": True,
}
TRACE_B = {
    "repository_sha": "2" * 40,
    "repository_ref": "feature/camera",
    "worktree_clean": False,
}


def assert_true(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def write_fixture(root: Path) -> None:
    files = {
        "README.md": b"documentation\n",
        "src/main.cpp": b"firmware\n",
        "include/config.h": b"configuration header\n",
        "platformio.ini": b"platform pin\n",
        "partitions_v1.csv": b"partitions\n",
        "build.sh": b"build\n",
        "scripts/get_git_sha.py": b"build metadata\n",
        "interface/package-lock.json": b"{}\n",
        "interface/src/app.html": b"<main>source</main>\n",
        "interface/src/app.test.js": b"test-only source\n",
        "interface/static/branding/logo.png": b"source logo\n",
        "config/audio_asset_manifest.json": b"{}\n",
        "tools/freq_audio/mulaw/alert.mul": b"audio source\n",
        "data/index.html": b"<main>deployed</main>\n",
        "data/audio/alert.mul": b"deployed audio\n",
        "data/branding/logo.png": b"deployed logo\n",
        "tools/v1replay/Package.swift": b"package\n",
        "tools/v1replay/Sources/v1replay/main.swift": b"replay\n",
        "tools/v1replay/Resources/Info.plist": b"plist\n",
        "tools/v1replay/scripts/build.sh": b"swift build\n",
        "scripts/bench/camera_capture.py": b"capture\n",
        "scripts/bench/camera_recorder.swift": b"native recorder\n",
        "scripts/bench/camera_preflight.py": b"preflight\n",
        "scripts/bench/bench_identity.py": b"identity contract\n",
        "scripts/bench/camera_contract.py": b"contract\n",
        "scripts/bench/camera_artifacts.py": b"artifact ownership\n",
        "scripts/bench/camera_grade.py": b"grader\n",
        "scripts/bench/camera_regrade.py": b"regrader\n",
        "scripts/bench/run_window.py": b"camera window integration\n",
        "scripts/bench/bench_policy.py": b"qualification policy\n",
        "tools/bench_score.py": b"camera scoring integration\n",
        "bench.sh": b"camera gate entrypoint\n",
    }
    for relative, content in files.items():
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)


def identity(root: Path, *, duration: int = 300, trace: dict = TRACE_A) -> dict:
    return build_identity_manifest(
        root,
        suite="core",
        duration_seconds=duration,
        profile="drive_wifi_off",
        segment="last",
        traceability=trace,
    )


def test_traceability_and_docs_do_not_change_behavior_identities(root: Path) -> None:
    first = identity(root, trace=TRACE_A)
    (root / "README.md").write_text("new documentation only\n", encoding="utf-8")
    (root / "interface/src/app.test.js").write_text("new test only\n", encoding="utf-8")
    second = identity(root, trace=TRACE_B)
    assert_true(first["product_fingerprint"] == second["product_fingerprint"], "non-product edit changed product")
    assert_true(first["grader_fingerprint"] == second["grader_fingerprint"], "non-grader edit changed grader")
    assert_true(first["traceability"] != second["traceability"], "traceability was not retained separately")


def test_product_inputs_change_only_product_identity(root: Path) -> None:
    for relative in (
        "src/main.cpp",
        "platformio.ini",
        "interface/src/app.html",
        "interface/static/branding/logo.png",
        "data/audio/alert.mul",
        "data/branding/logo.png",
        "tools/v1replay/Sources/v1replay/main.swift",
    ):
        before = identity(root)
        path = root / relative
        original = path.read_bytes()
        path.write_bytes(original + b"dirty relevant edit\n")
        after = identity(root)
        path.write_bytes(original)
        assert_true(before["product_fingerprint"] != after["product_fingerprint"], f"missed {relative}")
        assert_true(before["grader_fingerprint"] == after["grader_fingerprint"], f"{relative} changed grader")


def test_generated_deployed_html_does_not_change_product_identity(root: Path) -> None:
    path = root / "data/index.html"
    original = path.read_bytes()
    try:
        first = identity(root)
        path.write_bytes(
            b'<script>__sveltekit_new = {};</script><script src="/_app/immutable/new-entry.js"></script>\n'
        )
        second = identity(root)
    finally:
        path.write_bytes(original)

    assert_true(first["product"] == second["product"], "generated web output changed product identity")
    assert_true(first["grader_fingerprint"] == second["grader_fingerprint"], "generated web output changed grader")


def test_grader_inputs_change_only_grader_identity(root: Path) -> None:
    for relative in (
        "scripts/bench/camera_capture.py",
        "scripts/bench/camera_recorder.swift",
        "scripts/bench/camera_preflight.py",
        "scripts/bench/bench_identity.py",
        "scripts/bench/camera_contract.py",
        "scripts/bench/camera_artifacts.py",
        "scripts/bench/camera_grade.py",
        "scripts/bench/camera_regrade.py",
        "scripts/bench/run_window.py",
        "scripts/bench/bench_policy.py",
        "tools/bench_score.py",
        "bench.sh",
    ):
        before = identity(root)
        path = root / relative
        original = path.read_bytes()
        path.write_bytes(original + b"dirty grader edit\n")
        after = identity(root)
        path.write_bytes(original)
        assert_true(before["grader_fingerprint"] != after["grader_fingerprint"], f"missed {relative}")
        assert_true(before["product_fingerprint"] == after["product_fingerprint"], f"{relative} changed product")


def test_canonical_manifest_is_stable_and_repo_relative(root: Path) -> None:
    first = identity(root)
    second = identity(root)
    assert_true(first == second, "identical inputs did not produce identical manifest data")
    assert_true(canonical_bytes(first) == canonical_bytes(second), "canonical serialization was unstable")
    for behavior_kind in ("product", "grader"):
        for component in first[behavior_kind]["components"].values():
            for file_entry in component["files"]:
                assert_true(not Path(file_entry["path"]).is_absolute(), f"absolute path leaked: {file_entry}")
    assert_true(str(root) not in json.dumps(first, sort_keys=True), "machine-specific root leaked into manifest")


def test_dirty_relevant_content_digest_is_recorded(root: Path) -> None:
    path = root / "include/config.h"
    dirty_content = b"uncommitted configuration\n"
    path.write_bytes(dirty_content)
    manifest = identity(root)
    entries = [
        entry
        for component in manifest["product"]["components"].values()
        for entry in component["files"]
        if entry["path"] == "include/config.h"
    ]
    assert_true(len(entries) == 1, f"dirty file was not listed exactly once: {entries}")
    assert_true(entries[0]["sha256"] == hashlib.sha256(dirty_content).hexdigest(), "dirty bytes were not hashed")


def test_scenario_key_and_legacy_baseline_are_isolated(root: Path) -> None:
    baseline_root = root / "baselines"
    current = identity(root, duration=300)
    other_duration = identity(root, duration=301)
    assert_true(current["product_fingerprint"] == other_duration["product_fingerprint"], "scenario changed product")
    assert_true(current["scenario_fingerprint"] != other_duration["scenario_fingerprint"], "duration was ignored")

    legacy = baseline_root / "release" / "core" / "manifest.json"
    legacy.parent.mkdir(parents=True)
    legacy.write_text("{}\n", encoding="utf-8")
    compatible_dir = baseline_directory(baseline_root, "release", current)
    trace_only_change_dir = baseline_directory(
        baseline_root,
        "release",
        identity(root, duration=300, trace=TRACE_B),
    )
    assert_true(compatible_dir == trace_only_change_dir, "repository traceability changed baseline ownership")
    assert_true(compatible_dir != legacy.parent, "legacy board/suite baseline was selected")
    assert_true(not (compatible_dir / "manifest.json").exists(), "legacy baseline silently appeared compatible")
    expected_parts = (
        "release",
        current["product_fingerprint"],
        "core",
        current["scenario_fingerprint"],
    )
    assert_true(compatible_dir.parts[-4:] == expected_parts, f"wrong baseline ownership path: {compatible_dir}")


def test_pretty_output_is_repeatable(root: Path) -> None:
    manifest = identity(root)
    first = root / "first.json"
    second = root / "second.json"
    write_identity_manifest(first, manifest)
    write_identity_manifest(second, manifest)
    assert_true(first.read_bytes() == second.read_bytes(), "pretty manifest output was not stable")


def main() -> int:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_fixture(root)
        test_traceability_and_docs_do_not_change_behavior_identities(root)
        (root / "README.md").write_text("documentation\n", encoding="utf-8")
        test_product_inputs_change_only_product_identity(root)
        test_generated_deployed_html_does_not_change_product_identity(root)
        test_grader_inputs_change_only_grader_identity(root)
        test_canonical_manifest_is_stable_and_repo_relative(root)
        test_dirty_relevant_content_digest_is_recorded(root)
        write_fixture(root)
        test_scenario_key_and_legacy_baseline_are_isolated(root)
        test_pretty_output_is_repeatable(root)
    print("bench identity tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

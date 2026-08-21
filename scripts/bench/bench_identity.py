#!/usr/bin/env python3
"""Create deterministic product, hardware-scoring, camera-grader, and scenario identities."""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
from pathlib import Path, PurePosixPath
from typing import Any, Iterable, Mapping, Sequence


ROOT = Path(__file__).resolve().parents[2]
SHA256_NAME = "sha256"
CANONICALIZATION = "json-sort-keys-compact-utf8"
HEX_DIGEST_LENGTH = 64

# Generated web files, gzip copies, and data/_app version markers are
# intentionally represented by their checked-in sources, dependency lock,
# configuration, and deploy/build scripts. Actual deployed audio and branding
# files are hashed directly.
PRODUCT_COMPONENT_PATTERNS: dict[str, tuple[str, ...]] = {
    "firmware_sources": (
        "include/**/*",
        "src/**/*",
    ),
    "production_build": (
        "build.sh",
        "partitions_v1.csv",
        "platformio.ini",
        "scripts/build_production_artifacts.sh",
        "scripts/check_platformio_core_version.py",
        "scripts/enforce_reorder_warning.py",
        "scripts/force_app_upload_offset.py",
        "scripts/get_git_sha.py",
        "scripts/patch_arduino_gfx_qspi.py",
        "scripts/patch_openfontrender.py",
        "scripts/platformio_ca_bundle.sh",
        "tools/compress_web_assets.sh",
    ),
    "filesystem_assets": (
        "config/audio_asset_manifest.json",
        "interface/static/**/*",
        "tools/freq_audio/mulaw/**/*",
    ),
    "deployed_filesystem_assets": (
        "data/audio/**/*",
        "data/branding/**/*",
    ),
    "web_interface": (
        "interface/jsconfig.json",
        "interface/package-lock.json",
        "interface/package.json",
        "interface/scripts/**/*",
        "interface/src/**/*",
        "interface/svelte.config.js",
        "interface/vite.config.js",
    ),
    "v1replay": (
        "tools/v1replay/Package.swift",
        "tools/v1replay/Resources/**/*",
        "tools/v1replay/Sources/**/*",
        "tools/v1replay/scripts/**/*",
    ),
}

PRODUCT_EXCLUDE_PATTERNS = (
    "interface/src/*.spec.js",
    "interface/src/*.test.js",
    "interface/src/**/*.spec.js",
    "interface/src/**/*.test.js",
    "interface/src/test/*",
    "interface/src/test/**/*",
)

GRADER_COMPONENT_PATTERNS: dict[str, tuple[str, ...]] = {
    "identity_contract": ("scripts/bench/bench_identity.py",),
    "camera_capture": (
        "scripts/bench/camera_capture.py",
        "scripts/bench/camera_recorder.swift",
    ),
    "camera_preflight": ("scripts/bench/camera_preflight.py",),
    "camera_contract": ("scripts/bench/camera_contract.py",),
    "camera_artifacts": ("scripts/bench/camera_artifacts.py",),
    "camera_grader": ("scripts/bench/camera_grade.py",),
    "camera_regrader": ("scripts/bench/camera_regrade.py",),
    # Conservative integration coverage: these files also contain non-camera
    # bench code, but they own camera ordering and acceptance wiring. Hashing
    # them prevents that behavior from changing while evidence remains REUSE.
    "camera_integrations": (
        "scripts/bench/run_logged.py",
        "scripts/bench/run_window.py",
        "tools/bench_score.py",
    ),
    # Qualification policy decides whether archived grades and smoke evidence
    # may satisfy a grader-only revalidation.
    "qualification_policy": ("scripts/bench/bench_policy.py",),
    # The top-level bench entry point owns whether camera gating is invoked.
    "camera_gate_entrypoint": ("bench.sh",),
    "grader_dependencies": (
        "scripts/bench/requirements*.txt",
        "scripts/bench/pyproject.toml",
        "scripts/bench/uv.lock",
    ),
}

# These inputs define hardware metric derivation and scoring. They are separate
# from the camera grader because a change here needs a fresh full batch, not a
# camera-only archive regrade and smoke test.
HARDWARE_SCORING_COMPONENT_PATTERNS: dict[str, tuple[str, ...]] = {
    "identity_contract": ("scripts/bench/bench_identity.py",),
    "metric_contract": (
        "tools/hardware_metric_catalog.json",
        "tools/metric_schema.py",
    ),
    "metric_import": (
        "tools/import_perf_csv.py",
        "tools/metric_derivation.py",
        "tools/soak_parse_panic.py",
    ),
    "metric_score": ("tools/score_hardware_run.py",),
    "metric_reporting": ("tools/hardware_report_utils.py",),
    # These mixed integration files contain camera behavior too, so they also
    # remain in the camera grader fingerprint. Their hardware collection,
    # verdict, and qualification branches require the conservative full-batch
    # identity as well until those responsibilities are split into pure modules.
    "hardware_integrations": (
        "bench.sh",
        "scripts/bench/bench_policy.py",
        "scripts/bench/run_logged.py",
        "scripts/bench/run_window.py",
        "tools/bench_score.py",
    ),
}


def current_grader_fingerprint(root: Path = ROOT) -> str:
    """Return the current bench-grader identity without a scenario dependency."""
    return str(_behavior_manifest(root.resolve(), "grader", GRADER_COMPONENT_PATTERNS)["fingerprint"])


def current_hardware_scoring_fingerprint(root: Path = ROOT) -> str:
    """Return the current hardware collection, verdict, and qualification identity."""
    return str(
        _behavior_manifest(
            root.resolve(),
            "hardware-scoring",
            HARDWARE_SCORING_COMPONENT_PATTERNS,
        )["fingerprint"]
    )


def current_product_fingerprint(root: Path = ROOT) -> str:
    """Return the current deployed-product identity without a scenario dependency."""
    return str(_behavior_manifest(root.resolve(), "product", PRODUCT_COMPONENT_PATTERNS)["fingerprint"])


def canonical_bytes(value: Any) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode("utf-8")


def sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def _relative_files(
    root: Path,
    patterns: Sequence[str],
    exclude_patterns: Sequence[str] = (),
) -> list[Path]:
    found: dict[str, Path] = {}
    resolved_root = root.resolve()
    for pattern in patterns:
        for candidate in root.glob(pattern):
            if not candidate.is_file():
                continue
            resolved = candidate.resolve()
            try:
                relative = resolved.relative_to(resolved_root)
            except ValueError as exc:
                raise RuntimeError(f"identity input resolves outside repository: {candidate}") from exc
            relative_name = relative.as_posix()
            if any(PurePosixPath(relative_name).match(excluded) for excluded in exclude_patterns):
                continue
            found[relative_name] = candidate
    return [found[name] for name in sorted(found)]


def _component_manifest(
    root: Path,
    name: str,
    patterns: Sequence[str],
    exclude_patterns: Sequence[str] = (),
) -> dict[str, Any]:
    files = []
    for path in _relative_files(root, patterns, exclude_patterns):
        content = path.read_bytes()
        files.append(
            {
                "path": path.resolve().relative_to(root.resolve()).as_posix(),
                "sha256": sha256_bytes(content),
                "bytes": len(content),
            }
        )
    digest_payload = {"schema": "bench-identity-component-v1", "name": name, "files": files}
    return {"sha256": sha256_bytes(canonical_bytes(digest_payload)), "files": files}


def _behavior_manifest(
    root: Path,
    kind: str,
    component_patterns: Mapping[str, Sequence[str]],
) -> dict[str, Any]:
    components = {
        name: _component_manifest(
            root,
            name,
            patterns,
            PRODUCT_EXCLUDE_PATTERNS if kind == "product" else (),
        )
        for name, patterns in sorted(component_patterns.items())
    }
    fingerprint_payload = {
        "schema": f"bench-{kind}-fingerprint-v1",
        "components": {name: component["sha256"] for name, component in components.items()},
    }
    return {
        "fingerprint": sha256_bytes(canonical_bytes(fingerprint_payload)),
        "components": components,
    }


def scenario_manifest(
    *,
    suite: str,
    duration_seconds: int,
    profile: str,
    segment: str,
    blink_profile: str | None,
) -> dict[str, Any]:
    parameters = {
        "suite": suite,
        "duration_seconds": duration_seconds,
        "profile": profile,
        "segment": segment,
        "blink_profile": blink_profile if suite == "replay" else None,
    }
    payload = {"schema": "bench-scenario-fingerprint-v1", "parameters": parameters}
    return {"fingerprint": sha256_bytes(canonical_bytes(payload)), "parameters": parameters}


def git_traceability(root: Path) -> dict[str, Any]:
    def git(*args: str) -> str:
        proc = subprocess.run(
            ["git", *args],
            cwd=root,
            text=True,
            capture_output=True,
            check=False,
        )
        return proc.stdout.strip() if proc.returncode == 0 else "unknown"

    status = git("status", "--porcelain")
    return {
        "repository_sha": git("rev-parse", "HEAD"),
        "repository_ref": git("rev-parse", "--abbrev-ref", "HEAD"),
        "worktree_clean": status == "",
    }


def build_identity_manifest(
    root: Path,
    *,
    suite: str,
    duration_seconds: int,
    profile: str = "drive_wifi_off",
    segment: str = "last",
    blink_profile: str | None = None,
    traceability: Mapping[str, Any] | None = None,
) -> dict[str, Any]:
    root = root.resolve()
    product = _behavior_manifest(root, "product", PRODUCT_COMPONENT_PATTERNS)
    grader = _behavior_manifest(root, "grader", GRADER_COMPONENT_PATTERNS)
    hardware_scoring = _behavior_manifest(
        root,
        "hardware-scoring",
        HARDWARE_SCORING_COMPONENT_PATTERNS,
    )
    scenario = scenario_manifest(
        suite=suite,
        duration_seconds=duration_seconds,
        profile=profile,
        segment=segment,
        blink_profile=blink_profile,
    )
    trace = dict(traceability) if traceability is not None else git_traceability(root)
    return {
        "schema_version": 2,
        "kind": "bench_identity",
        "algorithm": SHA256_NAME,
        "canonicalization": CANONICALIZATION,
        "product_fingerprint": product["fingerprint"],
        "grader_fingerprint": grader["fingerprint"],
        "hardware_scoring_fingerprint": hardware_scoring["fingerprint"],
        "scenario_fingerprint": scenario["fingerprint"],
        "product": product,
        "grader": grader,
        "hardware_scoring": hardware_scoring,
        "scenario": scenario,
        "traceability": trace,
    }


def write_identity_manifest(path: Path, manifest: Mapping[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def load_identity_manifest(path: Path) -> dict[str, Any]:
    try:
        manifest = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise RuntimeError(f"failed to read bench identity manifest {path}: {exc}") from exc
    if (
        not isinstance(manifest, dict)
        or manifest.get("kind") != "bench_identity"
        or manifest.get("schema_version") not in {1, 2}
    ):
        raise RuntimeError(f"invalid bench identity manifest: {path}")
    for field in ("product_fingerprint", "grader_fingerprint", "scenario_fingerprint"):
        value = str(manifest.get(field) or "")
        if len(value) != HEX_DIGEST_LENGTH or any(character not in "0123456789abcdef" for character in value):
            raise RuntimeError(f"invalid {field} in bench identity manifest: {path}")
    if manifest.get("schema_version") == 2:
        value = str(manifest.get("hardware_scoring_fingerprint") or "")
        if len(value) != HEX_DIGEST_LENGTH or any(character not in "0123456789abcdef" for character in value):
            raise RuntimeError(f"invalid hardware_scoring_fingerprint in bench identity manifest: {path}")
    return manifest


def _safe_path_component(value: str, label: str) -> str:
    if not value or value in {".", ".."} or "/" in value or "\\" in value or "\0" in value:
        raise RuntimeError(f"invalid {label} for baseline path: {value!r}")
    return value


def baseline_directory(
    baseline_root: Path,
    board_id: str,
    identity: Mapping[str, Any],
) -> Path:
    scenario = identity.get("scenario") or {}
    parameters = scenario.get("parameters") if isinstance(scenario, dict) else {}
    suite = str(parameters.get("suite") or "") if isinstance(parameters, dict) else ""
    product_fingerprint = _safe_path_component(str(identity.get("product_fingerprint") or ""), "product fingerprint")
    hardware_scoring_fingerprint = _safe_path_component(
        str(identity.get("hardware_scoring_fingerprint") or ""),
        "hardware scoring fingerprint",
    )
    scenario_fingerprint = _safe_path_component(
        str(identity.get("scenario_fingerprint") or ""), "scenario fingerprint"
    )
    return (
        baseline_root.resolve()
        / _safe_path_component(board_id, "board id")
        / product_fingerprint
        / hardware_scoring_fingerprint
        / _safe_path_component(suite, "suite")
        / scenario_fingerprint
    )


def parse_args(argv: Iterable[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    create = subparsers.add_parser("create", help="write one suite's identity manifest")
    create.add_argument("--output", required=True)
    create.add_argument("--suite", choices=("core", "display", "replay"), required=True)
    create.add_argument("--duration-seconds", type=int, required=True)
    create.add_argument("--profile", default="drive_wifi_off")
    create.add_argument("--segment", default="last")
    create.add_argument("--blink-profile", choices=("scenario", "steady", "stress"), default=None)

    baseline = subparsers.add_parser("baseline-dir", help="print the compatible baseline directory")
    baseline.add_argument("--identity", required=True)
    baseline.add_argument("--baseline-root", required=True)
    baseline.add_argument("--board-id", required=True)
    return parser.parse_args(argv)


def main(argv: Iterable[str] | None = None) -> int:
    args = parse_args(argv)
    if args.command == "create":
        if args.duration_seconds < 1:
            raise SystemExit("--duration-seconds must be positive")
        if args.blink_profile is not None and args.suite != "replay":
            raise SystemExit("--blink-profile requires --suite replay")
        manifest = build_identity_manifest(
            ROOT,
            suite=args.suite,
            duration_seconds=args.duration_seconds,
            profile=args.profile,
            segment=args.segment,
            blink_profile=args.blink_profile,
        )
        write_identity_manifest(Path(args.output), manifest)
        return 0
    identity = load_identity_manifest(Path(args.identity))
    print(baseline_directory(Path(args.baseline_root), args.board_id, identity))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

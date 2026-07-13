#!/usr/bin/env python3
"""Run the tracked mutation catalog against targeted native suites."""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import tempfile
from datetime import datetime
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
CATALOG_PATH = ROOT / "test" / "mutations" / "critical_mutations.json"
ARTIFACT_ROOT = ROOT / ".artifacts" / "test_reports"
IGNORE_PATTERNS = shutil.ignore_patterns(
    ".git",
    ".pio",
    ".artifacts",
    "node_modules",
    "dist",
    "coverage",
    "__pycache__",
    ".scratch",
    "bench_testing",
    "r8",
    ".road_map_cache",
    "release",
    "data",
    "compile_commands.json",
    "road_map.bin",
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--critical", action="store_true", help="run the required critical catalog")
    mode.add_argument("--full", action="store_true", help="run the broader diagnostic catalog")
    parser.add_argument(
        "--catalog",
        default=str(CATALOG_PATH),
        help=f"mutation catalog JSON (default: {CATALOG_PATH})",
    )
    parser.add_argument(
        "--keep-workspace",
        action="store_true",
        help="retain the isolated mutated workspace under the report directory",
    )
    parser.add_argument(
        "--validate-only",
        action="store_true",
        help="validate the catalog (schema, suite names, anchors, mode lists) without running any builds",
    )
    parser.add_argument(
        "--only",
        metavar="ID[,ID...]",
        help="run only the given mutation id(s) from the selected mode (comma-separated)",
    )
    return parser.parse_args()


def load_catalog(path: Path) -> dict[str, Any]:
    payload = json.loads(path.read_text(encoding="utf-8"))
    if "modes" not in payload:
        raise RuntimeError(f"Missing 'modes' in mutation catalog: {path}")
    return payload


def resolve_mode_entries(catalog: dict[str, Any], mode: str) -> list[dict[str, Any]]:
    critical_entries = {
        entry["id"]: entry
        for entry in catalog["modes"].get("critical", [])
    }
    raw_mode_entries = catalog["modes"].get(mode)
    if raw_mode_entries is None:
        raise RuntimeError(f"Mutation catalog has no mode '{mode}'")

    resolved: list[dict[str, Any]] = []
    for item in raw_mode_entries:
        if isinstance(item, str):
            if item not in critical_entries:
                raise RuntimeError(f"Unknown mutation id in mode '{mode}': {item}")
            resolved.append(dict(critical_entries[item]))
        elif isinstance(item, dict):
            resolved.append(dict(item))
        else:
            raise RuntimeError(f"Unsupported mutation entry in mode '{mode}': {item!r}")
    if not resolved:
        raise RuntimeError(f"Mode '{mode}' resolved to an empty mutation list")
    return resolved


REQUIRED_ENTRY_KEYS = ("id", "module", "description", "impact", "file", "search", "replace", "tests")


def discover_native_suites() -> set[str]:
    """Native suite directories accepted by scripts/run_native_tests_serial.py (env=native)."""
    test_root = ROOT / "test"
    ignore_prefixes = ("test_device_", "test_car_mode_")
    return {
        path.name
        for path in test_root.iterdir()
        if path.is_dir()
        and path.name.startswith("test_")
        and not path.name.startswith(ignore_prefixes)
    }


def validate_catalog(catalog: dict[str, Any]) -> list[str]:
    """Static catalog validation: schema, suite names, anchors, mode-list completeness.

    Returns human-readable config errors (empty when valid). Performs no builds
    and never mutates the tree, so it is safe to run before every gate.
    """
    errors: list[str] = []
    known_suites = discover_native_suites()

    entries: dict[str, dict[str, Any]] = {}
    for mode_name in sorted(catalog["modes"]):
        try:
            resolved = resolve_mode_entries(catalog, mode_name)
        except Exception as exc:
            errors.append(f"mode '{mode_name}': {exc}")
            continue
        seen_ids: set[str] = set()
        for entry in resolved:
            entry_id = str(entry.get("id", "<missing id>"))
            if entry_id in seen_ids:
                errors.append(f"mode '{mode_name}': duplicate mutation id '{entry_id}'")
            seen_ids.add(entry_id)
            entries.setdefault(entry_id, entry)

    for entry_id, entry in sorted(entries.items()):
        missing_keys = [key for key in REQUIRED_ENTRY_KEYS if key not in entry]
        if missing_keys:
            errors.append(f"{entry_id}: missing required key(s): {', '.join(missing_keys)}")
            continue

        tests = entry["tests"]
        if not isinstance(tests, list) or not tests or not all(isinstance(t, str) for t in tests):
            errors.append(f"{entry_id}: 'tests' must be a non-empty list of suite names")
        else:
            for test in tests:
                if test not in known_suites:
                    errors.append(
                        f"{entry_id}: unknown native test suite '{test}' "
                        f"(no matching directory under test/)"
                    )

        target = ROOT / str(entry["file"])
        if not target.is_file():
            errors.append(f"{entry_id}: mutation target missing: {entry['file']}")
            continue
        search = str(entry["search"])
        replace = str(entry["replace"])
        if search == replace:
            errors.append(f"{entry_id}: 'search' and 'replace' are identical (no-op mutation)")
        occurrences = target.read_text(encoding="utf-8").count(search)
        if occurrences != 1:
            errors.append(
                f"{entry_id}: mutation anchor occurs {occurrences} time(s) "
                f"in {entry['file']} (expected exactly 1)"
            )

    critical_ids = {
        str(entry["id"])
        for entry in catalog["modes"].get("critical", [])
        if isinstance(entry, dict) and "id" in entry
    }
    full_ids = {
        item if isinstance(item, str) else str(item.get("id"))
        for item in catalog["modes"].get("full", [])
    }
    missing_from_full = sorted(critical_ids - full_ids)
    if missing_from_full:
        errors.append(
            "mode 'full' is missing critical id(s): "
            + ", ".join(missing_from_full)
            + " (every id in modes.critical must also appear in modes.full)"
        )

    return errors


def copy_workspace(destination: Path) -> None:
    shutil.copytree(ROOT, destination, ignore=IGNORE_PATTERNS)


def replace_once(text: str, search: str, replace: str) -> str:
    occurrences = text.count(search)
    if occurrences != 1:
        raise RuntimeError(
            f"Expected exactly one mutation anchor occurrence, found {occurrences}"
        )
    return text.replace(search, replace, 1)


def apply_mutation(workspace: Path, mutation: dict[str, Any]) -> Path:
    relative_path = Path(str(mutation["file"]))
    target = workspace / relative_path
    if not target.exists():
        raise RuntimeError(f"Mutation target missing in workspace: {relative_path}")

    original = target.read_text(encoding="utf-8")
    mutated = replace_once(original, str(mutation["search"]), str(mutation["replace"]))
    target.write_text(mutated, encoding="utf-8")
    return target


def restore_target(workspace: Path, mutation: dict[str, Any]) -> None:
    relative_path = Path(str(mutation["file"]))
    source = ROOT / relative_path
    target = workspace / relative_path
    shutil.copyfile(source, target)


def run_targeted_tests(workspace: Path, tests: list[str], log_path: Path) -> int:
    with log_path.open("w", encoding="utf-8") as log_file:
        result = subprocess.run(
            [sys.executable, "scripts/run_native_tests_serial.py", *tests],
            cwd=workspace,
            stdout=log_file,
            stderr=subprocess.STDOUT,
            check=False,
        )
    return result.returncode


def prepare_workspace(report_dir: Path, keep_workspace: bool) -> tuple[Path, tempfile.TemporaryDirectory[str] | None]:
    if keep_workspace:
        workspace = report_dir / "workspace"
        return workspace, None

    temp_dir = tempfile.TemporaryDirectory(prefix="v1simple_mutation_")
    workspace = Path(temp_dir.name) / "workspace"
    return workspace, temp_dir


def main() -> int:
    args = parse_args()
    mode = "critical"
    if args.full:
        mode = "full"
    elif args.critical:
        mode = "critical"

    catalog_path = Path(args.catalog)
    try:
        catalog = load_catalog(catalog_path)
    except Exception as exc:
        print(f"[mutation] CONFIG ERROR: {exc}", file=sys.stderr)
        return 2

    config_errors = validate_catalog(catalog)
    if config_errors:
        print("[mutation] CONFIG ERROR: mutation catalog failed validation:", file=sys.stderr)
        for error in config_errors:
            print(f"[mutation]   - {error}", file=sys.stderr)
        print(
            "[mutation] CONFIG ERROR: aborting before any mutation runs "
            "(catalog/config failure, NOT a mutation kill)",
            file=sys.stderr,
        )
        return 2

    try:
        mutations = resolve_mode_entries(catalog, mode)
    except Exception as exc:
        print(f"[mutation] CONFIG ERROR: {exc}", file=sys.stderr)
        return 2

    if args.validate_only:
        critical_count = len(catalog["modes"].get("critical", []))
        full_count = len(catalog["modes"].get("full", []))
        print(
            f"[mutation] catalog OK: {catalog_path} "
            f"(critical={critical_count} full={full_count}; "
            f"mode '{mode}' resolves {len(mutations)} mutation(s))"
        )
        return 0

    if args.only:
        requested = [item.strip() for item in args.only.split(",") if item.strip()]
        if not requested:
            print("[mutation] CONFIG ERROR: --only given but no mutation ids parsed", file=sys.stderr)
            return 2
        available_ids = {str(entry["id"]) for entry in mutations}
        unknown_ids = sorted(set(requested) - available_ids)
        if unknown_ids:
            print(
                f"[mutation] CONFIG ERROR: --only id(s) not in mode '{mode}': "
                f"{', '.join(unknown_ids)}",
                file=sys.stderr,
            )
            return 2
        mutations = [entry for entry in mutations if str(entry["id"]) in set(requested)]

    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    report_dir = ARTIFACT_ROOT / f"mutation_{timestamp}"
    logs_dir = report_dir / "logs"
    logs_dir.mkdir(parents=True, exist_ok=True)
    workspace, temp_dir = prepare_workspace(report_dir, args.keep_workspace)

    print(f"[mutation] mode={mode} catalog={catalog_path}")
    print(f"[mutation] preparing isolated workspace: {workspace}")
    try:
        try:
            copy_workspace(workspace)
        except Exception as exc:
            print(f"[mutation] ERROR: failed to copy workspace: {exc}", file=sys.stderr)
            return 2

        # Baseline phase: every referenced suite must pass UNMUTATED before any
        # mutation runs, so a broken/failing suite can never masquerade as a kill.
        baseline_tests = sorted({test for entry in mutations for test in entry["tests"]})
        baseline_log = logs_dir / "baseline.log"
        print(
            f"[mutation] baseline: running {len(baseline_tests)} suite(s) unmutated: "
            f"{', '.join(baseline_tests)}"
        )
        baseline_exit = run_targeted_tests(workspace, baseline_tests, baseline_log)
        if baseline_exit != 0:
            print(
                f"[mutation] CONFIG/BUILD ERROR: baseline (unmutated) run failed with "
                f"exit {baseline_exit}; see {baseline_log.relative_to(ROOT)} — "
                "refusing to attribute pre-existing failures to mutations",
                file=sys.stderr,
            )
            return 2
        print(f"[mutation] baseline: all {len(baseline_tests)} suite(s) passed unmutated")

        results: list[dict[str, Any]] = []
        survivors = 0

        for index, mutation in enumerate(mutations, start=1):
            mutation_id = str(mutation["id"])
            tests = list(mutation["tests"])
            log_path = logs_dir / f"{mutation_id}.log"

            print(
                f"[mutation] ({index}/{len(mutations)}) {mutation_id} "
                f"{mutation['module']}: {mutation['description']}"
            )
            try:
                apply_mutation(workspace, mutation)
                exit_code = run_targeted_tests(workspace, tests, log_path)
            except Exception as exc:
                print(f"[mutation] ERROR: {mutation_id} failed to execute: {exc}", file=sys.stderr)
                return 2
            finally:
                restore_target(workspace, mutation)

            killed = exit_code != 0
            if not killed:
                survivors += 1

            status = "KILLED" if killed else "SURVIVED"
            print(
                f"[mutation]   {status} by {', '.join(tests)} "
                f"(log: {log_path.relative_to(ROOT)})"
            )
            results.append(
                {
                    "id": mutation_id,
                    "module": mutation["module"],
                    "description": mutation["description"],
                    "impact": mutation["impact"],
                    "tests": tests,
                    "status": status,
                    "exit_code": exit_code,
                    "log": str(log_path.relative_to(ROOT)),
                }
            )

        summary = {
            "mode": mode,
            "catalog": str(catalog_path),
            "only": args.only or None,
            "baseline_tests": baseline_tests,
            "baseline_passed": True,
            "workspace": str(workspace) if args.keep_workspace else None,
            "workspace_retained": args.keep_workspace,
            "report_dir": str(report_dir),
            "all_killed": survivors == 0,
            "killed": len(results) - survivors,
            "survived": survivors,
            "results": results,
        }
        (report_dir / "summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")

        print(
            f"[mutation] summary: killed={summary['killed']} "
            f"survived={summary['survived']} report={report_dir.relative_to(ROOT)}"
        )
        return 0 if survivors == 0 else 1
    finally:
        if temp_dir is not None:
            temp_dir.cleanup()


if __name__ == "__main__":
    sys.exit(main())

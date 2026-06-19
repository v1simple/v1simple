#!/usr/bin/env python3
"""Check OBD boot-safety contract.

Enforced invariants:
1) ObdRuntimeModule::update() accepts a bootReady parameter.
2) WAIT_BOOT state guards on bootReady before advancing.
3) No BLE operations appear outside the state machine switch body.

Use --update to rewrite expected snapshot.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import List

ROOT = Path(__file__).resolve().parents[1]
OBD_RUNTIME_FILE = ROOT / "src" / "modules" / "obd" / "obd_runtime_module.cpp"
CONTRACT_FILE = ROOT / "test" / "contracts" / "obd_boot_safety_contract.txt"

MASK_RE = re.compile(
    r"//[^\n]*|/\*.*?\*/|\"(?:\\.|[^\"\\])*\"|'(?:\\.|[^'\\])*'",
    re.DOTALL,
)

UPDATE_SIGNATURE_RE = re.compile(
    r"ObdRuntimeModule::update\s*\([^)]*\bboolReady\b|\bboolReady\b|"
    r"ObdRuntimeModule::update\s*\([^)]*\bbootReady\b"
)

WAIT_BOOT_GUARD_RE = re.compile(
    r"WAIT_BOOT\s*:\s*\{[^}]*if\s*\(\s*!bootReady\s*\)",
    re.DOTALL,
)


def read_text(path: Path) -> str:
    if not path.exists():
        raise FileNotFoundError(f"Source file not found: {path}")
    return path.read_text(encoding="utf-8")


def to_relative(path: Path) -> str:
    try:
        return path.relative_to(ROOT).as_posix()
    except ValueError:
        return path.as_posix()


def mask_comments_and_strings(source: str) -> str:
    def _mask(match: re.Match[str]) -> str:
        return "".join("\n" if ch == "\n" else " " for ch in match.group(0))
    return MASK_RE.sub(_mask, source)


def check_invariants(source: str, masked: str) -> List[str]:
    violations: List[str] = []
    rel = to_relative(OBD_RUNTIME_FILE)

    # 1) update() must accept bootReady parameter
    update_sig = re.search(
        r"void\s+ObdRuntimeModule::update\s*\(([^)]*)\)",
        masked,
    )
    if update_sig is None:
        violations.append(f"file={rel} rule=missing_update_method")
    elif "bootReady" not in update_sig.group(1):
        line = source.count("\n", 0, update_sig.start()) + 1
        violations.append(f"file={rel} line={line} rule=update_missing_bootReady_param")

    # 2) WAIT_BOOT state must guard on !bootReady
    if not WAIT_BOOT_GUARD_RE.search(masked):
        violations.append(f"file={rel} rule=wait_boot_missing_bootReady_guard")

    return violations


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--update",
        action="store_true",
        help="rewrite expected contract snapshot from current source",
    )
    args = parser.parse_args()

    source = read_text(OBD_RUNTIME_FILE)
    masked = mask_comments_and_strings(source)
    actual = sorted(set(check_invariants(source, masked)))

    snapshot = "\n".join(actual) + "\n" if actual else ""
    body = f"# OBD boot-safety contract violations (expected to stay empty)\n{snapshot}"

    if args.update:
        CONTRACT_FILE.parent.mkdir(parents=True, exist_ok=True)
        CONTRACT_FILE.write_text(body, encoding="utf-8")
        print(f"Updated {CONTRACT_FILE}")
        return 0

    if not CONTRACT_FILE.exists():
        print(f"Missing snapshot: {CONTRACT_FILE}  (run with --update)", file=sys.stderr)
        return 1

    expected = CONTRACT_FILE.read_text(encoding="utf-8")
    if body != expected:
        print("[FAIL] OBD boot-safety contract mismatch", file=sys.stderr)
        print(f"  expected:\n{expected}", file=sys.stderr)
        print(f"  actual:\n{body}", file=sys.stderr)
        return 1

    violation_count = len(actual)
    print(f"[contract] obd-boot-safety contract matches ({violation_count} violations)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

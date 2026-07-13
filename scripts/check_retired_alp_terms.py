#!/usr/bin/env python3
"""
Contract guard: retired ALP terminology must not reappear in current code.

The AL Priority *Software Solution Manual* uses PDC / DLI / LID for the
working modes. Earlier project history used "scan"/"armed", then
"detection"/"defense". Both are retired.

This script fails if any retired term appears in:
  - src/         (firmware C++)
  - include/     (firmware headers)
  - interface/src/  (web UI)
  - test/        (native tests + mocks)

Retire-notes inside `docs/` are allowed as history — docs are not scanned.

Exit code 0 on clean, 1 if residual retired terms are found.
"""
from __future__ import annotations

import os
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

# Scan these trees only. Docs are intentionally excluded — retire-notes are
# history and must stay readable.
SCAN_DIRS = [
    "src",
    "include",
    "interface/src",
    "test",
]

# File extensions we consider "current code".
EXTENSIONS = {
    ".c", ".cc", ".cpp", ".h", ".hpp",
    ".js", ".ts", ".svelte",
    ".py",
}

# Retired terms. Each entry is (regex, human description).
# Case-sensitive by default. Word-boundary patterns avoid false positives
# on e.g. "detected" (which is fine) versus "detection mode" (which is not).
RETIRED_PATTERNS = [
    # Same-line ALP-context uses of the broad retired vocabulary. We keep this
    # deliberately ALP-prefixed so legitimate BLE/WiFi/OBD "scan" language and
    # power-module "armed" language are not blocked.
    (re.compile(r"\bALP\b[^\n]{0,120}\b(?:scan|armed|detection|defense)\b", re.IGNORECASE),
     "ALP-context retired term (use PDC / DLI / LID / Warm-Up / Targeted)"),
    # "detection mode" / "defense mode" — retired in favor of PDC/DLI/LID.
    (re.compile(r"\bdetection mode\b", re.IGNORECASE), "'detection mode'"),
    (re.compile(r"\bdefense mode\b", re.IGNORECASE), "'defense mode'"),
    # Camel-case identifiers that encoded retired terms.
    (re.compile(r"\bcolorAlpDetection\b"), "identifier 'colorAlpDetection'"),
    (re.compile(r"\bcolorAlpDefense\b"), "identifier 'colorAlpDefense'"),
    (re.compile(r"\bhasColorAlpDetection\b"), "identifier 'hasColorAlpDetection'"),
    (re.compile(r"\bhasColorAlpDefense\b"), "identifier 'hasColorAlpDefense'"),
    (re.compile(r"\balpDefenseMode_\b"), "field 'alpDefenseMode_' (now alpLidActive_)"),
    # JSON/HTTP field names.
    (re.compile(r'"alpDetection"'), 'JSON field "alpDetection"'),
    (re.compile(r'"alpDefense"'), 'JSON field "alpDefense"'),
    # Older retired terms — "scan mode", "armed mode", "jam mode",
    # "observe mode", "self-test"/"self test".
    (re.compile(r"\bscan mode\b", re.IGNORECASE), "'scan mode'"),
    (re.compile(r"\barmed mode\b", re.IGNORECASE), "'armed mode'"),
    (re.compile(r"\bjam mode\b", re.IGNORECASE), "'jam mode'"),
    (re.compile(r"\bobserve mode\b", re.IGNORECASE), "'observe mode'"),
    (re.compile(r"\bself[- ]test\b", re.IGNORECASE), "'self-test' (use 'Warm-Up')"),
    # Narrowly-targeted patterns for known leak forms that escaped word-boundary
    # guards above (H-01/H-02, 2026-04-23).
    (re.compile(r"scan-vs-armed", re.IGNORECASE), "'scan-vs-armed' (use PDC/DLI/LID)"),
    (re.compile(r"\benvelope armed\b", re.IGNORECASE), "'envelope armed' (use 'envelope captured' or similar)"),
]


def scan_file(path: Path) -> list[tuple[int, str, str]]:
    """Return [(lineno, description, line)] for each retired-term hit."""
    hits: list[tuple[int, str, str]] = []
    try:
        with path.open("r", encoding="utf-8", errors="replace") as f:
            for lineno, line in enumerate(f, start=1):
                for pat, desc in RETIRED_PATTERNS:
                    if pat.search(line):
                        hits.append((lineno, desc, line.rstrip()))
                        break
    except OSError:
        pass
    return hits


def should_scan(path: Path) -> bool:
    if path.suffix not in EXTENSIONS:
        return False
    # Skip generated / vendored / build output.
    parts = set(path.parts)
    skip_dirs = {"node_modules", ".pio", ".svelte-kit", "build", "dist",
                 ".git", "static", "public"}
    if parts & skip_dirs:
        return False
    return True


def main() -> int:
    total_hits = 0
    for scan_dir in SCAN_DIRS:
        root = REPO_ROOT / scan_dir
        if not root.exists():
            continue
        for path in sorted(root.rglob("*")):
            if not path.is_file() or not should_scan(path):
                continue
            hits = scan_file(path)
            for lineno, desc, line in hits:
                rel = path.relative_to(REPO_ROOT)
                print(f"{rel}:{lineno}: {desc}")
                print(f"    {line}")
                total_hits += 1

    if total_hits > 0:
        print()
        print(f"FAIL: {total_hits} retired ALP term(s) found.")
        print("Use manual vocabulary: PDC / DLI / LID / Warm-Up / Targeted.")
        print("See docs/plans/ALP_TERMINOLOGY_RENAME_20260417.md")
        return 1

    print("OK: no retired ALP terms found in src/, include/, interface/src/, test/.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

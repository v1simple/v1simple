#!/usr/bin/env python3
"""Validate a commit message against the project's Conventional Commits contract.

The format is the one already specified in the project instructions:

    <type>(<scope>): short summary

Rules enforced:
  - type is from the allowed set
  - scope, if present, is from the allowed set (lowercase, from src/modules/ + known areas)
  - summary is imperative-ish: no trailing period, not capitalized, non-empty
  - subject line <= 72 chars
  - blank line between subject and body (if a body exists)
  - body lines <= 100 chars (URLs and code fences exempt)
  - a `!` breaking-change marker or `BREAKING CHANGE:` footer is allowed

Exit 0  → message is valid
Exit 1  → violations found (printed to stderr)

Usage:
    python3 scripts/check_commit_msg.py .git/COMMIT_EDITMSG   # hook usage
    python3 scripts/check_commit_msg.py --selftest            # run built-in tests
    echo "feat(ble): add x" | python3 scripts/check_commit_msg.py -
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# Conventional Commit types. Keep this list short and meaningful.
TYPES = {
    "feat",      # new capability
    "fix",       # bug fix
    "perf",      # performance work (must cite a measurement)
    "refactor",  # behavior-preserving restructure
    "test",      # tests only
    "docs",      # documentation only
    "build",     # build system, platformio, deps
    "ci",        # workflows, gates, scripts
    "chore",     # housekeeping with no src/ behavior change
    "revert",    # revert of a previous commit
}

# Scopes: the subsystem touched. Derived from src/modules/ plus top-level areas.
SCOPES = {
    # runtime subsystems
    "alert_persistence", "alp", "auto_push", "ble", "display", "gps", "obd",
    "perf", "power", "qualification", "quiet", "speed", "speed_mute",
    "system", "touch", "voice", "volume_fade", "wifi",
    # cross-cutting areas
    "audio", "settings", "storage", "packet", "proxy", "boot", "bench",
    "interface",   # svelte web UI
    "release",     # release engineering / versioning
    "deps",
}

SUBJECT_MAX = 72
BODY_MAX = 100

SUBJECT_RE = re.compile(
    r"^(?P<type>[a-z]+)"
    r"(?:\((?P<scope>[a-z0-9_,-]+)\))?"
    r"(?P<breaking>!)?"
    r": "
    r"(?P<summary>.+)$"
)

URL_RE = re.compile(r"https?://\S")


def _is_exempt_body_line(line: str) -> bool:
    """Long lines that are legitimately unwrappable."""
    stripped = line.strip()
    return (
        bool(URL_RE.search(line))
        or stripped.startswith("```")
        or stripped.startswith("|")       # markdown table
        or stripped.startswith("    ")    # indented code block
        or ":" in stripped and " " not in stripped  # e.g. a bare file:line ref
    )


def validate(message: str) -> list[str]:
    """Return a list of violation strings. Empty list means valid."""
    problems: list[str] = []

    # Strip comment lines git adds to the editor buffer, and trailing blanks.
    lines = [ln for ln in message.splitlines() if not ln.startswith("#")]
    while lines and not lines[-1].strip():
        lines.pop()

    if not lines or not lines[0].strip():
        return ["empty commit message"]

    # A merge/revert/fixup commit is allowed through untouched.
    subject = lines[0].rstrip()
    if subject.startswith(("Merge ", "Revert ", "fixup!", "squash!")):
        return []

    m = SUBJECT_RE.match(subject)
    if not m:
        problems.append(
            f"subject does not match '<type>(<scope>): summary'\n"
            f"    got: {subject!r}\n"
            f"    e.g. fix(ble): defer bond deletion to the main loop"
        )
        # Without a parse there is nothing further to check on the subject.
        return problems + _check_body(lines)

    ctype = m.group("type")
    scope = m.group("scope")
    summary = m.group("summary")

    if ctype not in TYPES:
        problems.append(
            f"unknown type {ctype!r}. allowed: {', '.join(sorted(TYPES))}"
        )

    if scope:
        for part in scope.split(","):
            if part not in SCOPES:
                problems.append(
                    f"unknown scope {part!r}. allowed: {', '.join(sorted(SCOPES))}\n"
                    f"    (add a new one to SCOPES in scripts/check_commit_msg.py "
                    f"if a real new subsystem exists)"
                )

    if len(subject) > SUBJECT_MAX:
        problems.append(f"subject is {len(subject)} chars; max {SUBJECT_MAX}")

    if summary.endswith("."):
        problems.append("summary must not end with a period")

    if summary[:1].isupper() and not summary.split()[0].isupper():
        # Allow ALLCAPS acronyms (BLE, NVS); reject sentence-casing.
        problems.append("summary should start lowercase (imperative mood)")

    if summary.lower().startswith(("added ", "fixed ", "updated ", "changed ", "removed ")):
        problems.append(
            "summary should be imperative present tense "
            "('add', not 'added'; 'fix', not 'fixed')"
        )

    return problems + _check_body(lines)


def _check_body(lines: list[str]) -> list[str]:
    problems: list[str] = []
    if len(lines) < 2:
        return problems

    if lines[1].strip():
        problems.append("subject and body must be separated by a blank line")

    for i, line in enumerate(lines[2:], start=3):
        if len(line) > BODY_MAX and not _is_exempt_body_line(line):
            problems.append(f"body line {i} is {len(line)} chars; max {BODY_MAX}")
    return problems


# --------------------------------------------------------------------------
# Self-tests. This repo gates its own check scripts; this one is no exception.
# --------------------------------------------------------------------------

_VALID = [
    "fix(ble): defer bond deletion to the main loop",
    "feat(display): add secondary alert cards",
    "fix(ble,display): tighten notify-to-render path",
    "docs: correct the maintenance-UI feature list",
    "perf(display): halve partial-flush area on steady state",
    "feat(wifi)!: drop legacy includePasswords export",
    "chore(deps): bump ArduinoJson to 7.4.3",
    "Merge branch 'main' into feature",
    "revert: feat(display) add secondary alert cards",
    "fix(ble): keep NVS write off the host task\n\nThe NimBLE host task has a 5120-byte stack.\nDefer to the main loop instead.",
]

_INVALID = [
    ("", "empty"),
    ("update stuff", "no type"),
    ("Fix(ble): thing", "capitalized type"),
    ("feat(nonsense): thing", "unknown scope"),
    ("wibble(ble): thing", "unknown type"),
    ("fix(ble): Added a thing", "past tense + capitalized"),
    ("fix(ble): add a thing.", "trailing period"),
    ("fix(ble): " + "x" * 80, "too long"),
    ("fix(ble): add thing\nbody with no blank line", "missing blank line"),
]


def _selftest() -> int:
    failures = 0
    for msg in _VALID:
        problems = validate(msg)
        if problems:
            failures += 1
            print(f"FAIL (expected valid): {msg!r}\n  -> {problems}", file=sys.stderr)
    for msg, why in _INVALID:
        if not validate(msg):
            failures += 1
            print(f"FAIL (expected invalid: {why}): {msg!r}", file=sys.stderr)
    if failures:
        print(f"\n{failures} self-test failure(s)", file=sys.stderr)
        return 1
    print(f"commit-msg self-tests passed ({len(_VALID)} valid, {len(_INVALID)} invalid)")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("path", nargs="?", help="path to commit message file, or '-' for stdin")
    ap.add_argument("--selftest", action="store_true", help="run built-in tests")
    args = ap.parse_args()

    if args.selftest:
        return _selftest()

    if not args.path:
        ap.error("need a commit message file (or --selftest)")

    message = sys.stdin.read() if args.path == "-" else Path(args.path).read_text(encoding="utf-8")

    problems = validate(message)
    if not problems:
        return 0

    print("\n✗ commit message rejected\n", file=sys.stderr)
    for p in problems:
        print(f"  • {p}", file=sys.stderr)
    print(
        "\n  format: <type>(<scope>): short summary\n"
        "  example: fix(ble): defer bond deletion to the main loop\n"
        f"  types:  {', '.join(sorted(TYPES))}\n"
        "\n  (bypass with --no-verify only if you truly mean to)\n",
        file=sys.stderr,
    )
    return 1


if __name__ == "__main__":
    sys.exit(main())

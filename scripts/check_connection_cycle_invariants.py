#!/usr/bin/env python3
"""Enforce ConnectionCycleCoordinator invariants 1 and 2.

Invariant 1 — Single-writer ownership
--------------------------------------
Only `connection_cycle_coordinator_module.cpp` may reference `CycleState::`
enum values directly. Other source files in `src/` must use the uint8_t
state-code accessor; they must never switch on or assign `CycleState::`
values. This prevents external files from replicating or second-guessing
the coordinator's transition logic.

Invariant 2 — Legal-transition graph
-------------------------------------
Every `transitionTo(X)` call in the coordinator `.cpp` must match a
whitelisted edge. The whitelist encodes the complete legal graph derived
from reading the source. Any edit that introduces an unlisted edge fails CI.

Scope
-----
- Invariant 1 walks `src/` excluding `connection_cycle_coordinator_module.cpp`
  and `.h`. Flags any direct `CycleState::` reference.
- Invariant 2 parses only `connection_cycle_coordinator_module.cpp`.
  Recognizes `transitionTo(CycleState::X, ...)`, ternary forms, and helper
  calls that return a `CycleState` (today only `nextPostObdState`).
- Understands three caller functions:
    * `update(...)` — uses the enclosing `case CycleState::X:` as from-state.
    * `enterTeardown(...)` — called from `update()` under a guard that excludes
      SCAN_V1 and TEARDOWN; treated as "from any non-SCAN_V1, non-TEARDOWN
      state to TEARDOWN."
    * `updateTeardown(...)` — always called while state_ == TEARDOWN.

When to update
--------------
If a legitimate new edge is added to the coordinator, add it to WHITELIST
below and document the trigger in `src/modules/system/api.md`.

If `nextPostObdState` ever gains a fourth return path, update
`HELPER_EXPANSIONS` below.

If a new file legitimately needs direct `CycleState::` access, add it to
`SINGLE_WRITER_ALLOWED_FILES` below with justification.
"""

from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC_ROOT = ROOT / "src"
COORDINATOR_CPP = (
    ROOT / "src" / "modules" / "system" / "connection_cycle_coordinator_module.cpp"
)

# Files allowed to reference CycleState:: directly (invariant 1).
SINGLE_WRITER_ALLOWED_FILES: set[str] = {
    "src/modules/system/connection_cycle_coordinator_module.cpp",
    "src/modules/system/connection_cycle_coordinator_module.h",
}
CYCLE_STATE_REF_RE = re.compile(r"\bCycleState::")
SOURCE_SUFFIXES: set[str] = {".c", ".cc", ".cpp", ".h", ".hpp"}

# Complete legal edge set. Derived from reading the coordinator .cpp with the
# switch arms open. (from, to) pairs. Keep sorted by source for readability.
WHITELIST: set[tuple[str, str]] = {
    # From SCAN_V1
    ("SCAN_V1", "V1_SETTLING"),
    ("SCAN_V1", "WIFI_OPEN"),     # Manual WiFi intent or timeout with wifi enabled
    ("SCAN_V1", "STEADY"),        # Timeout fallback with wifi disabled
    # From V1_SETTLING
    ("V1_SETTLING", "OBD_SCAN"),
    ("V1_SETTLING", "PROXY_OPEN"),
    ("V1_SETTLING", "WIFI_OPEN"),
    ("V1_SETTLING", "STEADY"),
    ("V1_SETTLING", "TEARDOWN"),
    # From OBD_SCAN
    ("OBD_SCAN", "OBD_CONNECT"),
    ("OBD_SCAN", "OBD_SETTLED"),
    ("OBD_SCAN", "PROXY_OPEN"),
    ("OBD_SCAN", "WIFI_OPEN"),
    ("OBD_SCAN", "STEADY"),
    ("OBD_SCAN", "TEARDOWN"),
    # From OBD_CONNECT
    ("OBD_CONNECT", "OBD_SETTLED"),
    ("OBD_CONNECT", "PROXY_OPEN"),
    ("OBD_CONNECT", "WIFI_OPEN"),
    ("OBD_CONNECT", "STEADY"),
    ("OBD_CONNECT", "TEARDOWN"),
    # From OBD_SETTLED
    ("OBD_SETTLED", "PROXY_OPEN"),
    ("OBD_SETTLED", "WIFI_OPEN"),
    ("OBD_SETTLED", "STEADY"),
    ("OBD_SETTLED", "TEARDOWN"),
    # From PROXY_OPEN
    ("PROXY_OPEN", "WIFI_OPEN"),
    ("PROXY_OPEN", "TEARDOWN"),
    # From WIFI_OPEN
    ("WIFI_OPEN", "V1_SETTLING"),  # Late V1 edge takes priority over WiFi completion
    ("WIFI_OPEN", "STEADY"),
    ("WIFI_OPEN", "TEARDOWN"),
    # From STEADY
    ("STEADY", "V1_SETTLING"),  # Late V1 edge restarts normal connection sequencing
    ("STEADY", "WIFI_OPEN"),
    ("STEADY", "TEARDOWN"),
    # From TEARDOWN
    ("TEARDOWN", "SCAN_V1"),
}

# Helpers that return a CycleState. Maps helper name to the set of possible
# return values it can produce. Keep this in lock-step with the .cpp namespace
# helpers.
HELPER_EXPANSIONS: dict[str, set[str]] = {
    "nextPostObdState": {"PROXY_OPEN", "WIFI_OPEN", "STEADY"},
}

# enterTeardown() is invoked from update() at a site whose guard excludes
# SCAN_V1 and TEARDOWN from being the current state. Any other state is
# legal as the "from" half of the edge.
TEARDOWN_VALID_FROM: set[str] = {
    "V1_SETTLING",
    "OBD_SCAN",
    "OBD_CONNECT",
    "OBD_SETTLED",
    "PROXY_OPEN",
    "WIFI_OPEN",
    "STEADY",
}

# Known enum values. Derived from the .h; used to sanity-check the whitelist.
KNOWN_STATES: set[str] = {
    "SCAN_V1",
    "V1_SETTLING",
    "OBD_SCAN",
    "OBD_CONNECT",
    "OBD_SETTLED",
    "PROXY_OPEN",
    "WIFI_OPEN",
    "STEADY",
    "TEARDOWN",
}

FUNCTION_DEF_RE = re.compile(
    r"^\s*(?:void|CycleState|bool|uint32_t)\s+"
    r"ConnectionCycleCoordinatorModule::(\w+)\s*\("
)
CASE_RE = re.compile(r"\bcase\s+CycleState::(\w+)\s*:")
TRANSITION_CALL_RE = re.compile(r"\btransitionTo\s*\(")
OPEN_BRACE_RE = re.compile(r"^\s*\{")
CLOSE_BRACE_RE = re.compile(r"^\s*\}")


def _collect_transition_arg(lines: list[str], start_idx: int) -> tuple[str, int]:
    """Starting at a line containing `transitionTo(`, return the full first
    argument (before the top-level comma) plus the line index of the closing
    paren. Handles calls that span two or three lines."""
    line = lines[start_idx]
    call_start = line.index("transitionTo(") + len("transitionTo(")
    buf = line[call_start:]
    idx = start_idx
    while True:
        depth = 0
        for pos, ch in enumerate(buf):
            if ch == "(":
                depth += 1
            elif ch == ")":
                if depth == 0:
                    # We have the full call up to this point. Now split on the
                    # top-level comma to isolate the first argument.
                    full_args = buf[:pos]
                    return _split_first_arg(full_args), idx
                depth -= 1
        idx += 1
        if idx >= len(lines):
            raise RuntimeError(
                f"transitionTo call starting at line {start_idx + 1} never closes"
            )
        buf += " " + lines[idx]


def _split_first_arg(args: str) -> str:
    depth = 0
    for pos, ch in enumerate(args):
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        elif ch == "," and depth == 0:
            return args[:pos]
    return args


def _extract_targets(arg_expr: str) -> set[str]:
    """Extract all possible CycleState targets from a transitionTo first
    argument. Handles literals, ternaries, and helper-function returns."""
    targets: set[str] = set()
    for name in re.findall(r"CycleState::(\w+)", arg_expr):
        if name not in KNOWN_STATES:
            raise RuntimeError(
                f"Unknown CycleState value referenced in transitionTo: {name}"
            )
        targets.add(name)
    for helper, expansion in HELPER_EXPANSIONS.items():
        if re.search(rf"\b{helper}\s*\(", arg_expr):
            targets |= expansion
    if not targets:
        raise RuntimeError(
            f"Could not extract any CycleState target from: {arg_expr!r}"
        )
    return targets


def extract_edges(source: str) -> set[tuple[str, str]]:
    lines = source.splitlines()
    edges: set[tuple[str, str]] = set()
    current_function: str | None = None
    current_case: str | None = None

    idx = 0
    while idx < len(lines):
        line = lines[idx]

        fn_match = FUNCTION_DEF_RE.match(line)
        if fn_match:
            current_function = fn_match.group(1)
            current_case = None
            idx += 1
            continue

        case_match = CASE_RE.search(line)
        if case_match:
            current_case = case_match.group(1)
            if current_case not in KNOWN_STATES:
                raise RuntimeError(f"Unknown case label: {current_case}")
            idx += 1
            continue

        if TRANSITION_CALL_RE.search(line):
            arg_expr, end_idx = _collect_transition_arg(lines, idx)
            targets = _extract_targets(arg_expr)
            _record_edges(
                edges, current_function, current_case, targets, idx + 1
            )
            idx = end_idx + 1
            continue

        idx += 1

    return edges


def _record_edges(
    edges: set[tuple[str, str]],
    current_function: str | None,
    current_case: str | None,
    targets: set[str],
    source_line: int,
) -> None:
    if current_function == "update":
        if current_case is None:
            raise RuntimeError(
                f"transitionTo in update() without enclosing case at line "
                f"{source_line}"
            )
        for target in targets:
            edges.add((current_case, target))
    elif current_function == "enterTeardown":
        # Treat as edge from any valid pre-teardown state.
        for frm in TEARDOWN_VALID_FROM:
            for target in targets:
                edges.add((frm, target))
    elif current_function == "updateTeardown":
        for target in targets:
            edges.add(("TEARDOWN", target))
    elif current_function == "transitionTo":
        # The definition itself; ignore.
        return
    else:
        # Any new function that calls transitionTo should be explicitly handled.
        raise RuntimeError(
            f"transitionTo called from unhandled function "
            f"{current_function!r} at line {source_line}"
        )


def check_single_writer() -> list[str]:
    """Invariant 1: no file outside the coordinator uses CycleState:: directly."""
    violations: list[str] = []
    for path in sorted(SRC_ROOT.rglob("*")):
        if not path.is_file() or path.suffix not in SOURCE_SUFFIXES:
            continue
        relative = path.relative_to(ROOT).as_posix()
        if relative in SINGLE_WRITER_ALLOWED_FILES:
            continue
        for line_no, raw_line in enumerate(
            path.read_text(encoding="utf-8").splitlines(), start=1
        ):
            if CYCLE_STATE_REF_RE.search(raw_line):
                violations.append(f"{relative}:{line_no}: {raw_line.strip()}")
    return violations


def main() -> int:
    violations: list[str] = []

    # -- Invariant 1: single-writer ownership --------------------------------
    writer_violations = check_single_writer()
    if writer_violations:
        violations.append(
            "Single-writer violation: CycleState:: referenced outside the "
            "coordinator module. External code must use the uint8_t state-code "
            "accessor, not the enum directly."
        )
        for row in writer_violations:
            violations.append(f"  ! {row}")

    # -- Invariant 2: legal-transition graph ---------------------------------
    source = COORDINATOR_CPP.read_text(encoding="utf-8")
    try:
        edges = extract_edges(source)
    except RuntimeError as err:
        print("[contract] connection cycle invariants: parser error.")
        print(f"  - {err}")
        return 1

    illegal = sorted(edges - WHITELIST)
    unreachable = sorted(WHITELIST - edges)

    if illegal:
        violations.append(
            "Illegal transitions found (not in whitelist). Either the edge is "
            "wrong, or the whitelist must be extended with justification."
        )
        for frm, to in illegal:
            violations.append(f"  + {frm} -> {to}")

    if unreachable:
        violations.append(
            "Whitelisted transitions no longer present in the coordinator. "
            "Either an edge was legitimately removed (update whitelist) or a "
            "code path was dropped unintentionally."
        )
        for frm, to in unreachable:
            violations.append(f"  - {frm} -> {to}")

    if violations:
        print("[contract] connection cycle invariants contract violated.")
        for row in violations:
            print(row)
        return 1

    print(
        f"[contract] connection cycle invariants contract matches "
        f"({len(edges)} edges, whitelist stable, single-writer clean)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Regression tests for the Tier-0 BLE invariant guards.

The Tier-0 guards are the only automated defense for two invariants that brick
the product in the field: forbidden work inside BLE callbacks, and runtime
NimBLE teardown. Both guards report success by finding nothing, so a guard that
silently stopped detecting is indistinguishable from a clean tree. These tests
plant known violations and assert each guard still catches them.

Covers:
  - scripts/check_ble_deletion_contract.py
  - scripts/check_ble_hot_path_contract.py
"""

from __future__ import annotations

from contextlib import contextmanager
from pathlib import Path
import subprocess
import sys
import tempfile
from typing import Any, Iterator, Sequence

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

import check_ble_deletion_contract as deletion  # type: ignore  # noqa: E402
import check_ble_hot_path_contract as hot_path  # type: ignore  # noqa: E402


def assert_equal(actual: Any, expected: Any, message: str) -> None:
    if actual != expected:
        raise AssertionError(f"{message}: expected {expected!r}, got {actual!r}")


def assert_true(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def write_file(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


@contextmanager
def patched(module: Any, **replacements: Any) -> Iterator[None]:
    originals = {name: getattr(module, name) for name in replacements}
    try:
        for name, replacement in replacements.items():
            setattr(module, name, replacement)
        yield
    finally:
        for name, original in originals.items():
            setattr(module, name, original)


# --------------------------------------------------------------------------
# check_ble_deletion_contract.py
# --------------------------------------------------------------------------


def run_deletion_guard(tree: Path) -> int:
    """Run the deletion guard against a synthetic source tree."""
    with patched(deletion, ROOT=tree, SOURCE_DIRS=(tree / "src", tree / "include")):
        return deletion.main()


def test_deletion_guard_passes_on_clean_tree(tmpdir: Path) -> None:
    write_file(
        tmpdir / "src" / "ble_client.cpp",
        "// boot path\nvoid setup() { NimBLEDevice::init(\"x\"); }\n",
    )
    assert_equal(run_deletion_guard(tmpdir), 0, "clean tree should pass")


def test_deletion_guard_catches_banned_calls(tmpdir: Path) -> None:
    for call, label in (
        ("NimBLEDevice::deleteClient(client)", "deleteClient"),
        ("NimBLEDevice::deinit()", "deinit"),
    ):
        tree = tmpdir / label
        write_file(tree / "src" / "ble_proxy.cpp", f"void f() {{ {call}; }}\n")
        assert_equal(run_deletion_guard(tree), 1, f"{label} must be rejected")


def test_deletion_guard_sees_past_line_comments(tmpdir: Path) -> None:
    """The bug this suite exists for.

    `//.*$` under re.DOTALL matched greedily to end-of-file, so everything after
    a file's first line comment was stripped before scanning. On the real tree
    that blinded the guard to ~84% of production source.
    """
    write_file(
        tmpdir / "src" / "ble_connection.cpp",
        "// a perfectly ordinary comment on line 1\n"
        "void a() {}\n"
        "void b() { NimBLEDevice::deleteClient(c); }\n",
    )
    assert_equal(
        run_deletion_guard(tmpdir),
        1,
        "violation after a line comment must still be detected",
    )


def test_deletion_guard_reports_true_line_numbers(tmpdir: Path) -> None:
    """Comment masking must preserve byte offsets or line numbers drift."""
    write_file(
        tmpdir / "src" / "ble_proxy.cpp",
        "/* a\n   multi-line\n   banner comment */\n"
        "// and a line comment\n"
        "void f() { NimBLEDevice::deinit(); }\n",
    )
    with patched(deletion, ROOT=tmpdir, SOURCE_DIRS=(tmpdir / "src",)):
        path = (tmpdir / "src" / "ble_proxy.cpp")
        text = path.read_text(encoding="utf-8")
        masked = deletion.mask_comments_and_strings(text)
        assert_equal(len(masked), len(text), "masking must preserve length")
        match = deletion.BANNED_DEINIT.search(masked)
        assert_true(match is not None, "deinit should be found")
        line_no = text[: match.start()].count("\n") + 1
        assert_equal(line_no, 5, "reported line number must match the source")


def test_deletion_guard_ignores_commented_and_quoted_mentions(tmpdir: Path) -> None:
    write_file(
        tmpdir / "src" / "docs.cpp",
        "// never call NimBLEDevice::deleteClient() here\n"
        "/* NimBLEDevice::deinit() is banned */\n"
        'const char* docs = "NimBLEDevice::deleteClient() and '
        'NimBLEDevice::deinit() are banned";\n'
        "void f() {}\n",
    )
    assert_equal(
        run_deletion_guard(tmpdir), 0, "documentation mentions must not trip the guard"
    )


def test_deletion_guard_sees_past_comment_markers_in_strings(tmpdir: Path) -> None:
    write_file(
        tmpdir / "src" / "ble_proxy.cpp",
        'void f() { const char* url = "https://example.invalid"; '
        "NimBLEDevice::deleteClient(c); }\n",
    )
    assert_equal(
        run_deletion_guard(tmpdir),
        1,
        "comment markers in strings must not hide a later violation",
    )


def test_deletion_guard_restricts_delete_all_bonds_by_file(tmpdir: Path) -> None:
    authorized = tmpdir / "authorized"
    write_file(
        authorized / "src" / "ble_client.cpp",
        "void boot() { NimBLEDevice::deleteAllBonds(); }\n",
    )
    assert_equal(
        run_deletion_guard(authorized), 0, "deleteAllBonds is allowed in ble_client.cpp"
    )

    unauthorized = tmpdir / "unauthorized"
    write_file(
        unauthorized / "src" / "ble_proxy.cpp",
        "void boot() { NimBLEDevice::deleteAllBonds(); }\n",
    )
    assert_equal(
        run_deletion_guard(unauthorized),
        1,
        "deleteAllBonds outside ble_client.cpp must be rejected",
    )


def test_deletion_guard_scans_headers(tmpdir: Path) -> None:
    write_file(
        tmpdir / "include" / "ble_helper.h",
        "inline void f() { NimBLEDevice::deleteClient(c); }\n",
    )
    assert_equal(run_deletion_guard(tmpdir), 1, "headers must be scanned too")


# --------------------------------------------------------------------------
# check_ble_hot_path_contract.py
# --------------------------------------------------------------------------

FORBIDDEN_SAMPLES = (
    ("forbidden_serial_print", 'Serial.println("x");'),
    ("forbidden_log_call", "log_e(\"x\");"),
    ("forbidden_esp_log", 'ESP_LOGI("t", "x");'),
    ("forbidden_string", "String s;"),
    ("forbidden_new", "int* p = new int(1);"),
    ("forbidden_malloc", "void* p = malloc(4);"),
    ("forbidden_delay", "delay(1);"),
    ("forbidden_vtaskdelay", "vTaskDelay(1);"),
    ("forbidden_xsemaphoretake_portmaxdelay", "xSemaphoreTake(m, portMAX_DELAY);"),
)


def callback_source(body: str) -> str:
    return (
        "void V1BLEClient::notifyCallback(int a) {\n"
        f"  {body}\n"
        "}\n"
    )


def scan_callback(source: str, targets: Sequence[Sequence[str]]) -> list[str]:
    path = Path("src/ble_client.cpp")
    masked = hot_path.mask_comments_and_strings(source)
    return hot_path.make_callback_violations(source, masked, path, targets)


def test_hot_path_detects_every_forbidden_pattern() -> None:
    """Each FORBIDDEN_PATTERNS entry must actually fire on a real sample."""
    targets = (("V1BLEClient::notifyCallback",),)
    covered = {rule for rule, _ in hot_path.FORBIDDEN_PATTERNS}
    tested = {rule for rule, _ in FORBIDDEN_SAMPLES}
    assert_equal(
        tested,
        covered,
        "every forbidden pattern needs a sample (add one when adding a rule)",
    )

    for rule, snippet in FORBIDDEN_SAMPLES:
        violations = scan_callback(callback_source(snippet), targets)
        assert_true(
            any(f"rule={rule}" in v for v in violations),
            f"{rule} not detected for snippet {snippet!r}: got {violations}",
        )


def test_hot_path_ignores_comments_and_strings() -> None:
    targets = (("V1BLEClient::notifyCallback",),)
    source = callback_source('// delay(1); and "String"\n  /* new int; */')
    assert_equal(
        scan_callback(source, targets),
        [],
        "commented-out forbidden work must not be flagged",
    )


def test_hot_path_flags_missing_callback_body() -> None:
    targets = (("V1BLEClient::vanished",),)
    violations = scan_callback(callback_source("int x = 0;"), targets)
    assert_true(
        any("rule=missing_callback_body" in v for v in violations),
        f"a renamed or removed callback must be reported: {violations}",
    )


def test_hot_path_skips_declarations() -> None:
    """A prototype is not a body; the guard must find the definition."""
    source = (
        "void V1BLEClient::notifyCallback(int a) const;\n"
        + callback_source("delay(1);")
    )
    violations = scan_callback(source, (("V1BLEClient::notifyCallback",),))
    assert_true(
        any("rule=forbidden_delay" in v for v in violations),
        f"definition after a declaration must still be scanned: {violations}",
    )


def test_hot_path_contract_passes_on_the_live_tree() -> None:
    """The Tier-0 hot-path gate runs over real source and must be green."""
    script = "check_ble_hot_path_contract.py"
    result = subprocess.run(
        [sys.executable, str(ROOT / "scripts" / script)],
        capture_output=True,
        text=True,
    )
    assert_equal(result.returncode, 0, f"{script} should pass on a clean tree:\n{result.stdout}")


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="ble_tier0_guards_") as tmp:
        tmpdir = Path(tmp)
        test_deletion_guard_passes_on_clean_tree(tmpdir / "clean")
        test_deletion_guard_catches_banned_calls(tmpdir / "banned")
        test_deletion_guard_sees_past_line_comments(tmpdir / "line_comment")
        test_deletion_guard_reports_true_line_numbers(tmpdir / "line_numbers")
        test_deletion_guard_ignores_commented_and_quoted_mentions(tmpdir / "docs")
        test_deletion_guard_sees_past_comment_markers_in_strings(tmpdir / "strings")
        test_deletion_guard_restricts_delete_all_bonds_by_file(tmpdir / "bonds")
        test_deletion_guard_scans_headers(tmpdir / "headers")

    test_hot_path_detects_every_forbidden_pattern()
    test_hot_path_ignores_comments_and_strings()
    test_hot_path_flags_missing_callback_body()
    test_hot_path_skips_declarations()
    test_hot_path_contract_passes_on_the_live_tree()

    print("[ble-tier0-guards] regression tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Regressions for the fail-closed bug-squash build evidence generator."""

from __future__ import annotations

import argparse
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from typing import Any, Callable, TextIO
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

import generate_bug_squash_build_evidence as generator  # type: ignore  # noqa: E402


PINNED_PLATFORMIO_VERSION = "PlatformIO Core, version 6.1.19"
PINNED_PLATFORMIO_UNAVAILABLE = "pinned PlatformIO toolchain unavailable"


def assert_true(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def pinned_platformio_toolchain() -> dict[str, Any] | None:
    tools = generator.qualification.current_build_tool_identity()
    platformio = tools.get("platformio")
    if not isinstance(platformio, dict) or platformio.get("version") != PINNED_PLATFORMIO_VERSION:
        return None
    return tools


def require_pinned_platformio_toolchain(
    probe: Callable[[], object | None], *, stream: TextIO
) -> bool:
    try:
        identity = probe()
    except (AssertionError, OSError, ValueError, subprocess.SubprocessError):
        identity = None
    if identity is not None:
        return True
    print(PINNED_PLATFORMIO_UNAVAILABLE, file=stream)
    return False


def test_missing_pinned_platformio_toolchain_reports_cleanly(tmpdir: Path) -> None:
    del tmpdir

    def unavailable() -> object:
        raise ValueError("simulated missing toolchain")

    stream = io.StringIO()
    assert_true(
        main(
            ["--with-live-toolchain"],
            toolchain_probe=unavailable,
            error_stream=stream,
        )
        == 1,
        "missing pinned toolchain did not fail closed",
    )
    assert_true(stream.getvalue() == PINNED_PLATFORMIO_UNAVAILABLE + "\n", stream.getvalue())
    assert_true("Traceback" not in stream.getvalue(), stream.getvalue())


def test_output_must_be_below_ignored_artifact_root(tmpdir: Path) -> None:
    artifact_root = tmpdir / ".artifacts"
    with mock.patch.object(generator, "ARTIFACT_ROOT", artifact_root):
        accepted = generator.require_ignored_output(str(artifact_root / "qualification"))
        assert_true(accepted.is_dir(), "ignored child is accepted")
        try:
            generator.require_ignored_output(str(tmpdir / "public-output"))
        except generator.GenerationError as exc:
            assert_true("ignored .artifacts" in str(exc), "outside path fails closed")
        else:
            raise AssertionError("outside artifact path was accepted")


def test_contract_preflight_requires_real_exact_environments(tmpdir: Path) -> None:
    del tmpdir
    contracts = [
        {
            "kind": "production",
            "implementation_status": "active",
            "blocker_code": None,
            "environment": "real-env",
            "build_command": ["pio", "run", "-e", "real-env"],
        }
    ]
    assert_true(
        generator.preflight_build_contracts(contracts, {"real-env"}) == contracts,
        "declared exact environment passes",
    )
    for candidate, declared, expected in (
        (contracts, set(), "not implemented"),
        (
            [dict(contracts[0], build_command=["pio", "run"])],
            {"real-env"},
            "exact environment",
        ),
        (contracts * 2, {"real-env"}, "unique"),
    ):
        try:
            generator.preflight_build_contracts(candidate, declared)
        except generator.GenerationError as exc:
            assert_true(expected in str(exc), expected)
        else:
            raise AssertionError(f"invalid preflight accepted: {expected}")


def test_current_profile_builds_only_active_contracts(tmpdir: Path) -> None:
    del tmpdir
    profile, errors = generator.qualification.load_pinned_profile()
    assert_true(profile is not None and errors == [], "pinned profile loads")
    assert profile is not None
    active = generator.preflight_build_contracts(
        profile["build_contracts"],
        generator.declared_platformio_environments(),
    )
    assert_true(
        [contract["kind"] for contract in active]
        == ["production", "hil-fault", "car-production"],
        "generator retains every released build contract",
    )
    assert_true(
        next(contract for contract in active if contract["kind"] == "hil-fault")[
            "environment"
        ]
        == "waveshare-349-hil",
        "released HIL build uses its exact declared environment",
    )

    malformed = [dict(contract) for contract in profile["build_contracts"]]
    hil = next(contract for contract in malformed if contract["kind"] == "hil-fault")
    hil["environment"] = "invented-hil"
    hil["build_command"] = ["pio", "run", "-e", "invented-hil"]
    try:
        generator.preflight_build_contracts(
            malformed,
            generator.declared_platformio_environments(),
        )
    except generator.GenerationError as exc:
        assert_true("not implemented" in str(exc), "unknown environment")
    else:
        raise AssertionError("active build with an unknown environment was accepted")


def test_evidence_index_entry_is_relative_and_content_bound(tmpdir: Path) -> None:
    output = tmpdir / "pack"
    binary = output / "binaries" / "production.bin"
    binary.parent.mkdir(parents=True)
    binary.write_bytes(b"image")
    entry = generator.evidence_entry(
        "firmware-production",
        "firmware-production",
        "binary",
        binary,
        output,
    )
    assert_true(entry["path"] == "binaries/production.bin", "relative path")
    original_sha = entry["sha256"]
    binary.write_bytes(b"different-image")
    assert_true(
        generator.qualification.file_sha256(binary) != original_sha,
        "content mutation changes digest",
    )
    json.dumps(entry)


def test_real_build_tool_identity_is_content_bound(tmpdir: Path) -> None:
    del tmpdir
    generator.qualification.current_build_tool_identity.cache_clear()
    tools = generator.qualification.current_build_tool_identity()
    generator.qualification.current_build_tool_identity.cache_clear()
    repeated_tools = generator.qualification.current_build_tool_identity()
    assert_true(tools == repeated_tools, "tool identity is stable across fresh inspection")
    assert_true(
        set(tools) == {
            "schema_version",
            "platformio",
            "python",
            "git",
            "esptool",
            "platformio_packages",
            "identity_sha256",
        },
        "tool identity schema is exact",
    )
    assert_true(
        tools["schema_version"] == 2
        and set(tools["platformio"])
        == {"sha256", "package_sha256", "root", "version"},
        "PlatformIO binds launcher, package bytes, and an authenticated source root",
    )
    assert_true(
        len(tools["platformio_packages"]["identity_sha256"]) == 64,
        "PlatformIO package graph has an authenticated aggregate identity",
    )
    unsigned = {
        key: value for key, value in tools.items() if key != "identity_sha256"
    }
    assert_true(
        tools["identity_sha256"]
        == generator.qualification.canonical_commitment(
            "v1simple.hil.build-tools.v1",
            unsigned,
        ),
        "aggregate tool identity binds every independently observed tool",
    )

    injected = {
        "BASH_ENV": "/tmp/injected-shell",
        "GIT_DIR": "/tmp/injected-git",
        "PLATFORMIO_BUILD_FLAGS": "-D FORGED_BUILD=1",
        "PYTHONPATH": "/tmp/injected-python",
        "CXXFLAGS": "-DFORGED_TOOLCHAIN=1",
    }
    with mock.patch.dict(os.environ, injected, clear=False):
        environment = generator.qualification.authoritative_tool_environment(
            Path(generator.shutil.which("pio") or "")
        )
    for key in injected:
        assert_true(key not in environment, f"build environment retained {key}")


def test_default_regression_selection_does_not_require_live_toolchain(
    tmpdir: Path,
) -> None:
    del tmpdir
    deterministic = regression_tests(with_live_toolchain=False)
    qualification_only = regression_tests(with_live_toolchain=True)
    assert_true(
        test_real_build_tool_identity_is_content_bound not in deterministic,
        "development regressions selected the live qualification toolchain",
    )
    assert_true(
        test_real_build_tool_identity_is_content_bound in qualification_only,
        "qualification regressions omitted the live toolchain contract",
    )


def regression_tests(
    *,
    with_live_toolchain: bool,
) -> tuple[Callable[[Path], None], ...]:
    deterministic = (
        test_missing_pinned_platformio_toolchain_reports_cleanly,
        test_output_must_be_below_ignored_artifact_root,
        test_contract_preflight_requires_real_exact_environments,
        test_current_profile_builds_only_active_contracts,
        test_evidence_index_entry_is_relative_and_content_bound,
        test_default_regression_selection_does_not_require_live_toolchain,
    )
    if with_live_toolchain:
        return (*deterministic, test_real_build_tool_identity_is_content_bound)
    return deterministic


def main(
    argv: list[str] | None = None,
    *,
    toolchain_probe: Callable[[], object | None] = pinned_platformio_toolchain,
    error_stream: TextIO = sys.stderr,
) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--with-live-toolchain",
        action="store_true",
        help="also verify the qualification host's authenticated PlatformIO installation",
    )
    args = parser.parse_args(argv)
    if args.with_live_toolchain and not require_pinned_platformio_toolchain(
        toolchain_probe,
        stream=error_stream,
    ):
        return 1
    tests = regression_tests(
        with_live_toolchain=args.with_live_toolchain,
    )
    with tempfile.TemporaryDirectory(prefix="bug_squash_build_evidence_") as tmp:
        root = Path(tmp)
        for index, test in enumerate(tests):
            case_dir = root / f"case-{index}"
            case_dir.mkdir()
            test(case_dir)
    print(f"[bug-squash-build-evidence] {len(tests)} regression groups passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())

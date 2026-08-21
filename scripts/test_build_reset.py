#!/usr/bin/env python3
"""Focused regression tests for build.sh --reset."""

from __future__ import annotations

import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]


def assert_true(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def write_executable(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")
    path.chmod(0o755)


def run_reset(
    *, confirmation: str, fail_build: bool = False
) -> tuple[subprocess.CompletedProcess[str], list[str], bool]:
    temp_dir = tempfile.TemporaryDirectory()
    fixture = Path(temp_dir.name)

    shutil.copy2(ROOT / "build.sh", fixture / "build.sh")
    (fixture / "scripts").mkdir()
    (fixture / "scripts" / "platformio_ca_bundle.sh").write_text(":\n", encoding="utf-8")
    (fixture / "scripts" / "check_platformio_core_version.py").write_text("raise SystemExit(0)\n", encoding="utf-8")
    for relative in (".pio/build", "interface/build", "interface/.svelte-kit", "interface/node_modules"):
        (fixture / relative).mkdir(parents=True)
    (fixture / ".pio/build/sentinel").write_text("stale\n", encoding="utf-8")

    fake_bin = fixture / "fake-bin"
    log_path = fixture / "pio.log"
    write_executable(
        fake_bin / "pio",
        """#!/bin/sh
printf '%s\\n' "$*" >> "$BUILD_RESET_TEST_LOG"
if [ "${BUILD_RESET_FAIL_BUILD:-0}" = "1" ] && [ "$*" = "run -e waveshare-349" ]; then
    exit 42
fi
exit 0
""",
    )
    write_executable(fake_bin / "npm", "#!/bin/sh\nexit 0\n")
    write_executable(fake_bin / "sleep", "#!/bin/sh\nexit 0\n")
    write_executable(
        fake_bin / "python3",
        f"""#!/bin/sh
if [ "${{1:-}}" = "-" ]; then
    sed -n '1,$p' >/dev/null
    exit 1
fi
exec {shlex.quote(sys.executable)} "$@"
""",
    )

    env = os.environ.copy()
    env["PATH"] = f"{fake_bin}{os.pathsep}{env['PATH']}"
    env["PIO_CMD"] = str(fake_bin / "pio")
    env["BUILD_RESET_TEST_LOG"] = str(log_path)
    if fail_build:
        env["BUILD_RESET_FAIL_BUILD"] = "1"

    result = subprocess.run(
        ["bash", str(fixture / "build.sh"), "--reset"],
        cwd=fixture,
        env=env,
        input=confirmation,
        text=True,
        capture_output=True,
        check=False,
    )
    calls = log_path.read_text(encoding="utf-8").splitlines() if log_path.exists() else []
    host_build_was_cleaned = not (fixture / ".pio/build").exists()
    temp_dir.cleanup()
    return result, calls, host_build_was_cleaned


def test_reset_contract() -> None:
    result, calls, host_build_was_cleaned = run_reset(confirmation="yes\n")
    assert_true(result.returncode == 0, result.stdout + result.stderr)
    assert_true(
        calls
        == [
            "run -e waveshare-349",
            "run -e waveshare-349 -t size",
            "run -e waveshare-349 -t buildfs",
            "run -e waveshare-349 -t erase",
            "run -e waveshare-349 -t upload",
            "run -e waveshare-349 -t uploadfs",
            "device monitor -e waveshare-349",
        ],
        f"unexpected reset command order: {calls}",
    )
    assert_true(host_build_was_cleaned, "--reset did not clean host build artifacts")
    assert_true("Keep the SD card out" in result.stdout, "post-reset SD warning missing")


def test_reset_rejects_missing_confirmation() -> None:
    result, calls, _ = run_reset(confirmation="\n")
    assert_true(result.returncode != 0, "reset continued without confirmation")
    assert_true(not calls, f"reset touched PlatformIO after cancellation: {calls}")
    assert_true("nothing was erased" in result.stdout, "cancellation was not explicit")


def test_build_failure_happens_before_erase() -> None:
    result, calls, _ = run_reset(confirmation="y\n", fail_build=True)
    assert_true(result.returncode != 0, "simulated build failure was ignored")
    assert_true(calls == ["run -e waveshare-349"], f"erase ran after failed build: {calls}")


def test_build_reuses_frontend_and_firmware_outputs() -> None:
    source = (ROOT / "build.sh").read_text(encoding="utf-8")
    assert_true("npm run deploy:built" in source, "build.sh must deploy the frontend it just built")
    assert_true("npm run deploy\n" not in source, "build.sh still rebuilds the frontend during deploy")
    assert_true("--target buildprog" not in source, "--test still rebuilds firmware after the normal build")


def test_build_uses_the_authoritative_native_runner() -> None:
    source = (ROOT / "build.sh").read_text(encoding="utf-8")
    assert_true(
        'python3 "$SCRIPT_DIR/scripts/run_native_tests_serial.py"' in source,
        "--test bypasses the isolated native-suite runner",
    )
    assert_true('"$PIO_CMD" test -e native' not in source, "--test still uses aggregate PlatformIO tests")


def main() -> int:
    test_reset_contract()
    test_reset_rejects_missing_confirmation()
    test_build_failure_happens_before_erase()
    test_build_reuses_frontend_and_firmware_outputs()
    test_build_uses_the_authoritative_native_runner()
    print("build reset tests: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Focused behavior tests for the destructive and filesystem build paths."""

from __future__ import annotations

import json
import os
from pathlib import Path
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
    *,
    confirmation: str,
    fail_build: bool = False,
    arguments: tuple[str, ...] = ("--reset",),
    reset_available: bool = False,
    inherited_upload_port: str = "",
) -> tuple[subprocess.CompletedProcess[str], list[list[str]], bool]:
    temp_dir = tempfile.TemporaryDirectory()
    fixture = Path(temp_dir.name)

    shutil.copy2(ROOT / "build.sh", fixture / "build.sh")
    (fixture / "scripts").mkdir()
    (fixture / "scripts" / "platformio_ca_bundle.sh").write_text(":\n", encoding="utf-8")
    (fixture / "scripts" / "check_platformio_core_version.py").write_text("raise SystemExit(0)\n", encoding="utf-8")
    (fixture / "scripts" / "prepare_web_build.py").write_text("raise SystemExit(0)\n", encoding="utf-8")
    for relative in (".pio/build", "interface/build", "interface/.svelte-kit", "interface/node_modules"):
        (fixture / relative).mkdir(parents=True)
    (fixture / ".pio/build/sentinel").write_text("stale\n", encoding="utf-8")

    fake_bin = fixture / "fake-bin"
    log_path = fixture / "pio.log"
    write_executable(
        fake_bin / "pio",
        f"""#!{sys.executable}
import json
import os
import sys
with open(os.environ["BUILD_RESET_TEST_LOG"], "a", encoding="utf-8") as log:
    log.write(json.dumps(sys.argv[1:]) + "\\n")
    if os.environ["BUILD_RESET_FAKE_SERIAL"] == "1" and "upload" in sys.argv:
        port = (sys.argv[sys.argv.index("--upload-port") + 1]
                if "--upload-port" in sys.argv else os.environ["PLATFORMIO_UPLOAD_PORT"] or "auto")
        log.write(json.dumps(["upload-target", port]) + "\\n")
if os.environ.get("BUILD_RESET_FAIL_BUILD") == "1" and sys.argv[1:] == ["run", "-e", "waveshare-349"]:
    raise SystemExit(42)
""",
    )
    write_executable(fake_bin / "npm", "#!/bin/sh\nexit 0\n")
    write_executable(fake_bin / "sleep", "#!/bin/sh\nexit 0\n")
    write_executable(
        fake_bin / "python3",
        f"""#!{sys.executable}
import json
import os
import subprocess
import sys
if sys.argv[1] == "-":
    sys.stdin.read()
    if os.environ["BUILD_RESET_FAKE_SERIAL"] != "1":
        raise SystemExit(1)
    if len(sys.argv) > 2:
        with open(os.environ["BUILD_RESET_TEST_LOG"], "a", encoding="utf-8") as log:
            log.write(json.dumps(["reset", sys.argv[2]]) + "\\n")
    raise SystemExit(0)
raise SystemExit(subprocess.call([{sys.executable!r}, *sys.argv[1:]]))
""",
    )
    write_executable(
        fake_bin / "ls",
        f"""#!{sys.executable}
import json
import os
with open(os.environ["BUILD_RESET_TEST_LOG"], "a", encoding="utf-8") as log:
    log.write(json.dumps(["enumerate-reset-ports"]) + "\\n")
print("/dev/cu.usbmodemUNRELATEDXXX")
print("/dev/cu.usbmodemTARGETXXX")
""",
    )

    env = os.environ.copy()
    env["PATH"] = f"{fake_bin}{os.pathsep}{env['PATH']}"
    env["PIO_CMD"] = str(fake_bin / "pio")
    env["BUILD_RESET_TEST_LOG"] = str(log_path)
    env["BUILD_RESET_FAKE_SERIAL"] = "1" if reset_available else "0"
    env["PLATFORMIO_UPLOAD_PORT"] = inherited_upload_port
    if fail_build:
        env["BUILD_RESET_FAIL_BUILD"] = "1"

    result = subprocess.run(
        ["bash", str(fixture / "build.sh"), *arguments],
        cwd=fixture,
        env=env,
        input=confirmation,
        text=True,
        capture_output=True,
        check=False,
    )
    calls = [json.loads(line) for line in log_path.read_text(encoding="utf-8").splitlines()] if log_path.exists() else []
    host_build_was_cleaned = not (fixture / ".pio/build").exists()
    temp_dir.cleanup()
    return result, calls, host_build_was_cleaned


def test_reset_contract() -> None:
    result, calls, host_build_was_cleaned = run_reset(confirmation="yes\n")
    assert_true(result.returncode == 0, result.stdout + result.stderr)
    assert_true(
        calls
        == [
            ["run", "-e", "waveshare-349"],
            ["run", "-e", "waveshare-349", "-t", "size"],
            ["run", "-e", "waveshare-349", "-t", "buildfs"],
            ["run", "-e", "waveshare-349", "-t", "erase"],
            ["run", "-e", "waveshare-349", "-t", "upload"],
            ["run", "-e", "waveshare-349", "-t", "uploadfs"],
            ["device", "monitor", "-e", "waveshare-349"],
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
    assert_true(calls == [["run", "-e", "waveshare-349"]], f"erase ran after failed build: {calls}")


def test_skip_web_rejects_missing_filesystem_data() -> None:
    result, calls, _ = run_reset(
        confirmation="",
        arguments=("--skip-web", "--upload-fs"),
    )
    assert_true(result.returncode != 0, "missing web files were accepted for uploadfs")
    assert_true(not calls, f"PlatformIO ran before missing web files were rejected: {calls}")
    assert_true("data/index.html is missing or empty" in result.stderr, result.stderr)


def test_build_reuses_frontend_and_firmware_outputs() -> None:
    source = (ROOT / "build.sh").read_text(encoding="utf-8")
    assert_true("npm run deploy:built" in source, "build.sh must deploy the frontend it just built")
    assert_true("npm run deploy\n" not in source, "build.sh still rebuilds the frontend during deploy")


def test_option_values_remain_single_arguments() -> None:
    for option, value in (("--env", "waveshare-349 -t erase"),
                          ("--upload-port", "test port -t erase *")):
        result, calls, _ = run_reset(
            confirmation="",
            arguments=("--skip-web", option, value, "--jobs", "2", "--monitor"),
        )
        assert_true(result.returncode == 0, result.stdout + result.stderr)
        build_args = ["-e", value] if option == "--env" else ["-e", "waveshare-349", "--upload-port", value]
        monitor_args = ["-e", value] if option == "--env" else ["-e", "waveshare-349", "--port", value]
        assert_true(calls == [
            ["run", *build_args, "-j", "2"],
            ["run", *build_args, "-j", "2", "-t", "size"],
            ["device", "monitor", *monitor_args],
        ], f"option value became separate PlatformIO arguments: {calls}")


def test_reset_preserves_port_and_job_arguments() -> None:
    port = "test port -t erase *"
    result, calls, _ = run_reset(
        confirmation="yes\n",
        arguments=("--reset", "--upload-port", port, "--jobs", "2"),
    )
    assert_true(result.returncode == 0, result.stdout + result.stderr)
    base = ["run", "-e", "waveshare-349", "--upload-port", port]
    assert_true(calls == [
        [*base, "-j", "2"],
        [*base, "-j", "2", "-t", "size"],
        [*base, "-j", "2", "-t", "buildfs"],
        [*base, "-t", "erase"],
        [*base, "-j", "2", "-t", "upload"],
        [*base, "-j", "2", "-t", "uploadfs"],
        ["device", "monitor", "-e", "waveshare-349", "--port", port],
    ], f"reset or monitor split option values: {calls}")


def test_help_does_not_require_platformio() -> None:
    env = os.environ.copy()
    env["PATH"] = "/usr/bin:/bin"
    env.pop("PIO_CMD", None)
    result = subprocess.run(
        ["/bin/bash", str(ROOT / "build.sh"), "--help"],
        cwd=ROOT,
        env=env,
        text=True,
        capture_output=True,
        check=False,
    )
    assert_true(result.returncode == 0, result.stdout + result.stderr)
    assert_true("Usage:" in result.stdout, result.stdout)
    assert_true("PlatformIO not found" not in result.stdout + result.stderr, result.stdout + result.stderr)


def test_supplemental_reset_uses_only_a_concrete_upload_target() -> None:
    for explicit, inherited, expected in (
        ("", "/dev/cu.usbmodemTARGETXXX", "/dev/cu.usbmodemTARGETXXX"),
        ("/dev/cu.usbmodemTARGETXXX", "", "/dev/cu.usbmodemTARGETXXX"),
        ("/dev/cu.usbmodemTARGETXXX", "/dev/cu.usbmodemOTHERXXX", "/dev/cu.usbmodemTARGETXXX"),
        ("", "", ""),
        ("", "/dev/cu.usbmodem*", ""),
        ("/dev/cu.usbmodem?", "/dev/cu.usbmodemOTHERXXX", ""),
        ("/dev/cu.usbmodem[AB]", "", ""),
    ):
        arguments = ("--skip-web", "--upload")
        if explicit:
            arguments += ("--upload-port", explicit)
        result, calls, _ = run_reset(
            confirmation="",
            arguments=arguments,
            reset_available=True,
            inherited_upload_port=inherited,
        )
        context = f"explicit={explicit!r}, inherited={inherited!r}"
        assert_true(result.returncode == 0, f"{context}: {result.stdout}{result.stderr}")
        assert_true(["enumerate-reset-ports"] not in calls, f"{context}: enumerated unrelated ports")
        base = ["run", "-e", "waveshare-349"]
        if explicit:
            base += ["--upload-port", explicit]
        expected_calls = [base, [*base, "-t", "size"], [*base, "-t", "upload"],
                          ["upload-target", explicit or inherited or "auto"]]
        if expected:
            expected_calls.append(["reset", expected])
        assert_true(calls == expected_calls, f"{context}: unexpected target or upload arguments: {calls}")
        if not expected:
            assert_true("skipping post-upload reset" in result.stdout, f"{context}: skip was not reported")


def main() -> int:
    test_reset_contract()
    test_reset_rejects_missing_confirmation()
    test_build_failure_happens_before_erase()
    test_skip_web_rejects_missing_filesystem_data()
    test_build_reuses_frontend_and_firmware_outputs()
    test_option_values_remain_single_arguments()
    test_reset_preserves_port_and_job_arguments()
    test_help_does_not_require_platformio()
    test_supplemental_reset_uses_only_a_concrete_upload_target()
    print("build reset tests: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

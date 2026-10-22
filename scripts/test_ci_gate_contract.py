#!/usr/bin/env python3
"""Regression tests for release-critical local and hosted CI sequencing."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def test_car_build_is_clean_and_uses_one_argument_contract() -> None:
    source = (ROOT / "scripts" / "ci-test.sh").read_text(encoding="utf-8")
    declaration = "CAR_BUILD_ARGS=(-e esp32-s3-car-install)"
    jobs_extension = 'CAR_BUILD_ARGS+=(-j "$PIO_JOBS")'
    install = (
        'run_step "Car install dependencies" "$PIO_CMD" pkg install '
        '-e esp32-s3-car-install'
    )
    clean = (
        'run_step "Car install firmware clean" "$PIO_CMD" run '
        '"${CAR_BUILD_ARGS[@]}" -t clean'
    )
    build = (
        'run_step "Car install firmware build" "$PIO_CMD" run '
        '"${CAR_BUILD_ARGS[@]}"'
    )

    require(source.count(declaration) == 1, "car environment arguments are not singular")
    require(source.count(jobs_extension) == 1, "car build no longer shares the CI job limit")
    require(source.count(install) == 1, "CI must resolve car dependencies before cleaning")
    require(source.count(clean) == 1, "CI must clean the car firmware before building")
    require(source.count(build) == 1, "CI must build the car firmware after cleaning")
    require(source.index(install) < source.index(clean),
            "car dependencies must be present before fail-closed patch hooks run during clean")
    require(source.index(clean) < source.index(build), "car clean must precede car build")


def test_current_frontend_bundle_precedes_firmware_metadata_and_car_build() -> None:
    source = (ROOT / "scripts" / "ci-test.sh").read_text(encoding="utf-8")
    frontend_steps = (
        'run_step "Frontend dependencies" bash -c \'cd interface && npm ci\'',
        'run_step "Frontend dependency audit" bash -c \'cd interface && npm audit --audit-level=high\'',
        'run_step "Frontend lint and type checks" bash -c \'cd interface && npm run lint\'',
        'run_step "Frontend unit tests" bash -c \'cd interface && npm test\'',
        'run_step "Frontend bundle" bash -c \'cd interface && npm run build\'',
    )
    previous = -1
    for step in frontend_steps:
        require(source.count(step) == 1, f"CI must run this frontend step once: {step}")
        require(source.index(step) > previous, "frontend validation/build order changed")
        previous = source.index(step)
    # pio check calls load_build_metadata, which executes __idedata and all
    # production pre-hooks even though it does not compile firmware. A warm
    # developer checkout used to hide this dependency on the ignored stamp.
    for consumer in (
        'run_step "Firmware static analysis" "$PIO_CMD" check -e waveshare-349 --fail-on-defect=medium',
        'run_step "Car install firmware build" "$PIO_CMD" run "${CAR_BUILD_ARGS[@]}"',
    ):
        require(source.count(consumer) == 1, f"missing firmware consumer: {consumer}")
        require(previous < source.index(consumer),
                "a clean checkout needs a verified frontend before firmware metadata/build hooks")


def test_release_publication_is_owned_by_tested_script() -> None:
    workflow = (ROOT / ".github" / "workflows" / "release.yml").read_text(
        encoding="utf-8"
    )
    invocation = "run: python3 scripts/release_publication.py"
    require(
        workflow.count(invocation) == 1,
        "release workflow must invoke the tested publication owner exactly once",
    )
    require("git push" not in workflow, "release workflow must not carry inline push policy")
    gate = (ROOT / "scripts" / "ci-test.sh").read_text(encoding="utf-8")
    require(
        gate.count("python3 scripts/test_release_publication.py") == 1,
        "authoritative gate must run publication decision regressions",
    )


def test_clean_linux_runner_binds_exact_source_and_cold_amd64_image() -> None:
    runner = (ROOT / "scripts" / "run_clean_linux_ci.sh").read_text(encoding="utf-8")
    require(
        "check_tracked_source_state.py" in runner,
        "clean Linux runner must reject tracked source drift",
    )
    require(
        "test \"$(git -C /work/repo rev-parse HEAD)\" = \"$SOURCE_SHA\"" in runner,
        "container checkout must match the recorded source commit",
    )
    require(
        "docker.io/library/ubuntu@sha256:" in runner,
        "clean Linux runner must use an immutable Ubuntu image",
    )
    require(
        "--platform linux/amd64 --pull=always" in runner,
        "clean Linux runner must match the hosted architecture and pull its pinned image",
    )
    require(
        "./scripts/bootstrap_linux_validation.sh ci" in runner,
        "clean Linux runner must consume the shared CI bootstrap",
    )
    bootstrap = "./scripts/bootstrap_linux_validation.sh ci"
    tool_path = 'export PATH="$V1_SWIFT_ROOT/usr/bin:$V1_PIO_VENV/bin:$PATH"'
    gate = "PLATFORMIO_RUN_JOBS=1 ./scripts/ci-test.sh"
    require(
        runner.count('export V1_PIO_VENV=/opt/v1-pio-venv') == 1,
        "clean Linux runner must give its CI tool environment one explicit owner",
    )
    require(
        runner.count('export V1_SWIFT_ROOT=/opt/swift-6.3.3') == 1,
        "clean Linux runner must give its Swift toolchain one explicit owner",
    )
    require(
        runner.count(tool_path) == 1
        and runner.index(bootstrap) < runner.index(tool_path) < runner.index(gate),
        "clean Linux runner must expose bootstrapped PlatformIO and Swift to the full gate",
    )
    require(
        "trap retain_reports EXIT" in runner
        and "cp -R .artifacts/test_reports /host-artifacts/test_reports" in runner,
        "clean Linux runner must retain available build reports even on failure",
    )


def test_linux_failure_bundle_retains_environment_and_build_reports() -> None:
    workflow = (ROOT / ".github" / "workflows" / "ci.yml").read_text(
        encoding="utf-8"
    )
    require(
        workflow.count("if: failure()") == 1,
        "Linux diagnostics must remain failure-only",
    )
    failure_bundle = workflow.split("- name: Upload log on failure", maxsplit=1)[1]
    for required_path in (
        "linux-bootstrap.log",
        "ci-test.log",
        ".artifacts/linux-validation-environment.txt",
        ".artifacts/test_reports",
    ):
        require(
            failure_bundle.count(required_path) == 1,
            f"Linux failure bundle must retain {required_path}",
        )
    require(
        failure_bundle.count("include-hidden-files: true") == 1,
        "artifact upload must include the explicitly selected .artifacts paths",
    )

    bootstrap = (ROOT / "scripts" / "bootstrap_linux_validation.sh").read_text(
        encoding="utf-8"
    )
    require(
        'VALIDATION_PYTHON="$PIO_VENV/bin/python"' in bootstrap,
        "CI environment verification must use the interpreter owning installed tools",
    )


def test_macos_local_and_hosted_gates_run_both_swift_suites() -> None:
    workflow = (ROOT / ".github" / "workflows" / "ci.yml").read_text(encoding="utf-8")
    macos_job = workflow.split("  v1replay-swift-oracle:", maxsplit=1)[1].split(
        "\n  test:", maxsplit=1
    )[0]
    require("runs-on: macos-15" in macos_job, "Swift suites require a macOS runner")
    for invocation in (
        "run: swift test --package-path tools/v1replay",
        "run: python3 tools/v1replay/Tests/test_peripheral_lifecycle.py",
    ):
        require(macos_job.count(invocation) == 1, f"macOS CI must run {invocation} once")
        step = next(step for step in macos_job.split("      - name:") if invocation in step)
        require(
            "DEVELOPER_DIR: /Applications/Xcode_16.4.app/Contents/Developer" in step,
            "Swift suites must use the hosted job's selected Xcode toolchain",
        )

    gate = (ROOT / "scripts" / "ci-test.sh").read_text(encoding="utf-8")
    host_tools = gate.split('section "Host Tools"', maxsplit=1)[1].split("\nelse", maxsplit=1)[0]
    require('if [[ "$(uname -s)" == "Darwin" ]]' in host_tools,
            "local Swift suites require macOS")
    for invocation in (
        'run_step "v1replay Swift package tests" run_v1replay_swift_tests',
        'run_step "v1replay peripheral lifecycle regression suite" run_peripheral_lifecycle_checks',
    ):
        require(host_tools.count(invocation) == 1, f"local macOS gate must run {invocation} once")
    require(gate.count('"${python_driver[@]}" "$ROOT_DIR/tools/v1replay/Tests/test_peripheral_lifecycle.py"') == 1,
            "local lifecycle helper must invoke the standalone regression runner")


def main() -> int:
    test_car_build_is_clean_and_uses_one_argument_contract()
    test_current_frontend_bundle_precedes_firmware_metadata_and_car_build()
    test_release_publication_is_owned_by_tested_script()
    test_clean_linux_runner_binds_exact_source_and_cold_amd64_image()
    test_linux_failure_bundle_retains_environment_and_build_reports()
    test_macos_local_and_hosted_gates_run_both_swift_suites()
    print("CI gate sequencing tests: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Focused regressions for HIL-only fault-control production exclusion."""

from __future__ import annotations

from pathlib import Path
import shutil
import tempfile
from types import SimpleNamespace

import check_bug_squash_hil_fault_controls as checker


ROOT = Path(__file__).resolve().parents[1]
FULL_SHA = "a" * 40


def assert_true(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def fixture_root(temporary: Path) -> Path:
    root = temporary / "repository"
    (root / "src" / "modules").mkdir(parents=True)
    shutil.copy2(ROOT / "platformio.ini", root / "platformio.ini")
    shutil.copytree(ROOT / "src" / "modules" / "hil", root / "src" / "modules" / "hil")
    for relative in checker.EXPECTED_BSC04_PRODUCT_HIL_FILES:
        destination = root / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / relative, destination)
    for relative in checker.EXPECTED_BSC05_PRODUCT_HIL_FILES:
        destination = root / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / relative, destination)
    for relative in checker.EXPECTED_BSC06_PRODUCT_HIL_FILES:
        destination = root / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / relative, destination)
    for relative in checker.EXPECTED_BSC16_PRODUCT_HIL_FILES:
        destination = root / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / relative, destination)
    for relative in checker.EXPECTED_BSC10_PRODUCT_HIL_FILES:
        destination = root / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / relative, destination)
    shutil.copy2(ROOT / checker.BSC16_BATTERY_MANAGER, root / checker.BSC16_BATTERY_MANAGER)
    shutil.copy2(ROOT / checker.BSC04_MAIN, root / checker.BSC04_MAIN)
    connection_state = root / checker.BSC05_CONNECTION_STATE
    connection_state.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(ROOT / checker.BSC05_CONNECTION_STATE, connection_state)
    shutil.copy2(ROOT / checker.BSC06_TRANSPORT, root / checker.BSC06_TRANSPORT)
    shutil.copy2(ROOT / checker.BSC10_WIFI_CLIENT, root / checker.BSC10_WIFI_CLIENT)
    shutil.copy2(ROOT / checker.BSC10_TRANSACTION, root / checker.BSC10_TRANSACTION)
    for relative in checker.RELEASE_CONFIGURATION_FILES:
        destination = root / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / relative, destination)
    ci_test = root / checker.CI_TEST_FILE
    ci_test.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(ROOT / checker.CI_TEST_FILE, ci_test)
    ci_workflow = root / checker.CI_WORKFLOW_FILE
    ci_workflow.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(ROOT / checker.CI_WORKFLOW_FILE, ci_workflow)
    return root


def assert_error_contains(errors: list[str], fragment: str) -> None:
    assert_true(any(fragment in error for error in errors), f"missing error {fragment!r}: {errors}")


def test_repository_static_contract_passes() -> None:
    errors = checker.validate_static(ROOT)
    assert_true(not errors, f"repository static contract failed: {errors}")


def test_structural_guard_accepts_formatter_comment_spacing() -> None:
    for separator in (" ", "  "):
        source = (
            "#pragma once\n"
            "#if defined(V1SIMPLE_HIL_FAULT_CONTROL)\n"
            "int guarded = 1;\n"
            f"#endif{separator}// V1SIMPLE_HIL_FAULT_CONTROL\n"
        )
        assert_true(
            checker.has_structural_outer_guard(source),
            f"outer guard rejected {len(separator)}-space formatter variant",
        )
    assert_true(
        not checker.has_structural_outer_guard(
            "#if defined(V1SIMPLE_HIL_FAULT_CONTROL)\nint guarded = 1;\n#endif\n"
        ),
        "outer guard accepted an unauthenticated closing directive",
    )


def test_hil_environment_cannot_become_default_or_leak_to_production_flags() -> None:
    with tempfile.TemporaryDirectory(prefix="hil-fault-controls-") as raw:
        root = fixture_root(Path(raw))
        platformio = root / "platformio.ini"
        original = platformio.read_text(encoding="utf-8")
        platformio.write_text(
            original.replace("default_envs = waveshare-349", "default_envs = waveshare-349-hil"),
            encoding="utf-8",
        )
        assert_error_contains(checker.validate_static(root), "must not be a default")

        platformio.write_text(
            original.replace(
                "build_flags = \n",
                "build_flags = \n    -D V1SIMPLE_HIL_FAULT_CONTROL=1\n",
                1,
            ),
            encoding="utf-8",
        )
        assert_error_contains(checker.validate_static(root), "production environment defines HIL macro")

        platformio.write_text(
            original + "\n[env]\nbuild_flags = -D V1SIMPLE_HIL_FAULT_CONTROL=1\n",
            encoding="utf-8",
        )
        assert_error_contains(checker.validate_static(root), "non-HIL PlatformIO section defines HIL macro")

        platformio.write_text(
            original.replace(
                "    ${env:waveshare-349.build_flags}\n    -D CAR_MODE_PWR_SHORT=1",
                "    ${env:waveshare-349-hil.build_flags}\n    -D CAR_MODE_PWR_SHORT=1",
            ),
            encoding="utf-8",
        )
        assert_error_contains(
            checker.validate_static(root),
            "production environment references or inherits HIL environment",
        )

        platformio.write_text(
            original.replace(
                "[env:esp32-s3-car-install]\n",
                "[env:esp32-s3-car-install]\nextends =\n"
                "    env:waveshare-349\n    env:waveshare-349-hil\n",
            ),
            encoding="utf-8",
        )
        assert_error_contains(
            checker.validate_static(root),
            "production environment references or inherits HIL environment",
        )

        platformio.write_text(
            original.replace(
                "    ${env:waveshare-349.build_flags}\n    -D CAR_MODE_PWR_SHORT=1",
                "    ${env:hil-leak-base.build_flags}\n    -D CAR_MODE_PWR_SHORT=1",
            )
            + "\n[env:hil-leak-base]\nextends = env:waveshare-349-hil\n"
            "build_flags = ${env:waveshare-349-hil.build_flags}\n",
            encoding="utf-8",
        )
        assert_error_contains(
            checker.validate_static(root),
            "production environment references or inherits HIL environment",
        )

        platformio.write_text(original, encoding="utf-8")
        release_workflow = root / ".github" / "workflows" / "release.yml"
        release_workflow.write_text(
            release_workflow.read_text(encoding="utf-8") + "\n# waveshare-349-hil\n",
            encoding="utf-8",
        )
        assert_error_contains(checker.validate_static(root), "referenced by release configuration")

        platformio.write_text(original, encoding="utf-8")
        ci_test = root / checker.CI_TEST_FILE
        ci_test.write_text(
            ci_test.read_text(encoding="utf-8").replace(
                checker.CI_AUTHORITATIVE_GATE, "# authoritative HIL gate removed"
            ),
            encoding="utf-8",
        )
        assert_error_contains(checker.validate_static(root), "must run exactly once immediately")

        ci_test.write_text((ROOT / checker.CI_TEST_FILE).read_text(encoding="utf-8"), encoding="utf-8")
        ci_workflow = root / checker.CI_WORKFLOW_FILE
        ci_workflow.write_text(
            ci_workflow.read_text(encoding="utf-8").replace(
                '"platformio==6.1.19"', '"platformio>=6.1.19,<7"'
            ),
            encoding="utf-8",
        )
        assert_error_contains(checker.validate_static(root), "install exactly the pinned")


def test_unguarded_and_elif_call_sites_are_rejected() -> None:
    with tempfile.TemporaryDirectory(prefix="hil-fault-controls-") as raw:
        root = fixture_root(Path(raw))
        call_site = root / "src" / "escaped.cpp"
        call_site.write_text("HilFaultController* escaped_controller;\n", encoding="utf-8")
        assert_error_contains(checker.validate_static(root), "unguarded HIL call site")

        call_site.write_text(
            "#if defined(V1SIMPLE_HIL_FAULT_CONTROL)\n"
            "HilFaultController* guarded_controller;\n"
            "#elif defined(OTHER_FEATURE)\n"
            "HilFaultController* escaped_elif_controller;\n"
            "#endif\n",
            encoding="utf-8",
        )
        errors = checker.validate_static(root)
        assert_error_contains(errors, "unguarded HIL call site: src/escaped.cpp:4")

        call_site.write_text(
            "#if defined(OTHER_FEATURE)\n"
            "int unrelated;\n"
            "#elif defined(V1SIMPLE_HIL_FAULT_CONTROL)\n"
            "HilFaultController* guarded_elif_controller;\n"
            "#endif\n",
            encoding="utf-8",
        )
        errors = checker.validate_static(root)
        assert_true(not errors, f"positive HIL #elif was misclassified: {errors}")

        for suffix in (".cc", ".ino"):
            escaped = root / "src" / f"escaped{suffix}"
            escaped.write_text("HilFaultController* escaped;\n", encoding="utf-8")
            assert_error_contains(checker.validate_static(root), f"unguarded HIL call site: src/escaped{suffix}")
            escaped.unlink()


def test_hil_inventory_guard_symlink_and_forbidden_runtime_are_rejected() -> None:
    with tempfile.TemporaryDirectory(prefix="hil-fault-controls-") as raw:
        root = fixture_root(Path(raw))
        header = root / "src" / "modules" / "hil" / "hil_ready_barrier.h"
        original = header.read_text(encoding="utf-8")
        header.write_text(original.replace("#if defined(V1SIMPLE_HIL_FAULT_CONTROL)\n", ""), encoding="utf-8")
        assert_error_contains(checker.validate_static(root), "not enclosed by the compile guard")

        header.write_text(
            "HilFaultController* before_guard;\n" + original,
            encoding="utf-8",
        )
        assert_error_contains(checker.validate_static(root), "not enclosed by the compile guard")

        header.write_text(original.replace("#include", "void* leak = malloc(4);\n#include", 1), encoding="utf-8")
        assert_error_contains(checker.validate_static(root), "forbidden dynamic")

        header.unlink()
        header.symlink_to(root / "platformio.ini")
        assert_error_contains(checker.validate_static(root), "must not be a symlink")


def test_bsc16_hook_rejects_direct_hardware_access_and_wiring_drift() -> None:
    with tempfile.TemporaryDirectory(prefix="hil-fault-controls-") as raw:
        root = fixture_root(Path(raw))
        hook = root / "src" / "modules" / "power" / "battery_bsc16_hil_fault_module.cpp"
        original = hook.read_text(encoding="utf-8")
        hook.write_text(
            original.replace("#include <cstdio>\n", "#include <cstdio>\nvoid escaped() { digitalWrite(1, 1); }\n"),
            encoding="utf-8",
        )
        assert_error_contains(checker.validate_static(root), "mutates hardware directly")

        hook.write_text(original, encoding="utf-8")
        manager = root / checker.BSC16_BATTERY_MANAGER
        manager.write_text(
            manager.read_text(encoding="utf-8").replace(
                "adcInitialized = initADC();",
                "adcInitialized = initADC(); // removed admission ordering token",
                1,
            ).replace(
                "batteryBsc16HilFaultModule().beginAdcAdmission(",
                "batteryBsc16HilFaultModule().missingAdmission(",
            ),
            encoding="utf-8",
        )
        assert_error_contains(checker.validate_static(root), "admission wiring must contain exactly one")


def test_bsc04_hook_rejects_direct_hardware_access_and_wiring_drift() -> None:
    with tempfile.TemporaryDirectory(prefix="hil-fault-controls-") as raw:
        root = fixture_root(Path(raw))
        hook = root / "src" / "modules" / "system" / "connection_bsc04_hil_fault_module.cpp"
        original = hook.read_text(encoding="utf-8")
        hook.write_text(
            original.replace("#include <cstdio>\n", "#include <cstdio>\nvoid escaped() { digitalWrite(1, 1); }\n"),
            encoding="utf-8",
        )
        assert_error_contains(checker.validate_static(root), "mutates hardware directly")

        hook.write_text(original, encoding="utf-8")
        main = root / checker.BSC04_MAIN
        main.write_text(
            main.read_text(encoding="utf-8").replace(
                "connectionBsc04HilFaultModule().routeVerifyPushMatchEdge(",
                "connectionBsc04HilFaultModule().bypassVerifyPushMatchEdge(",
                1,
            ),
            encoding="utf-8",
        )
        assert_error_contains(checker.validate_static(root), "routing wiring must contain exactly one")


def test_bsc05_hook_rejects_missing_or_misordered_generation_wiring() -> None:
    with tempfile.TemporaryDirectory(prefix="hil-fault-controls-") as raw:
        root = fixture_root(Path(raw))
        main = root / checker.BSC05_MAIN
        original = main.read_text(encoding="utf-8")
        main.write_text(
            original.replace(
                "bleBsc05HilFaultModule().routeNotification(",
                "bleBsc05HilFaultModule().bypassNotification(",
                1,
            ),
            encoding="utf-8",
        )
        assert_error_contains(checker.validate_static(root), "notification wiring must contain exactly one")

        main.write_text(original, encoding="utf-8")
        connection = root / checker.BSC05_CONNECTION_STATE
        original_connection = connection.read_text(encoding="utf-8")
        connection.write_text(
            original_connection.replace(
                "bleBsc05HilFaultModule().recordSessionOpened(sessionGeneration, millis());",
                "bleBsc05HilFaultModule().recordSessionOpenedBeforeQueue(sessionGeneration, millis());",
                1,
            ),
            encoding="utf-8",
        )
        assert_error_contains(checker.validate_static(root), "lifecycle wiring must contain exactly one")

        queue_open_block = """    if (bleQueue_) {
        bleQueue_->openSession(sessionGeneration);
    }
"""
        record_open_block = """#if defined(V1SIMPLE_HIL_FAULT_CONTROL)
    bleBsc05HilFaultModule().recordSessionOpened(sessionGeneration, millis());
#endif
"""
        connection.write_text(
            original_connection.replace(
                queue_open_block + record_open_block,
                record_open_block + queue_open_block,
                1,
            ),
            encoding="utf-8",
        )
        assert_error_contains(checker.validate_static(root), "new-session signal must follow queue generation admission")


def test_bsc06_hook_rejects_missing_or_preclaim_transport_routing() -> None:
    with tempfile.TemporaryDirectory(prefix="hil-fault-controls-") as raw:
        root = fixture_root(Path(raw))
        transport = root / checker.BSC06_TRANSPORT
        original = transport.read_text(encoding="utf-8")
        route_token = "obdBsc06HilFaultModule().routeOperation("
        transport.write_text(original.replace(route_token, "obdBsc06HilFaultModule().bypassOperation(", 1),
                             encoding="utf-8")
        assert_error_contains(checker.validate_static(root), "operation barrier wiring must contain exactly one")

        claim_start = original.index("        if (!sObdTransport.requestEpoch.tryClaim(request.dispatchEpoch))")
        claim_end = original.index("\n\n", claim_start) + 2
        claim_block = original[claim_start:claim_end]
        route_start = original.index("#if defined(V1SIMPLE_HIL_FAULT_CONTROL)", claim_end)
        route_end = original.index("#endif", route_start) + len("#endif\n")
        route_block = original[route_start:route_end]
        reordered = original[:claim_start] + route_block + claim_block + original[route_end:]
        transport.write_text(reordered, encoding="utf-8")
        assert_error_contains(checker.validate_static(root), "after request-epoch claim and before GATT dispatch")


def test_bsc10_hook_rejects_missing_or_late_admission() -> None:
    with tempfile.TemporaryDirectory(prefix="hil-fault-controls-") as raw:
        root = fixture_root(Path(raw))
        transaction = root / checker.BSC10_TRANSACTION
        original = transaction.read_text(encoding="utf-8")
        transaction.write_text(
            original.replace(
                "if (runtime.admitStart && !runtime.admitStart(runtime.ctx))",
                "if (false && runtime.admitStart && !runtime.admitStart(runtime.ctx))",
                1,
            ),
            encoding="utf-8",
        )
        assert_error_contains(checker.validate_static(root), "admission hook must occur exactly once")

        transaction.write_text(original, encoding="utf-8")
        wifi_client = root / checker.BSC10_WIFI_CLIENT
        wifi_client.write_text(
            wifi_client.read_text(encoding="utf-8").replace(
                "wifiBsc10HilFaultModule().admitLifecycleStart(admission, millis())",
                "wifiBsc10HilFaultModule().bypassLifecycleStart(admission, millis())",
                1,
            ),
            encoding="utf-8",
        )
        assert_error_contains(checker.validate_static(root), "WiFi admission wiring must contain exactly one")


def complete_artifacts(
    root: Path,
    full_sha: str = FULL_SHA,
    environments: tuple[str, ...] = checker.BUILD_ENVIRONMENTS,
) -> None:
    short_sha = full_sha[:7].encode()
    for environment in environments:
        directory = root / ".pio" / "build" / environment
        directory.mkdir(parents=True, exist_ok=True)
        elf = bytearray(2048)
        elf[:7] = b"\x7fELF\x01\x01\x01"
        elf[16:18] = (2).to_bytes(2, "little")
        elf[18:20] = (94).to_bytes(2, "little")
        elf[20:24] = (1).to_bytes(4, "little")
        elf[28:32] = (52).to_bytes(4, "little")
        elf[40:42] = (52).to_bytes(2, "little")
        elf[42:44] = (32).to_bytes(2, "little")
        elf[44:46] = (1).to_bytes(2, "little")
        elf[52:56] = (1).to_bytes(4, "little")
        elf[56:60] = (256).to_bytes(4, "little")
        elf[68:72] = (1024).to_bytes(4, "little")
        elf[72:76] = (1024).to_bytes(4, "little")
        elf[256 : 256 + len(short_sha) + 1] = short_sha + b"\x00"
        elf[320 : 320 + len(environment)] = environment.encode()
        (directory / "firmware.elf").write_bytes(elf)

        segment = bytearray(2048)
        segment[: len(short_sha) + 1] = short_sha + b"\x00"
        segment[64 : 64 + len(environment)] = environment.encode()
        header = bytearray(24)
        header[0:2] = b"\xe9\x01"
        segment_header = (0x3F400000).to_bytes(4, "little") + len(segment).to_bytes(4, "little")
        (directory / "firmware.bin").write_bytes(header + segment_header + segment + b"\x00")


def bound_manifest(root: Path, full_sha: str = FULL_SHA) -> checker.BoundBuildManifest:
    digests: dict[tuple[str, str], str] = {}
    for environment in checker.BUILD_ENVIRONMENTS:
        errors, environment_digests = checker.collect_environment_artifact_digests(
            root, environment
        )
        assert_true(not errors, f"could not bind fixture artifacts: {errors}")
        digests.update(environment_digests)
    return checker.BoundBuildManifest(
        full_sha=full_sha,
        platformio_version="PlatformIO Core, version 6.1.19",
        platformio_tree_sha256=next(iter(checker.EXPECTED_PLATFORMIO_TREE_SHA256)),
        interpreter_sha256="b" * 64,
        artifact_sha256=digests,
    )


def test_binary_absence_requires_complete_real_artifacts_and_scans_markers() -> None:
    with tempfile.TemporaryDirectory(prefix="hil-fault-controls-") as raw:
        empty_root = Path(raw)
        errors = checker.validate_binary_absence(
            empty_root,
            checker.BoundBuildManifest(
                FULL_SHA,
                "PlatformIO Core, version 6.1.19",
                next(iter(checker.EXPECTED_PLATFORMIO_TREE_SHA256)),
                "b" * 64,
                {
                    (environment, kind): "c" * 64
                    for environment in checker.BUILD_ENVIRONMENTS
                    for kind in ("elf", "bin")
                },
            ),
        )
        assert_true(len(errors) == 4, f"expected four explicit artifact blockers: {errors}")
        assert_true(all("unavailable" in error for error in errors), str(errors))

    with tempfile.TemporaryDirectory(prefix="hil-fault-controls-") as raw:
        temporary = Path(raw)
        complete_artifacts(temporary)
        manifest = bound_manifest(temporary)
        assert_true(not checker.validate_binary_absence(temporary, manifest), "clean artifacts failed")

        marker_path = checker.canonical_artifact(temporary, "waveshare-349", "bin")
        marker_path.write_bytes(marker_path.read_bytes() + checker.FORBIDDEN_BINARY_MARKERS[0])
        tamper_errors = checker.validate_binary_absence(temporary, manifest)
        assert_error_contains(
            tamper_errors,
            "production artifact contains HIL-only marker: waveshare-349 bin",
        )
        assert_error_contains(tamper_errors, "does not match the full-revision bound build manifest")

        marker_path.write_bytes(
            b"\xe9\x01" + bytes(22) + (0x3F400000).to_bytes(4, "little") + bytes(4)
        )
        decoy_manifest = bound_manifest(temporary)
        assert_error_contains(
            checker.validate_binary_absence(temporary, decoy_manifest),
            "production artifact format is invalid: waveshare-349 bin",
        )

        complete_artifacts(temporary, "b" * 40)
        mismatched_revision_manifest = bound_manifest(temporary, FULL_SHA)
        assert_error_contains(
            checker.validate_binary_absence(temporary, mismatched_revision_manifest),
            "not bound to target Git revision: waveshare-349 bin",
        )


def test_binary_errors_never_echo_canonical_paths() -> None:
    secret = "private-operational-artifact-name"
    with tempfile.TemporaryDirectory(prefix="hil-fault-controls-") as raw:
        temporary = Path(raw) / secret
        complete_artifacts(temporary)
        manifest = bound_manifest(temporary)
        target = temporary / "target.elf"
        target.write_bytes(bytes(52))
        link = checker.canonical_artifact(temporary, "waveshare-349", "elf")
        link.unlink()
        link.symlink_to(target)
        errors = checker.validate_binary_absence(temporary, manifest)
        assert_error_contains(errors, "nonempty regular non-symlink")
        assert_true(all(secret not in error for error in errors), str(errors))


def test_bound_build_requires_clean_full_sha_and_exact_environment_commands() -> None:
    commands: list[tuple[str, ...]] = []
    observed_environments: list[dict[str, str]] = []
    active_root: Path | None = None

    def successful_runner(command, **_kwargs):
        commands.append(tuple(command))
        observed_environments.append(dict(_kwargs.get("env", {})))
        if command[-1] == "--version":
            return SimpleNamespace(
                returncode=0, stdout="PlatformIO Core, version 6.1.19\n", stderr=""
            )
        if "status" in command:
            return SimpleNamespace(returncode=0, stdout="", stderr="")
        if "rev-parse" in command:
            return SimpleNamespace(returncode=0, stdout="a" * 40 + "\n", stderr="")
        if "run" in command and "-e" in command:
            assert active_root is not None
            environment = command[command.index("-e") + 1]
            if command[-1] == "clean":
                for kind in ("elf", "bin"):
                    path = checker.canonical_artifact(active_root, environment, kind)
                    if path.exists() or path.is_symlink():
                        path.unlink()
            else:
                complete_artifacts(active_root, FULL_SHA, (environment,))
        return SimpleNamespace(returncode=0, stdout="", stderr="")

    injected = dict(checker.os.environ)
    injected.update(
        {
            "GIT_DIR": "/tmp/decoy",
            "PYTHONPATH": "/tmp/injected",
            "PLATFORMIO_BUILD_FLAGS": "-D V1SIMPLE_HIL_FAULT_CONTROL=1",
            "SCONSFLAGS": "--site-dir=/tmp/injected",
        }
    )
    with tempfile.TemporaryDirectory(prefix="hil-fault-controls-") as raw:
        bound_root = fixture_root(Path(raw))
        active_root = bound_root
        complete_artifacts(bound_root, FULL_SHA, checker.BUILD_ENVIRONMENTS)
        build_errors, manifest = checker.validate_bound_builds(
            bound_root, successful_runner, injected
        )
        assert_true(
            not build_errors and manifest is not None and manifest.full_sha == FULL_SHA,
            f"bound build failed: {build_errors}",
        )
        assert_true(
            len(manifest.artifact_sha256) == len(checker.BUILD_ENVIRONMENTS) * 2,
            f"artifact manifest incomplete: {manifest.artifact_sha256}",
        )
    expected_pio = []
    identity = checker.resolve_platformio_identity(ROOT, injected)
    assert_true(identity is not None, "test PlatformIO identity was not trusted")
    pio = (str(identity.interpreter), "-I", "-m", "platformio")
    for environment in checker.BUILD_ENVIRONMENTS:
        expected_pio.extend(
            [
                (
                    str(checker.AUTHORITATIVE_GIT),
                    "--git-dir",
                    str(bound_root / ".git"),
                    "--work-tree",
                    str(bound_root),
                    "-c",
                    "core.fsmonitor=false",
                    "status",
                    "--porcelain=v1",
                    "--untracked-files=all",
                ),
                (
                    str(checker.AUTHORITATIVE_GIT),
                    "--git-dir",
                    str(bound_root / ".git"),
                    "--work-tree",
                    str(bound_root),
                    "-c",
                    "core.fsmonitor=false",
                    "rev-parse",
                    "--verify",
                    "HEAD^{commit}",
                ),
                (*pio, "--version"),
                (*pio, "pkg", "install", "-e", environment),
                (
                    str(checker.AUTHORITATIVE_GIT),
                    "--git-dir",
                    str(bound_root / ".git"),
                    "--work-tree",
                    str(bound_root),
                    "-c",
                    "core.fsmonitor=false",
                    "status",
                    "--porcelain=v1",
                    "--untracked-files=all",
                ),
                (
                    str(checker.AUTHORITATIVE_GIT),
                    "--git-dir",
                    str(bound_root / ".git"),
                    "--work-tree",
                    str(bound_root),
                    "-c",
                    "core.fsmonitor=false",
                    "rev-parse",
                    "--verify",
                    "HEAD^{commit}",
                ),
                (*pio, "--version"),
                (*pio, "run", "-e", environment, "-t", "clean"),
                (*pio, "run", "-e", environment),
            ]
        )
    assert_true(commands[3:-3] == expected_pio, f"unexpected build commands: {commands}")
    for child_environment in observed_environments:
        assert_true(
            all(
                key not in child_environment
                for key in ("GIT_DIR", "PYTHONPATH", "PLATFORMIO_BUILD_FLAGS", "SCONSFLAGS")
            ),
            f"injected environment escaped: {child_environment}",
        )

    def dirty_runner(command, **_kwargs):
        if command[-1] == "--version":
            return SimpleNamespace(
                returncode=0, stdout="PlatformIO Core, version 6.1.19\n", stderr=""
            )
        if "status" in command:
            return SimpleNamespace(returncode=0, stdout=" M source.cpp\n", stderr="")
        return SimpleNamespace(returncode=0, stdout="a" * 40 + "\n", stderr="")

    with tempfile.TemporaryDirectory(prefix="hil-fault-controls-") as raw:
        dirty_root = fixture_root(Path(raw))
        dirty_errors, _ = checker.validate_bound_builds(dirty_root, dirty_runner)
        assert_error_contains(dirty_errors, "must be clean")

    bootstrap_commands: list[tuple[str, ...]] = []

    def failed_bootstrap_runner(command, **_kwargs):
        bootstrap_commands.append(tuple(command))
        if command[-1] == "--version":
            return SimpleNamespace(
                returncode=0, stdout="PlatformIO Core, version 6.1.19\n", stderr=""
            )
        if "status" in command:
            return SimpleNamespace(returncode=0, stdout="", stderr="")
        if "rev-parse" in command:
            return SimpleNamespace(returncode=0, stdout=FULL_SHA + "\n", stderr="")
        if "pkg" in command and "install" in command:
            return SimpleNamespace(returncode=1, stdout="", stderr="bootstrap failed")
        return SimpleNamespace(returncode=0, stdout="", stderr="")

    with tempfile.TemporaryDirectory(prefix="hil-fault-controls-") as raw:
        bootstrap_root = fixture_root(Path(raw))
        bootstrap_errors, _ = checker.validate_bound_builds(
            bootstrap_root, failed_bootstrap_runner
        )
        assert_error_contains(bootstrap_errors, "bound dependency bootstrap failed")
        assert_true(
            not any("run" in command for command in bootstrap_commands),
            f"clean/build ran after bootstrap failure: {bootstrap_commands}",
        )

    bootstrap_completed = False

    def changed_binding_runner(command, **_kwargs):
        nonlocal bootstrap_completed
        if command[-1] == "--version":
            version = "6.1.20" if bootstrap_completed else "6.1.19"
            return SimpleNamespace(
                returncode=0,
                stdout=f"PlatformIO Core, version {version}\n",
                stderr="",
            )
        if "status" in command:
            return SimpleNamespace(returncode=0, stdout="", stderr="")
        if "rev-parse" in command:
            return SimpleNamespace(returncode=0, stdout=FULL_SHA + "\n", stderr="")
        if "pkg" in command and "install" in command:
            bootstrap_completed = True
        return SimpleNamespace(returncode=0, stdout="", stderr="")

    with tempfile.TemporaryDirectory(prefix="hil-fault-controls-") as raw:
        changed_root = fixture_root(Path(raw))
        changed_errors, _ = checker.validate_bound_builds(
            changed_root, changed_binding_runner
        )
        assert_error_contains(changed_errors, "changed after dependency bootstrap")

    def stale_clean_runner(command, **_kwargs):
        if command[-1] == "--version":
            return SimpleNamespace(
                returncode=0, stdout="PlatformIO Core, version 6.1.19\n", stderr=""
            )
        if "status" in command:
            return SimpleNamespace(returncode=0, stdout="", stderr="")
        if "rev-parse" in command:
            return SimpleNamespace(returncode=0, stdout=FULL_SHA + "\n", stderr="")
        return SimpleNamespace(returncode=0, stdout="", stderr="")

    with tempfile.TemporaryDirectory(prefix="hil-fault-controls-") as raw:
        stale_root = fixture_root(Path(raw))
        complete_artifacts(stale_root, FULL_SHA, checker.BUILD_ENVIRONMENTS)
        stale_errors, _ = checker.validate_bound_builds(stale_root, stale_clean_runner)
        assert_error_contains(stale_errors, "bound clean left canonical artifact present")

    with tempfile.TemporaryDirectory(prefix="hil-fault-controls-") as raw:
        fake_root = fixture_root(Path(raw))
        repository_tool = fake_root / "tools" / "pio"
        repository_tool.parent.mkdir()
        repository_tool.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
        repository_tool.chmod(0o700)
        repository_environment = dict(checker.os.environ)
        repository_environment["PATH"] = str(repository_tool.parent)
        repository_environment["PIO_CMD"] = str(repository_tool)
        repository_errors, _ = checker.validate_bound_builds(
            fake_root, successful_runner, repository_environment
        )
        assert_error_contains(repository_errors, "tool identity is unavailable")

        home_override = dict(checker.os.environ)
        home_override["PIO_CMD"] = str(
            checker.repository_owner_home(ROOT) / "bin" / "pio"
        )
        home_errors, _ = checker.validate_bound_builds(
            fake_root, successful_runner, home_override
        )
        assert_error_contains(home_errors, "tool identity is unavailable")

        identity = checker.resolve_platformio_identity(ROOT, dict(checker.os.environ))
        assert_true(identity is not None, "real PlatformIO identity unavailable")
        assert_true(
            all(len(value) == 64 for value in checker.EXPECTED_PLATFORMIO_TREE_SHA256),
            "pinned PlatformIO tree identity is not a full SHA-256",
        )
        altered_tree = fake_root / "altered-platformio"
        shutil.copytree(identity.module_root, altered_tree)
        init_file = altered_tree / "__init__.py"
        init_file.write_text(init_file.read_text(encoding="utf-8") + "\n# altered\n", encoding="utf-8")
        assert_true(
            checker.platformio_tree_sha256(altered_tree)
            not in checker.EXPECTED_PLATFORMIO_TREE_SHA256,
            "altered PlatformIO package matched a pinned tree identity",
        )


def main() -> int:
    tests = (
        test_repository_static_contract_passes,
        test_structural_guard_accepts_formatter_comment_spacing,
        test_hil_environment_cannot_become_default_or_leak_to_production_flags,
        test_unguarded_and_elif_call_sites_are_rejected,
        test_hil_inventory_guard_symlink_and_forbidden_runtime_are_rejected,
        test_bsc16_hook_rejects_direct_hardware_access_and_wiring_drift,
        test_bsc04_hook_rejects_direct_hardware_access_and_wiring_drift,
        test_bsc05_hook_rejects_missing_or_misordered_generation_wiring,
        test_bsc06_hook_rejects_missing_or_preclaim_transport_routing,
        test_bsc10_hook_rejects_missing_or_late_admission,
        test_binary_absence_requires_complete_real_artifacts_and_scans_markers,
        test_binary_errors_never_echo_canonical_paths,
        test_bound_build_requires_clean_full_sha_and_exact_environment_commands,
    )
    for test in tests:
        test()
        print(f"PASS {test.__name__}")
    print(f"PASS {len(tests)} HIL fault-control checker regressions")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

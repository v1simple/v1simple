#!/usr/bin/env python3
"""Ensure release artifacts come from the tested PlatformIO production env."""

from __future__ import annotations

import csv
import re
import sys
from pathlib import Path

from check_ci_evidence import (
    DEFAULT_JOB_NAME,
    DEFAULT_STEP_NAME,
    DEFAULT_WORKFLOW_NAME,
    DEFAULT_WORKFLOW_PATH,
)


ROOT = Path(__file__).resolve().parents[1]
PLATFORMIO_INI = ROOT / "platformio.ini"
CI_TEST = ROOT / "scripts" / "ci-test.sh"
PRODUCTION_BUILD = ROOT / "scripts" / "build_production_artifacts.sh"
CI_YML = ROOT / ".github" / "workflows" / "ci.yml"
RELEASE_YML = ROOT / ".github" / "workflows" / "release.yml"
PARTITIONS = ROOT / "partitions_v1.csv"
ENV = "waveshare-349"


def env_value(key: str) -> str:
    text = PLATFORMIO_INI.read_text(encoding="utf-8")
    match = re.search(
        rf"^\[env:{ENV}\]\s*$([\s\S]*?)(?=^\[|\Z)",
        text,
        re.MULTILINE,
    )
    if not match:
        raise ValueError(f"platformio.ini missing [env:{ENV}]")
    value = re.search(rf"^\s*{re.escape(key)}\s*=\s*(.+?)\s*$", match.group(1), re.MULTILINE)
    if not value:
        raise ValueError(f"[env:{ENV}] missing {key}")
    return value.group(1).strip()


def env_value_optional(key: str) -> str | None:
    text = PLATFORMIO_INI.read_text(encoding="utf-8")
    match = re.search(
        rf"^\[env:{ENV}\]\s*$([\s\S]*?)(?=^\[|\Z)",
        text,
        re.MULTILINE,
    )
    if not match:
        raise ValueError(f"platformio.ini missing [env:{ENV}]")
    value = re.search(rf"^\s*{re.escape(key)}\s*=\s*(.+?)\s*$", match.group(1), re.MULTILINE)
    if not value:
        return None
    return value.group(1).strip()


def workflow_flag(flag: str) -> str:
    text = RELEASE_YML.read_text(encoding="utf-8")
    match = re.search(rf"{re.escape(flag)}\s+([^\s\\]+)", text)
    if not match:
        raise ValueError(f"release.yml missing {flag}")
    return match.group(1).strip()


def workflow_image_offset(image_name: str) -> int:
    text = RELEASE_YML.read_text(encoding="utf-8")
    match = re.search(rf"(?i)\b(0x[0-9a-f]+|\d+)\s+release/{re.escape(image_name)}\b", text)
    if not match:
        raise ValueError(f"release.yml missing merge-bin offset for {image_name}")
    return int(match.group(1), 0)


def partition_offset(name: str) -> int:
    with PARTITIONS.open(newline="", encoding="utf-8") as fh:
        for row in csv.reader(fh):
            if not row or row[0].strip().startswith("#"):
                continue
            if row[0].strip() == name:
                return int(row[3].strip(), 0)
    raise ValueError(f"partition table missing {name} partition")


def expected_freq() -> str:
    hz = int(env_value("board_build.f_flash").rstrip("Ll"))
    if hz % 1_000_000 != 0:
        raise ValueError(f"unsupported board_build.f_flash value: {hz}")
    return f"{hz // 1_000_000}m"


def expected_flash_mode() -> str:
    memory_type = (env_value_optional("board_build.arduino.memory_type") or "").lower()
    if memory_type in ("opi_opi", "opi_qspi"):
        return "dout"

    mode = env_value("board_build.flash_mode").lower()
    if mode in ("qio", "qout"):
        return "dio"
    return mode


def expected_flash_size() -> str:
    return env_value_optional("board_upload.flash_size") or env_value("board_build.flash_size")


def require_contains(text: str, needle: str, label: str, errors: list[str]) -> None:
    if needle not in text:
        errors.append(f"{label} missing required text: {needle!r}")


def check_production_env_is_tested(errors: list[str]) -> None:
    """Guard the shared exact-artifact build and protected PR-gate contract."""

    ci_test_text = CI_TEST.read_text(encoding="utf-8")
    production_text = PRODUCTION_BUILD.read_text(encoding="utf-8")
    ci_workflow_text = CI_YML.read_text(encoding="utf-8")
    release_text = RELEASE_YML.read_text(encoding="utf-8")
    dependency_command = f'"$PIO_CMD" pkg install -e {ENV}'

    require_contains(
        ci_workflow_text,
        "bash ./scripts/ci-test.sh",
        ".github/workflows/ci.yml resilient shell invocation",
        errors,
    )
    workflow_path = CI_YML.relative_to(ROOT).as_posix()
    if workflow_path != DEFAULT_WORKFLOW_PATH:
        errors.append(
            "Protected CI workflow path does not match the authoritative workflow: "
            f"{DEFAULT_WORKFLOW_PATH!r} != {workflow_path!r}"
        )
    for required in (
        f"name: {DEFAULT_WORKFLOW_NAME}",
        f"    name: {DEFAULT_JOB_NAME}",
        f"      - name: {DEFAULT_STEP_NAME}",
        "pull_request:",
        "workflow_dispatch:",
        "branches: [main]",
    ):
        require_contains(
            ci_workflow_text,
            required,
            ".github/workflows/ci.yml protected-check identity",
            errors,
        )
    if re.search(r"^\s{2}push:\s*$", ci_workflow_text, re.MULTILINE):
        errors.append("ci.yml must not repeat the full PR gate after merge to main")
    require_contains(
        ci_test_text,
        "./scripts/build_production_artifacts.sh",
        "scripts/ci-test.sh",
        errors,
    )
    require_contains(
        release_text,
        "./scripts/build_production_artifacts.sh",
        ".github/workflows/release.yml",
        errors,
    )
    for required in (
        f"PIO_BUILD_ARGS=(-e {ENV})",
        dependency_command,
        'run_step "Firmware clean" "$PIO_CMD" run "${PIO_BUILD_ARGS[@]}" -t clean',
        'run_step "Firmware build" run_firmware_build_with_memory_log',
        'run_step "LittleFS image build" "$PIO_CMD" run "${PIO_BUILD_ARGS[@]}" -t buildfs',
        "scripts/check_memory_headroom.py",
        "scripts/report_flash_package_size.py",
        "scripts/check_littlefs_image_compatibility.py",
        "npm run build",
        "npm run deploy:built",
    ):
        require_contains(
            production_text,
            required,
            "scripts/build_production_artifacts.sh",
            errors,
        )

    dependency_index = production_text.find(dependency_command)
    clean_index = production_text.find(
        'run_step "Firmware clean" "$PIO_CMD" run "${PIO_BUILD_ARGS[@]}" -t clean'
    )
    if -1 not in (dependency_index, clean_index) and dependency_index > clean_index:
        errors.append(
            "scripts/build_production_artifacts.sh must install PlatformIO project "
            "dependencies before the first firmware command"
        )

    if "./scripts/ci-test.sh" in release_text:
        errors.append("release.yml must not rerun the protected PR gate")
    if "cd interface && npm ci" in ci_workflow_text:
        errors.append("ci.yml must not install frontend dependencies before ci-test.sh installs them")
    require_contains(
        ci_test_text,
        'run_step "Frontend dependencies" npm ci',
        "scripts/ci-test.sh strict frontend dependency install",
        errors,
    )
    if "npm install" in ci_test_text:
        errors.append("ci-test.sh must not fall back from npm ci to a mutable npm install")

    build_index = release_text.find("./scripts/build_production_artifacts.sh")
    merge_index = release_text.find("Merge firmware for ESP Web Tools")
    if -1 not in (build_index, merge_index) and build_index > merge_index:
        errors.append("release.yml must build production artifacts before packaging them")


def check_release_version_automation(errors: list[str]) -> None:
    """Keep the one-click release path safe, ordered, and idempotent."""

    release_text = RELEASE_YML.read_text(encoding="utf-8")
    for required in (
        "push:",
        "branches: [main]",
        "persist-credentials: false",
        '"$GITHUB_EVENT_NAME" != "push"',
        'python3 scripts/prepare_release.py --lookup-run-id "$RELEASE_RUN_ID"',
        'git checkout --detach "$RESUME_SHA"',
        'EVENT_SHA: ${{ github.sha }}',
        'git checkout --detach "$EVENT_SHA"',
        "RELEASE_BUMP: patch",
        'python3 scripts/prepare_release.py',
        '--bump "$RELEASE_BUMP"',
        '--resume-tag "$RESUME_TAG"',
        'git commit -m "chore(release): prepare $RELEASE_TAG"',
        'git diff --name-only "$BASE_SHA" "$RELEASE_SHA"',
        "python3 scripts/check_release_config_change.py",
        "./scripts/build_production_artifacts.sh",
        'EXISTING_SHA="$(git rev-parse "$RELEASE_TAG^{commit}")"',
        "Release-Run-ID: $RELEASE_RUN_ID",
        "push --atomic origin",
        "HEAD:refs/heads/main",
        '"refs/tags/$RELEASE_TAG:refs/tags/$RELEASE_TAG"',
        "published: ${{ steps.publish.outputs.published }}",
        "deploy_pages: ${{ steps.publish.outputs.deploy_pages }}",
        'echo "published=false" >> "$GITHUB_OUTPUT"',
        'python3 scripts/prepare_release.py --latest-tag',
        'if [ "$LATEST_RELEASE_TAG" = "$RELEASE_TAG" ]',
        "if: steps.publish.outputs.published == 'true'",
        "if: needs.release.outputs.deploy_pages == 'true'",
        "generate_release_notes: true",
    ):
        require_contains(release_text, required, ".github/workflows/release.yml", errors)

    for forbidden in (
        "body_path: RELEASE_NOTES.md",
        "git push --tags",
        "git push --force",
        "GITHUB_SHA",
        "run: ./scripts/ci-test.sh",
        "Bump FIRMWARE_VERSION in include/config.h before releasing",
        "python3 scripts/check_ci_evidence.py",
        "actions: read",
        "workflow_dispatch:",
        "paths-ignore:",
    ):
        if forbidden in release_text:
            errors.append(f"release.yml contains retired release behavior: {forbidden!r}")

    prepare_index = release_text.find("--bump \"$RELEASE_BUMP\"")
    resume_index = release_text.find("--lookup-run-id")
    build_index = release_text.find("./scripts/build_production_artifacts.sh")
    publish_index = release_text.find("Publish release commit and tag")
    ordered = (resume_index, prepare_index, build_index, publish_index)
    if -1 not in ordered and list(ordered) != sorted(ordered):
        errors.append("release.yml must resolve reruns, prepare, build, then publish")


def main() -> int:
    errors: list[str] = []
    check_production_env_is_tested(errors)
    check_release_version_automation(errors)

    try:
        expected = {
            "--flash-mode": expected_flash_mode(),
            "--flash-freq": expected_freq(),
            "--flash-size": expected_flash_size(),
        }
        actual = {flag: workflow_flag(flag) for flag in expected}
    except ValueError as exc:
        errors.append(str(exc))
        expected = {}
        actual = {}

    for flag, expected_value in expected.items():
        actual_value = actual.get(flag, "")
        if actual_value.upper() != expected_value.upper():
            errors.append(f"{flag}: expected {expected_value}, got {actual_value}")

    try:
        app_partition_offset = partition_offset("app")
        storage_partition_offset = partition_offset("storage")
        upload_offset = int(env_value("board_upload.offset_address"), 0)
        firmware_offset = workflow_image_offset("firmware.bin")
        littlefs_offset = workflow_image_offset("littlefs.bin")
        if upload_offset != app_partition_offset:
            errors.append(
                "board_upload.offset_address must match app partition offset: "
                f"{upload_offset:#x} != {app_partition_offset:#x}"
            )
        if firmware_offset != app_partition_offset:
            errors.append(
                f"release firmware offset: expected {app_partition_offset:#x}, got {firmware_offset:#x}"
            )
        if littlefs_offset != storage_partition_offset:
            errors.append(
                f"release LittleFS offset: expected {storage_partition_offset:#x}, got {littlefs_offset:#x}"
            )
    except ValueError as exc:
        errors.append(str(exc))

    release_text = RELEASE_YML.read_text(encoding="utf-8")
    if "merge_bin" in release_text or "--flash_mode" in release_text:
        errors.append("release.yml must use current esptool merge-bin/--flash-* spelling")
    if "scripts/check_release_image_info.py" not in release_text:
        errors.append("release.yml must validate merged-firmware.bin with check_release_image_info.py")
    for required in (
        "web-installer/index.html",
        "cp THIRD_PARTY_NOTICES.md release/THIRD_PARTY_NOTICES.md",
        "cp licenses/ArduinoJson-LICENSE.txt release/ArduinoJson-LICENSE.txt",
        "cp licenses/NimBLE-Arduino-LICENSE.txt release/NimBLE-Arduino-LICENSE.txt",
        "cp licenses/NimBLE-Arduino-NOTICE.txt release/NimBLE-Arduino-NOTICE.txt",
        "cp licenses/Arduino-GFX-LICENSE.txt release/Arduino-GFX-LICENSE.txt",
        "cp licenses/OpenFontRender-LICENSE.txt release/OpenFontRender-LICENSE.txt",
        "cp licenses/FreeType-FTL.txt release/FreeType-FTL.txt",
        "cp licenses/Svelte-LICENSE.md release/Svelte-LICENSE.md",
        "cp licenses/SvelteKit-LICENSE.txt release/SvelteKit-LICENSE.txt",
        "cp licenses/daisyUI-LICENSE.txt release/daisyUI-LICENSE.txt",
        "cp licenses/Tailwind-CSS-LICENSE.txt release/Tailwind-CSS-LICENSE.txt",
        "cp THIRD_PARTY_NOTICES.md release/pages/THIRD_PARTY_NOTICES.md",
        "cp licenses/ArduinoJson-LICENSE.txt release/pages/licenses/ArduinoJson-LICENSE.txt",
        "cp licenses/NimBLE-Arduino-LICENSE.txt release/pages/licenses/NimBLE-Arduino-LICENSE.txt",
        "cp licenses/NimBLE-Arduino-NOTICE.txt release/pages/licenses/NimBLE-Arduino-NOTICE.txt",
        "cp licenses/Arduino-GFX-LICENSE.txt release/pages/licenses/Arduino-GFX-LICENSE.txt",
        "cp licenses/OpenFontRender-LICENSE.txt release/pages/licenses/OpenFontRender-LICENSE.txt",
        "cp licenses/FreeType-FTL.txt release/pages/licenses/FreeType-FTL.txt",
        "cp licenses/Svelte-LICENSE.md release/pages/licenses/Svelte-LICENSE.md",
        "cp licenses/SvelteKit-LICENSE.txt release/pages/licenses/SvelteKit-LICENSE.txt",
        "cp licenses/daisyUI-LICENSE.txt release/pages/licenses/daisyUI-LICENSE.txt",
        "cp licenses/Tailwind-CSS-LICENSE.txt release/pages/licenses/Tailwind-CSS-LICENSE.txt",
        "\n            release/THIRD_PARTY_NOTICES.md",
        "\n            release/ArduinoJson-LICENSE.txt",
        "\n            release/NimBLE-Arduino-LICENSE.txt",
        "\n            release/NimBLE-Arduino-NOTICE.txt",
        "\n            release/Arduino-GFX-LICENSE.txt",
        "\n            release/OpenFontRender-LICENSE.txt",
        "\n            release/FreeType-FTL.txt",
        "\n            release/Svelte-LICENSE.md",
        "\n            release/SvelteKit-LICENSE.txt",
        "\n            release/daisyUI-LICENSE.txt",
        "\n            release/Tailwind-CSS-LICENSE.txt",
        "scripts/check_web_installer_page.py --site-dir release/pages",
        "uses: actions/configure-pages@",
        "enablement: true",
        "actions/upload-pages-artifact@fc324d3547104276b827a68afc52ff2a11cc49c9",
        "uses: actions/deploy-pages@",
    ):
        if required not in release_text:
            errors.append(f"release.yml missing web installer Pages deployment contract: {required}")

    if errors:
        print("[contract] release workflow production-env contract failed:")
        for error in errors:
            print(f"  - {error}")
        return 1

    print("[contract] release artifacts use the tested production env and flash policy")
    return 0


if __name__ == "__main__":
    sys.exit(main())

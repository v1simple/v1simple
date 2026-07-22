#!/usr/bin/env python3
"""Fail closed unless HIL fault controls are isolated from production firmware."""

from __future__ import annotations

import configparser
from dataclasses import dataclass
import hashlib
import os
from pathlib import Path
import re
import stat
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
HIL_MACRO = "V1SIMPLE_HIL_FAULT_CONTROL"
HIL_ENVIRONMENT = "waveshare-349-hil"
EXPECTED_PLATFORMIO_VERSION = "6.1.19"
EXPECTED_PLATFORMIO_TREE_SHA256 = {
    # Official PyPI platformio==6.1.19 wheel used by GitHub Actions.
    "0002aa2f70cd2ce5934e25a8fcfafaef1ded3c5ea579206b851a12b5bde92394",
    # PlatformIO/pioarduino 6.1.19 standalone environment used by maintainers.
    "5712e0d34c18cd9af6232737c0550d6e25e98646f1101710f205310abdbe4fc2",
}
PRODUCTION_ENVIRONMENTS = ("waveshare-349", "esp32-s3-car-install")
BUILD_ENVIRONMENTS = (HIL_ENVIRONMENT, *PRODUCTION_ENVIRONMENTS)
AUTHORITATIVE_GIT = Path("/usr/bin/git")
COMPILED_SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".ino", ".s", ".S"}
RELEASE_CONFIGURATION_FILES = (
    "build.sh",
    ".github/workflows/release.yml",
    "scripts/build_production_artifacts.sh",
)
CI_TEST_FILE = "scripts/ci-test.sh"
CI_WORKFLOW_FILE = ".github/workflows/ci.yml"
CI_PLATFORMIO_INSTALL = (
    'pip install "platformio==6.1.19" "cryptography>=41" '
    '"Pillow>=10.4,<13" "clang-format==22.1.8"'
)
CI_REGRESSION_GATE = (
    'run_step "HIL fault-control exclusion regression tests" '
    "python3 scripts/test_check_bug_squash_hil_fault_controls.py"
)
CI_AUTHORITATIVE_GATE = (
    'run_step "HIL fault-control authoritative build gate" '
    "python3 scripts/check_bug_squash_hil_fault_controls.py"
)
EXPECTED_HIL_FILES = {
    "src/modules/hil/hil_fault_controller.cpp",
    "src/modules/hil/hil_fault_controller.h",
    "src/modules/hil/hil_fault_serial_module.cpp",
    "src/modules/hil/hil_fault_serial_module.h",
    "src/modules/hil/hil_next_boot_fault.h",
    "src/modules/hil/hil_ready_barrier.h",
}
EXPECTED_BSC16_PRODUCT_HIL_FILES = {
    "src/modules/power/battery_bsc16_hil_fault_module.cpp",
    "src/modules/power/battery_bsc16_hil_fault_module.h",
}
EXPECTED_BSC04_PRODUCT_HIL_FILES = {
    "src/modules/system/connection_bsc04_hil_fault_module.cpp",
    "src/modules/system/connection_bsc04_hil_fault_module.h",
}
EXPECTED_BSC10_PRODUCT_HIL_FILES = {
    "src/modules/wifi/wifi_bsc10_hil_fault_module.cpp",
    "src/modules/wifi/wifi_bsc10_hil_fault_module.h",
}
BSC04_MAIN = "src/main.cpp"
BSC10_WIFI_CLIENT = "src/wifi_client.cpp"
BSC10_TRANSACTION = "src/modules/wifi/wifi_client_enable_transaction.cpp"
BSC16_BATTERY_MANAGER = "src/battery_manager.cpp"
HIL_REFERENCE_RE = re.compile(
    r"(?:modules/hil/|hil_fault_|hil_ready_barrier|HilFault|HilReady|V1SIMPLE_HIL_FAULT_CONTROL)"
)
CONFIG_REFERENCE_RE = re.compile(r"\$\{([^{}]+)\}")
OUTER_GUARD_RE = re.compile(
    r"^\s*#\s*if\s+defined\s*\(\s*V1SIMPLE_HIL_FAULT_CONTROL\s*\)\s*$",
    re.MULTILINE,
)
OUTER_GUARD_END_RE = re.compile(
    r"^\s*#\s*endif\s*//\s*V1SIMPLE_HIL_FAULT_CONTROL\s*$"
)
FORBIDDEN_RUNTIME_RE = re.compile(
    r"(?:\bnew\s+|\bdelete\s+|\bmalloc\s*\(|\bcalloc\s*\(|\brealloc\s*\(|"
    r"\bfree\s*\(|\bstd::(?:vector|string|map|unordered_map)\b|\bString\b|"
    r"\bdelay\s*\(|\bvTaskDelay\s*\(|\bPreferences\b|\bnvs_[A-Za-z0-9_]*\b|"
    r"\bWebServer\b|\bHTTPClient\b|\bWiFiClient\b)"
)
BSC16_FORBIDDEN_HARDWARE_RE = re.compile(
    r"\b(?:digitalRead|digitalWrite|gpio_[A-Za-z0-9_]*|latchPowerOn|pinMode|setTCA9554Pin|TwoWire)\b"
)
FORBIDDEN_BINARY_MARKERS = (
    b"V1HIL",
    b"HilFaultController",
    b"HilFaultSerialModule",
    b"wifi-ap-start-fail-once",
    b"wifi-internal-sram-hold",
    b"v1-verify-push-suppress-once",
    b"v1-notification-delay-once",
    b"obd-transport-operation-barrier-once",
    b"wifi-enable-admission-fail-once",
    b"obd-physical-link-preownership-barrier-once",
    b"sd-mutex-hold",
    b"battery-adc-init-fail-once",
)


@dataclass(frozen=True)
class BoundBuildManifest:
    full_sha: str
    platformio_version: str
    platformio_tree_sha256: str
    interpreter_sha256: str
    artifact_sha256: dict[tuple[str, str], str]


@dataclass(frozen=True)
class PlatformioIdentity:
    interpreter: Path
    interpreter_sha256: str
    module_root: Path
    tree_sha256: str
    version: str


def load_platformio(root: Path, errors: list[str]) -> configparser.ConfigParser | None:
    parser = configparser.ConfigParser(interpolation=None, strict=True)
    parser.optionxform = str
    try:
        with (root / "platformio.ini").open("r", encoding="utf-8") as handle:
            parser.read_file(handle)
    except (OSError, configparser.Error) as exc:
        errors.append(f"platformio.ini could not be parsed: {exc}")
        return None
    return parser


def comma_values(raw: str) -> tuple[str, ...]:
    return tuple(value.strip() for value in raw.split(",") if value.strip())


def extends_values(raw: str) -> tuple[str, ...]:
    return tuple(value for value in re.split(r"[\s,]+", raw.strip()) if value)


def section_text(parser: configparser.ConfigParser, section: str) -> str:
    return "\n".join(f"{key}={value}" for key, value in parser.items(section, raw=True))


def referenced_sections(
    parser: configparser.ConfigParser,
    section: str,
) -> set[str]:
    references: set[str] = set()
    if section.startswith("env:") and parser.has_section("env"):
        references.add("env")
    for key, value in parser.items(section, raw=True):
        if key == "extends":
            references.update(
                candidate
                for candidate in extends_values(value)
                if parser.has_section(candidate)
            )
        for token in CONFIG_REFERENCE_RE.findall(value):
            if "." not in token:
                continue
            referenced_section, _ = token.rsplit(".", 1)
            if parser.has_section(referenced_section):
                references.add(referenced_section)
    return references


def reachable_sections(
    parser: configparser.ConfigParser,
    start: str,
) -> set[str]:
    reached: set[str] = set()
    pending = [start]
    while pending:
        section = pending.pop()
        if section in reached or not parser.has_section(section):
            continue
        reached.add(section)
        pending.extend(referenced_sections(parser, section) - reached)
    return reached


def line_is_guarded(lines: list[str], target_index: int) -> bool:
    frames: list[dict[str, bool]] = []
    possible_without_hil = True

    def condition_without_hil(directive: str) -> tuple[bool, bool]:
        positive = re.fullmatch(
            r"#\s*(?:if\s+defined\s*\(\s*V1SIMPLE_HIL_FAULT_CONTROL\s*\)|"
            r"ifdef\s+V1SIMPLE_HIL_FAULT_CONTROL)",
            directive,
        )
        if positive is not None:
            return False, False
        negative = re.fullmatch(
            r"#\s*(?:if\s+!\s*defined\s*\(\s*V1SIMPLE_HIL_FAULT_CONTROL\s*\)|"
            r"ifndef\s+V1SIMPLE_HIL_FAULT_CONTROL)",
            directive,
        )
        if negative is not None:
            return True, True
        # Unknown conditions are conservatively treated as potentially true and
        # potentially false when the HIL macro is absent.
        return True, False

    for index, line in enumerate(lines):
        stripped = line.strip()
        if re.match(r"^#\s*(if|ifdef|ifndef)\b", stripped):
            condition_possible, condition_always = condition_without_hil(stripped)
            frames.append(
                {
                    "parent_possible": possible_without_hil,
                    "prior_always": condition_always,
                }
            )
            possible_without_hil = possible_without_hil and condition_possible
        elif re.match(r"^#\s*elif\b", stripped) and frames:
            frame = frames[-1]
            condition_possible, condition_always = condition_without_hil(
                re.sub(r"^#\s*elif\b", "#if", stripped, count=1)
            )
            possible_without_hil = (
                frame["parent_possible"]
                and not frame["prior_always"]
                and condition_possible
            )
            frame["prior_always"] = frame["prior_always"] or condition_always
        elif re.match(r"^#\s*else\b", stripped) and frames:
            frame = frames[-1]
            possible_without_hil = frame["parent_possible"] and not frame["prior_always"]
            frame["prior_always"] = True
        elif re.match(r"^#\s*endif\b", stripped) and frames:
            possible_without_hil = frames.pop()["parent_possible"]
        if index == target_index:
            return not possible_without_hil
    return False


def has_structural_outer_guard(text: str) -> bool:
    lines = text.splitlines()
    substantive = [index for index, line in enumerate(lines) if line.strip()]
    if not substantive:
        return False
    first = substantive[0]
    if lines[first].strip() == "#pragma once":
        substantive = substantive[1:]
    if not substantive:
        return False
    guard_index = substantive[0]
    closing_index = substantive[-1]
    if (
        OUTER_GUARD_RE.fullmatch(lines[guard_index]) is None
        or OUTER_GUARD_END_RE.fullmatch(lines[closing_index]) is None
    ):
        return False

    depth = 0
    for index in range(guard_index, closing_index + 1):
        stripped = lines[index].strip()
        if re.match(r"^#\s*(if|ifdef|ifndef)\b", stripped):
            depth += 1
        elif re.match(r"^#\s*(elif|else)\b", stripped) and depth == 1:
            return False
        elif re.match(r"^#\s*endif\b", stripped):
            depth -= 1
            if depth < 0 or (depth == 0 and index != closing_index):
                return False
    return depth == 0


def validate_static(root: Path) -> list[str]:
    errors: list[str] = []
    parser = load_platformio(root, errors)
    if parser is not None:
        required_sections = {"platformio", f"env:{HIL_ENVIRONMENT}", *(
            f"env:{environment}" for environment in PRODUCTION_ENVIRONMENTS
        )}
        for section in sorted(required_sections):
            if not parser.has_section(section):
                errors.append(f"platformio.ini is missing [{section}]")
        if parser.has_section("platformio"):
            defaults = comma_values(parser.get("platformio", "default_envs", fallback=""))
            if HIL_ENVIRONMENT in defaults:
                errors.append("HIL environment must not be a default environment")
            if defaults != ("waveshare-349",):
                errors.append("default environment must remain exactly waveshare-349")
        hil_section = f"env:{HIL_ENVIRONMENT}"
        if parser.has_section(hil_section):
            hil_text = section_text(parser, hil_section)
            if parser.get(hil_section, "extends", fallback="").strip() != "env:waveshare-349":
                errors.append("HIL environment must extend the portable production environment")
            if re.search(rf"(?:-D\s*|\b){HIL_MACRO}(?:=1)?\b", hil_text) is None:
                errors.append("HIL environment must define the HIL-only compile macro")
        for section in parser.sections():
            if section == hil_section or HIL_MACRO not in section_text(parser, section):
                continue
            if section in {f"env:{environment}" for environment in PRODUCTION_ENVIRONMENTS}:
                errors.append(f"production environment defines HIL macro: {section[4:]}")
            else:
                errors.append(f"non-HIL PlatformIO section defines HIL macro: {section}")
        for environment in PRODUCTION_ENVIRONMENTS:
            production_section = f"env:{environment}"
            reached = reachable_sections(parser, production_section)
            if hil_section in reached:
                errors.append(
                    f"production environment references or inherits HIL environment: {environment}"
                )
            leaking_sections = sorted(
                section
                for section in reached
                if section != hil_section and HIL_MACRO in section_text(parser, section)
            )
            if leaking_sections:
                errors.append(
                    f"production environment effectively defines HIL macro: {environment}"
                )

    for relative in RELEASE_CONFIGURATION_FILES:
        path = root / relative
        try:
            release_text = path.read_text(encoding="utf-8")
        except (OSError, UnicodeError):
            errors.append(f"release configuration is unavailable: {relative}")
            continue
        if HIL_ENVIRONMENT in release_text:
            errors.append(f"HIL environment is referenced by release configuration: {relative}")

    try:
        ci_lines = (root / CI_TEST_FILE).read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeError):
        errors.append("HIL authoritative checker CI wiring is unavailable")
    else:
        regression_indices = [
            index for index, line in enumerate(ci_lines) if line.strip() == CI_REGRESSION_GATE
        ]
        authoritative_indices = [
            index for index, line in enumerate(ci_lines) if line.strip() == CI_AUTHORITATIVE_GATE
        ]
        if (
            len(regression_indices) != 1
            or len(authoritative_indices) != 1
            or authoritative_indices[0] != regression_indices[0] + 1
        ):
            errors.append(
                "authoritative HIL checker must run exactly once immediately after its CI regressions"
            )
    try:
        ci_workflow = (root / CI_WORKFLOW_FILE).read_text(encoding="utf-8")
    except (OSError, UnicodeError):
        errors.append("CI PlatformIO version pin is unavailable")
    else:
        if ci_workflow.count(CI_PLATFORMIO_INSTALL) != 1:
            errors.append("CI must install exactly the pinned PlatformIO 6.1.19 toolchain")

    hil_root = root / "src" / "modules" / "hil"
    actual_hil_files: set[str] = set()
    if not hil_root.is_dir():
        errors.append("HIL module directory is missing")
    else:
        for path in sorted(hil_root.rglob("*")):
            if path.is_symlink():
                errors.append(f"HIL source must not be a symlink: {path.relative_to(root)}")
                continue
            if not path.is_file():
                continue
            relative = path.relative_to(root).as_posix()
            actual_hil_files.add(relative)
            try:
                text = path.read_text(encoding="utf-8")
            except (OSError, UnicodeError) as exc:
                errors.append(f"HIL source could not be read: {relative}: {exc}")
                continue
            if not has_structural_outer_guard(text):
                errors.append(f"HIL source is not enclosed by the compile guard: {relative}")
            forbidden = FORBIDDEN_RUNTIME_RE.search(text)
            if forbidden is not None:
                errors.append(
                    f"HIL source contains forbidden dynamic, persistent, network, or blocking API: "
                    f"{relative}: {forbidden.group(0)}"
                )
        if actual_hil_files != EXPECTED_HIL_FILES:
            errors.append(
                "HIL source inventory mismatch: expected "
                + ", ".join(sorted(EXPECTED_HIL_FILES))
            )

    source_root = root / "src"
    for relative in sorted(EXPECTED_BSC04_PRODUCT_HIL_FILES):
        path = root / relative
        if path.is_symlink() or not path.is_file():
            errors.append(f"BSC-04 product HIL source is unavailable or a symlink: {relative}")
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except (OSError, UnicodeError) as exc:
            errors.append(f"BSC-04 product HIL source could not be read: {relative}: {exc}")
            continue
        if not has_structural_outer_guard(text):
            errors.append(f"BSC-04 product HIL source is not enclosed by the compile guard: {relative}")
        forbidden = BSC16_FORBIDDEN_HARDWARE_RE.search(text)
        if forbidden is not None:
            errors.append(
                f"BSC-04 product HIL source mutates hardware directly: {relative}: {forbidden.group(0)}"
            )

    main_path = root / BSC04_MAIN
    try:
        main_source = main_path.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as exc:
        errors.append(f"BSC-04 VerifyPush routing wiring is unavailable: {exc}")
    else:
        raw_edge = "bool v1VerifyPushMatchEdge = bleClient.consumeVerifyPushMatchEdge();"
        route_edge = "connectionBsc04HilFaultModule().routeVerifyPushMatchEdge("
        cycle_context = "const CycleContext cycleContext{"
        coordinator_update = "connectionCycleCoordinatorModule.update(cycleContext);"
        for token in (raw_edge, route_edge, cycle_context, coordinator_update):
            if main_source.count(token) != 1:
                errors.append(f"BSC-04 VerifyPush routing wiring must contain exactly one {token}")
        raw_index = main_source.find(raw_edge)
        route_index = main_source.find(route_edge)
        context_index = main_source.find(cycle_context)
        update_index = main_source.find(coordinator_update)
        if min(raw_index, route_index, context_index, update_index) < 0 or not (
            raw_index < route_index < context_index < update_index
        ):
            errors.append(
                "BSC-04 suppression hook must route the consumed VerifyPush edge before coordinator admission"
            )

    for relative in sorted(EXPECTED_BSC16_PRODUCT_HIL_FILES):
        path = root / relative
        if path.is_symlink() or not path.is_file():
            errors.append(f"BSC-16 product HIL source is unavailable or a symlink: {relative}")
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except (OSError, UnicodeError) as exc:
            errors.append(f"BSC-16 product HIL source could not be read: {relative}: {exc}")
            continue
        if not has_structural_outer_guard(text):
            errors.append(f"BSC-16 product HIL source is not enclosed by the compile guard: {relative}")
        forbidden = BSC16_FORBIDDEN_HARDWARE_RE.search(text)
        if forbidden is not None:
            errors.append(
                f"BSC-16 product HIL source mutates hardware directly: {relative}: {forbidden.group(0)}"
            )

    battery_manager = root / BSC16_BATTERY_MANAGER
    try:
        battery_source = battery_manager.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as exc:
        errors.append(f"BSC-16 battery admission wiring is unavailable: {exc}")
    else:
        required_once = (
            "batteryBsc16HilFaultModule().beginAdcAdmission(",
            "batteryBsc16HilFaultModule().completeAdcAdmissionSuppression(",
        )
        for token in required_once:
            if battery_source.count(token) != 1:
                errors.append(f"BSC-16 battery admission wiring must contain exactly one {token}")
        latch_index = battery_source.find("latchPowerOn()")
        hook_index = battery_source.find(required_once[0])
        completion_index = battery_source.find(required_once[1])
        adc_index = battery_source.find("adcInitialized = initADC();", hook_index)
        if (
            latch_index < 0
            or hook_index < 0
            or completion_index < 0
            or adc_index < 0
            or not latch_index < hook_index < completion_index < adc_index
        ):
            errors.append("BSC-16 ADC fault hook must remain after latch initialization and before ADC admission")

    for relative in sorted(EXPECTED_BSC10_PRODUCT_HIL_FILES):
        path = root / relative
        if path.is_symlink() or not path.is_file():
            errors.append(f"BSC-10 product HIL source is unavailable or a symlink: {relative}")
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except (OSError, UnicodeError) as exc:
            errors.append(f"BSC-10 product HIL source could not be read: {relative}: {exc}")
            continue
        if not has_structural_outer_guard(text):
            errors.append(f"BSC-10 product HIL source is not enclosed by the compile guard: {relative}")

    try:
        transaction_source = (root / BSC10_TRANSACTION).read_text(encoding="utf-8")
        wifi_client_source = (root / BSC10_WIFI_CLIENT).read_text(encoding="utf-8")
    except (OSError, UnicodeError) as exc:
        errors.append(f"BSC-10 WiFi admission wiring is unavailable: {exc}")
    else:
        admission_token = "if (runtime.admitStart && !runtime.admitStart(runtime.ctx))"
        attempt_token = "if (!runtime.attemptStart(runtime.ctx))"
        if transaction_source.count(admission_token) != 1:
            errors.append("BSC-10 transaction admission hook must occur exactly once")
        if transaction_source.find(admission_token) >= transaction_source.find(attempt_token):
            errors.append("BSC-10 transaction admission hook must precede lifecycle mutation")
        route_token = "wifiBsc10HilFaultModule().admitLifecycleStart(admission, millis())"
        callback_token = "runtime.admitStart = [](void* ctx)"
        attempt_callback_token = "runtime.attemptStart = [](void* ctx)"
        for token in (route_token, callback_token):
            if wifi_client_source.count(token) != 1:
                errors.append(f"BSC-10 WiFi admission wiring must contain exactly one {token}")
        if wifi_client_source.find(callback_token) >= wifi_client_source.find(attempt_callback_token):
            errors.append("BSC-10 WiFi admission callback must be installed before lifecycle mutation")

    if source_root.is_dir():
        for path in sorted(source_root.rglob("*")):
            if path.is_symlink() or not path.is_file() or path.suffix not in COMPILED_SOURCE_SUFFIXES:
                continue
            relative = path.relative_to(root).as_posix()
            if relative.startswith("src/modules/hil/"):
                continue
            try:
                lines = path.read_text(encoding="utf-8").splitlines()
            except (OSError, UnicodeError) as exc:
                errors.append(f"source could not be read while checking HIL call sites: {relative}: {exc}")
                continue
            for index, line in enumerate(lines):
                if OUTER_GUARD_END_RE.fullmatch(line) is not None:
                    continue
                if HIL_REFERENCE_RE.search(line) is not None and not line_is_guarded(lines, index):
                    errors.append(f"unguarded HIL call site: {relative}:{index + 1}")
    return errors


def artifact_has_expected_format(kind: str, content: bytes) -> bool:
    if kind == "elf":
        if not (
            len(content) >= 1024
            and content[:7] == b"\x7fELF\x01\x01\x01"
            and int.from_bytes(content[16:18], "little") in (2, 3)
            and int.from_bytes(content[18:20], "little") == 94
            and int.from_bytes(content[20:24], "little") == 1
            and int.from_bytes(content[40:42], "little") == 52
            and int.from_bytes(content[42:44], "little") == 32
        ):
            return False
        program_offset = int.from_bytes(content[28:32], "little")
        program_count = int.from_bytes(content[44:46], "little")
        if not (1 <= program_count <= 128) or program_offset + program_count * 32 > len(content):
            return False
        for index in range(program_count):
            offset = program_offset + index * 32
            if int.from_bytes(content[offset : offset + 4], "little") != 1:
                continue
            file_offset = int.from_bytes(content[offset + 4 : offset + 8], "little")
            file_size = int.from_bytes(content[offset + 16 : offset + 20], "little")
            if file_size > 0 and file_offset + file_size <= len(content):
                return True
        return False
    if len(content) < 1024 or content[0] != 0xE9 or not 1 <= content[1] <= 16:
        return False
    offset = 24
    for _ in range(content[1]):
        if offset + 8 > len(content):
            return False
        segment_size = int.from_bytes(content[offset + 4 : offset + 8], "little")
        offset += 8
        if segment_size == 0 or offset + segment_size > len(content):
            return False
        offset += segment_size
    return offset < len(content)


def canonical_artifact(root: Path, environment: str, kind: str) -> Path:
    return root / ".pio" / "build" / environment / f"firmware.{kind}"


def has_symlink_component(root: Path, path: Path) -> bool:
    cursor = root
    for component in path.relative_to(root).parts[:-1]:
        cursor /= component
        try:
            if stat.S_ISLNK(os.lstat(cursor).st_mode):
                return True
        except OSError:
            return True
    return False


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def collect_environment_artifact_digests(
    root: Path,
    environment: str,
) -> tuple[list[str], dict[tuple[str, str], str]]:
    errors: list[str] = []
    digests: dict[tuple[str, str], str] = {}
    for kind in ("elf", "bin"):
        path = canonical_artifact(root, environment, kind)
        try:
            metadata = os.lstat(path)
            if (
                stat.S_ISLNK(metadata.st_mode)
                or not stat.S_ISREG(metadata.st_mode)
                or metadata.st_size <= 0
                or has_symlink_component(root, path)
            ):
                raise OSError("artifact is not a regular bound-build output")
            digests[(environment, kind)] = sha256_file(path)
        except OSError as exc:
            errors.append(
                f"bound build artifact unavailable: {environment} {kind}: "
                f"{type(exc).__name__}"
            )
    return errors, digests


def validate_binary_absence(root: Path, manifest: BoundBuildManifest) -> list[str]:
    errors: list[str] = []
    expected_keys = {
        (environment, kind)
        for environment in BUILD_ENVIRONMENTS
        for kind in ("elf", "bin")
    }
    if (
        re.fullmatch(r"[0-9a-f]{40,64}", manifest.full_sha) is None
        or manifest.platformio_tree_sha256 not in EXPECTED_PLATFORMIO_TREE_SHA256
        or re.fullmatch(r"[0-9a-f]{64}", manifest.interpreter_sha256) is None
        or manifest.platformio_version
        != f"PlatformIO Core, version {EXPECTED_PLATFORMIO_VERSION}"
        or set(manifest.artifact_sha256) != expected_keys
        or any(
            re.fullmatch(r"[0-9a-f]{64}", digest) is None
            for digest in manifest.artifact_sha256.values()
        )
    ):
        return ["bound build manifest identity or artifact inventory is invalid"]
    expected_short_sha = manifest.full_sha[:7]
    for environment in PRODUCTION_ENVIRONMENTS:
        for kind in ("elf", "bin"):
            path = canonical_artifact(root, environment, kind)
            try:
                metadata = os.lstat(path)
            except OSError as exc:
                errors.append(
                    f"production artifact unavailable: {environment} {kind}: "
                    f"{type(exc).__name__}"
                )
                continue
            if (
                stat.S_ISLNK(metadata.st_mode)
                or not stat.S_ISREG(metadata.st_mode)
                or metadata.st_size <= 0
                or has_symlink_component(root, path)
            ):
                errors.append(
                    f"production artifact is not a nonempty regular non-symlink file: "
                    f"{environment} {kind}"
                )
                continue
            try:
                content = path.read_bytes()
            except OSError as exc:
                errors.append(
                    f"production artifact could not be read: {environment} {kind}: "
                    f"{type(exc).__name__}"
                )
                continue
            if not artifact_has_expected_format(kind, content):
                errors.append(
                    f"production artifact format is invalid: {environment} {kind}"
                )
                continue
            expected_digest = manifest.artifact_sha256.get((environment, kind), "")
            actual_digest = hashlib.sha256(content).hexdigest()
            if (
                re.fullmatch(r"[0-9a-f]{64}", expected_digest) is None
                or actual_digest != expected_digest
            ):
                errors.append(
                    f"production artifact does not match the full-revision bound build manifest: "
                    f"{environment} {kind}"
                )
            if expected_short_sha.encode("ascii") + b"\x00" not in content:
                errors.append(
                    f"production artifact is not bound to target Git revision: "
                    f"{environment} {kind}"
                )
                continue
            for marker in FORBIDDEN_BINARY_MARKERS:
                if marker in content:
                    errors.append(
                        f"production artifact contains HIL-only marker: {environment} {kind}: "
                        f"{marker.decode('ascii')}"
                    )
    return errors


def repository_owner_home(root: Path) -> Path:
    try:
        import pwd

        return Path(pwd.getpwuid(root.stat().st_uid).pw_dir)
    except (ImportError, KeyError, OSError):
        return Path.home()


def path_is_within(path: Path, parent: Path) -> bool:
    try:
        path.relative_to(parent)
        return True
    except ValueError:
        return False


def trusted_external_path(
    root: Path,
    candidate: Path,
    require_executable: bool,
) -> bool:
    lexical = candidate.absolute()
    resolved = candidate.resolve()
    repository = root.resolve()
    temporary_roots = {Path("/tmp").resolve(), Path("/private/tmp").resolve()}
    for path in (lexical, resolved):
        if path_is_within(path, repository) or any(
            path_is_within(path, temporary) for temporary in temporary_roots
        ):
            return False
    try:
        repository_owner = root.stat().st_uid
        for path in (lexical, resolved):
            cursor = Path(path.anchor)
            for component in path.parts[1:]:
                cursor /= component
                metadata = os.stat(cursor)
                if (
                    metadata.st_uid not in (0, repository_owner)
                    or metadata.st_mode & stat.S_IWOTH
                    or (
                        metadata.st_mode & stat.S_IWGRP
                        and metadata.st_uid != repository_owner
                    )
                ):
                    return False
        metadata = os.stat(resolved)
    except OSError:
        return False
    if not stat.S_ISREG(metadata.st_mode):
        return False
    if require_executable and not os.access(lexical, os.X_OK):
        return False
    return True


def platformio_tree_sha256(root: Path) -> str:
    digest = hashlib.sha256()
    for path in sorted(root.rglob("*")):
        if (
            not path.is_file()
            or "__pycache__" in path.parts
            or path.suffix in {".pyc", ".pyo"}
        ):
            continue
        relative = path.relative_to(root).as_posix().encode("utf-8")
        content = path.read_bytes()
        digest.update(len(relative).to_bytes(4, "big"))
        digest.update(relative)
        digest.update(len(content).to_bytes(8, "big"))
        digest.update(content)
    return digest.hexdigest()


def resolve_platformio_identity(
    root: Path,
    source_environment: dict[str, str],
) -> PlatformioIdentity | None:
    if source_environment.get("PIO_CMD", "pio") != "pio":
        return None
    allowed_interpreter_roots = (
        repository_owner_home(root) / ".platformio" / "penv",
        Path("/opt/hostedtoolcache/Python"),
    )
    candidates = (
        Path(sys.executable).absolute(),
        repository_owner_home(root) / ".platformio" / "penv" / "bin" / "python",
    )
    probe = (
        "import pathlib, platformio, sys; "
        "print(pathlib.Path(sys.executable).resolve()); "
        "print(pathlib.Path(sys.prefix).resolve()); "
        "print(pathlib.Path(platformio.__file__).resolve()); "
        "print(platformio.__version__)"
    )
    observed: set[Path] = set()
    for interpreter in candidates:
        if interpreter in observed:
            continue
        observed.add(interpreter)
        if not any(
            path_is_within(interpreter, allowed.absolute())
            for allowed in allowed_interpreter_roots
        ) or not trusted_external_path(root, interpreter, True):
            continue
        environment = positive_child_environment(root, interpreter, source_environment)
        try:
            result = subprocess.run(
                [str(interpreter), "-I", "-c", probe],
                cwd=root,
                env=environment,
                capture_output=True,
                text=True,
                check=False,
            )
            lines = result.stdout.splitlines()
            if result.returncode != 0 or len(lines) != 4:
                continue
            reported_interpreter = Path(lines[0]).resolve()
            prefix = Path(lines[1]).resolve()
            module_file = Path(lines[2]).resolve()
            version = lines[3].strip()
            module_root = module_file.parent
            if (
                reported_interpreter != interpreter.resolve()
                or not path_is_within(module_root, prefix)
                or not trusted_external_path(root, module_file, False)
                or version != EXPECTED_PLATFORMIO_VERSION
            ):
                continue
            tree_sha256 = platformio_tree_sha256(module_root)
            if tree_sha256 not in EXPECTED_PLATFORMIO_TREE_SHA256:
                continue
            return PlatformioIdentity(
                interpreter=interpreter,
                interpreter_sha256=sha256_file(interpreter.resolve()),
                module_root=module_root,
                tree_sha256=tree_sha256,
                version=version,
            )
        except (OSError, UnicodeError):
            continue
    return None


def positive_child_environment(
    root: Path,
    pio_executable: Path,
    source_environment: dict[str, str],
) -> dict[str, str]:
    allowed = {
        "LANG",
        "LC_ALL",
        "LOGNAME",
        "NO_PROXY",
        "REQUESTS_CA_BUNDLE",
        "SSL_CERT_FILE",
        "SYSTEMROOT",
        "TMPDIR",
        "TZ",
        "USER",
    }
    environment = {
        key: value for key, value in source_environment.items() if key in allowed
    }
    environment["HOME"] = str(repository_owner_home(root))
    environment["PATH"] = os.pathsep.join(
        (str(pio_executable.parent), "/usr/sbin", "/usr/bin", "/sbin", "/bin")
    )
    environment["GIT_CONFIG_NOSYSTEM"] = "1"
    environment["GIT_CONFIG_GLOBAL"] = os.devnull
    environment["GIT_CONFIG_COUNT"] = "0"
    return environment


def canonical_artifacts_are_absent(root: Path, environment: str) -> bool:
    for kind in ("elf", "bin"):
        try:
            os.lstat(canonical_artifact(root, environment, kind))
        except FileNotFoundError:
            continue
        except OSError:
            return False
        return False
    return True


def validate_bound_builds(
    root: Path,
    runner=subprocess.run,
    source_environment: dict[str, str] | None = None,
) -> tuple[list[str], BoundBuildManifest | None]:
    errors: list[str] = []
    source_environment = dict(os.environ) if source_environment is None else source_environment
    initial_identity = resolve_platformio_identity(root, source_environment)
    if initial_identity is None or not AUTHORITATIVE_GIT.is_file():
        return ["authoritative Git or PlatformIO tool identity is unavailable"], None
    pio_command = [str(initial_identity.interpreter), "-I", "-m", "platformio"]
    environment = positive_child_environment(
        root, initial_identity.interpreter, source_environment
    )
    common = {
        "cwd": root,
        "env": environment,
        "capture_output": True,
        "text": True,
        "check": False,
    }
    git_prefix = [
        str(AUTHORITATIVE_GIT),
        "--git-dir",
        str(root / ".git"),
        "--work-tree",
        str(root),
        "-c",
        "core.fsmonitor=false",
    ]
    try:
        pio_version = runner([*pio_command, "--version"], **common)
        status = runner(
            [*git_prefix, "status", "--porcelain=v1", "--untracked-files=all"],
            **common,
        )
        revision = runner([*git_prefix, "rev-parse", "--verify", "HEAD^{commit}"], **common)
    except OSError as exc:
        return [f"build binding tool unavailable: {type(exc).__name__}"], None
    if pio_version.returncode != 0 or pio_version.stdout.strip() != (
        f"PlatformIO Core, version {EXPECTED_PLATFORMIO_VERSION}"
    ):
        errors.append("authoritative PlatformIO identity check failed")
    if status.returncode != 0:
        errors.append("target Git cleanliness could not be verified")
    elif status.stdout.strip():
        errors.append("target Git worktree must be clean before the bound rebuild")
    full_sha = revision.stdout.strip().lower() if revision.returncode == 0 else ""
    if re.fullmatch(r"[0-9a-f]{40,64}", full_sha) is None:
        errors.append("target full Git revision could not be verified")
    if errors:
        return errors, None

    def dependency_binding_error(environment_name: str, phase: str) -> str | None:
        try:
            bound_status = runner(
                [*git_prefix, "status", "--porcelain=v1", "--untracked-files=all"],
                **common,
            )
            bound_revision = runner(
                [*git_prefix, "rev-parse", "--verify", "HEAD^{commit}"],
                **common,
            )
            bound_pio_version = runner([*pio_command, "--version"], **common)
            bound_identity = resolve_platformio_identity(root, source_environment)
        except OSError as exc:
            return (
                f"dependency bootstrap binding unavailable {phase}: {environment_name}: "
                f"{type(exc).__name__}"
            )
        if (
            bound_status.returncode != 0
            or bound_status.stdout.strip()
            or bound_revision.returncode != 0
            or bound_revision.stdout.strip().lower() != full_sha
            or bound_pio_version.returncode != 0
            or bound_pio_version.stdout != pio_version.stdout
            or bound_identity != initial_identity
        ):
            return (
                f"Git or PlatformIO identity changed {phase} dependency bootstrap: "
                f"{environment_name}"
            )
        return None

    artifact_sha256: dict[tuple[str, str], str] = {}
    for build_environment in BUILD_ENVIRONMENTS:
        binding_error = dependency_binding_error(build_environment, "before")
        if binding_error is not None:
            return [binding_error], None
        try:
            bootstrap = runner(
                [*pio_command, "pkg", "install", "-e", build_environment],
                **common,
            )
        except OSError as exc:
            errors.append(
                f"bound dependency bootstrap tool unavailable: {build_environment}: "
                f"{type(exc).__name__}"
            )
            return errors, None
        if bootstrap.returncode != 0:
            errors.append(f"bound dependency bootstrap failed: {build_environment}")
            return errors, None
        binding_error = dependency_binding_error(build_environment, "after")
        if binding_error is not None:
            return [binding_error], None
        for command_kind, command in (
            ("clean", [*pio_command, "run", "-e", build_environment, "-t", "clean"]),
            ("build", [*pio_command, "run", "-e", build_environment]),
        ):
            try:
                result = runner(command, **common)
            except OSError as exc:
                errors.append(
                    f"bound {command_kind} tool unavailable: {build_environment}: "
                    f"{type(exc).__name__}"
                )
                return errors, None
            if result.returncode != 0:
                errors.append(f"bound {command_kind} failed: {build_environment}")
                return errors, None
            if command_kind == "clean" and not canonical_artifacts_are_absent(
                root, build_environment
            ):
                errors.append(
                    f"bound clean left canonical artifact present: {build_environment}"
                )
                return errors, None
        artifact_errors, environment_digests = collect_environment_artifact_digests(
            root, build_environment
        )
        if artifact_errors:
            return artifact_errors, None
        artifact_sha256.update(environment_digests)
    try:
        final_status = runner(
            [*git_prefix, "status", "--porcelain=v1", "--untracked-files=all"], **common
        )
        final_revision = runner(
            [*git_prefix, "rev-parse", "--verify", "HEAD^{commit}"], **common
        )
        final_pio_version = runner([*pio_command, "--version"], **common)
        final_identity = resolve_platformio_identity(root, source_environment)
    except OSError as exc:
        return [f"post-build Git binding unavailable: {type(exc).__name__}"], None
    if final_status.returncode != 0 or final_status.stdout.strip():
        errors.append("target Git worktree changed during the bound rebuild")
    if final_revision.returncode != 0 or final_revision.stdout.strip().lower() != full_sha:
        errors.append("target Git revision changed during the bound rebuild")
    if (
        final_pio_version.returncode != 0
        or final_pio_version.stdout != pio_version.stdout
        or final_identity != initial_identity
    ):
        errors.append("authoritative PlatformIO identity changed during the bound rebuild")
    manifest = BoundBuildManifest(
        full_sha=full_sha,
        platformio_version=pio_version.stdout.strip(),
        platformio_tree_sha256=initial_identity.tree_sha256,
        interpreter_sha256=initial_identity.interpreter_sha256,
        artifact_sha256=artifact_sha256,
    )
    return errors, manifest if not errors else None


def main() -> int:
    static_errors = validate_static(ROOT)
    build_errors: list[str] = []
    manifest: BoundBuildManifest | None = None
    if not static_errors:
        build_errors, manifest = validate_bound_builds(ROOT)
    binary_errors = (
        []
        if static_errors or build_errors or manifest is None
        else validate_binary_absence(ROOT, manifest)
    )
    errors = [*static_errors, *build_errors, *binary_errors]
    if errors:
        print("[hil-fault-controls] validation failed:")
        for error in errors:
            print(f"  - {error}")
        return 1
    print("[hil-fault-controls] bound rebuild, static isolation, and production binary absence verified")
    return 0


if __name__ == "__main__":
    sys.exit(main())

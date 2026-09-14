#!/usr/bin/env python3
"""Guard warning-free production builds and narrowly scoped vendor repairs."""

from __future__ import annotations

import ast
import configparser
import re
import shlex
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SHA256 = re.compile(r"[0-9a-f]{64}")


def literal_constants(path: Path) -> tuple[str, dict[str, object]]:
    source = path.read_text(encoding="utf-8")
    module = ast.parse(source)
    constants: dict[str, object] = {}
    for node in module.body:
        if not isinstance(node, ast.Assign) or len(node.targets) != 1:
            continue
        target = node.targets[0]
        if not isinstance(target, ast.Name):
            continue
        try:
            constants[target.id] = ast.literal_eval(node.value)
        except (TypeError, ValueError):
            continue
    return source, constants


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def check_production_flags() -> None:
    parser = configparser.ConfigParser(interpolation=None)
    parser.read(ROOT / "platformio.ini", encoding="utf-8")
    flags = shlex.split(parser.get("env:waveshare-349", "build_flags"))
    for required in ("-Wall", "-Wextra", "-Werror"):
        require(required in flags, f"waveshare-349 is missing {required}")
    require(not any(flag.startswith("-Wno-") for flag in flags),
            "waveshare-349 must not globally suppress a warning class")

    car_flags = parser.get("env:esp32-s3-car-install", "build_flags")
    require("${env:waveshare-349.build_flags}" in car_flags,
            "car-install must inherit the strict production warning contract")

    fault_flags = parser.get("env:waveshare-349-fault", "build_flags")
    fault_scripts = parser.get("env:waveshare-349-fault", "extra_scripts")
    require("${env:waveshare-349.build_flags}" in fault_flags,
            "bench fault build must inherit the strict production warning contract")
    require("BENCH_FAULT_INJECT" not in fault_flags,
            "bench fault build must not accept an empty environment interpolation")
    require("configure_bench_fault_inject.py" in fault_scripts,
            "bench fault build must validate and apply its selected fault")

    fault_configuration = (ROOT / "scripts" / "configure_bench_fault_inject.py").read_text(
        encoding="utf-8"
    )
    require('{"1", "2", "3"}' in fault_configuration,
            "bench fault selector must allow only the three defined negative controls")
    require('("BENCH_FAULT_INJECT", int(raw_fault))' in fault_configuration,
            "validated bench fault must reach the compiler definition")

    warning_script = (ROOT / "scripts" / "enforce_reorder_warning.py").read_text(
        encoding="utf-8"
    )
    require('"waveshare-349-fault"' in warning_script,
            "bench fault build must receive production-only warning promotions")


def check_open_font_render_patch() -> None:
    source, constants = literal_constants(ROOT / "scripts" / "patch_openfontrender.py")
    require("-Wno-" not in source, "OpenFontRender patch must not add a broad compiler suppression")
    require(source.count('#pragma GCC diagnostic ignored \\"-Wcast-function-type\\"') == 1,
            "OpenFontRender may suppress only its one legacy debug-hook cast")

    for prefix in ("CMAP", "GRAYS", "TTOBJS"):
        upstream_hash = constants.get(f"{prefix}_UPSTREAM_SHA256")
        patched_hash = constants.get(f"{prefix}_PATCHED_SHA256")
        require(isinstance(upstream_hash, str) and SHA256.fullmatch(upstream_hash) is not None,
                f"{prefix} upstream source is not fingerprinted")
        require(isinstance(patched_hash, str) and SHA256.fullmatch(patched_hash) is not None,
                f"{prefix} patched source is not fingerprinted")
        require(upstream_hash != patched_hash, f"{prefix} source fingerprints must differ")

    cmap = constants.get("CMAP_PATCHED")
    require(isinstance(cmap, str) and "TT_CONFIG_CMAP_FORMAT_10" in cmap,
            "cmap helper must remain guarded by every format that uses it")
    grays = constants.get("GRAYS_PATCHED")
    require(isinstance(grays, str) and grays.count("/* fall through */") == 6,
            "compact span writer must retain all six explicit fallthrough annotations")
    ttobjs = constants.get("TTOBJS_PATCHED")
    require(isinstance(ttobjs, str) and ttobjs.count("#pragma GCC diagnostic push") == 1 and
            ttobjs.count("#pragma GCC diagnostic pop") == 1,
            "legacy debug-hook exception must remain scoped to one expression")

    for relative in ("sfnt/ttcmap.c", "smooth/ftgrays.c", "truetype/ttobjs.c"):
        require(f'"{relative}"' in source, f"warning patch omits {relative}")


def check_webserver_patch() -> None:
    source, constants = literal_constants(ROOT / "scripts" / "patch_arduino_webserver_body.py")
    patched_include = constants.get("INCLUDE_PATCHED")
    require(isinstance(patched_include, str), "WebServer patched marker is missing")
    require("__attribute__((used))" in patched_include and "used, retain" not in patched_include,
            "WebServer marker must use its live reference, not unsupported retain")
    warningful_include = constants.get("INCLUDE_WARNINGFUL_PATCHED")
    require(isinstance(warningful_include, str) and "used, retain" in warningful_include,
            "WebServer must recognize and migrate the prior warningful patch")
    function_patch = constants.get("FUNCTION_PATCHED")
    require(isinstance(function_patch, str) and
            "&v1simple_webserver_exact_body_contract" in function_patch,
            "WebServer reader must retain a live marker reference")
    for name in ("UPSTREAM_SHA256", "V1_PATCHED_SHA256", "BROKEN_V2_PATCHED_SHA256",
                 "WARNINGFUL_PATCHED_SHA256", "PATCHED_SHA256"):
        value = constants.get(name)
        require(isinstance(value, str) and SHA256.fullmatch(value) is not None,
                f"WebServer source state {name} is not fingerprinted")
    require("-Wno-" not in source, "WebServer patch must not suppress compiler warnings")

    _, framework_constants = literal_constants(ROOT / "scripts" / "verify_esp32s3_framework.py")
    framework_expected = framework_constants.get("EXPECTED")
    require(isinstance(framework_expected, dict) and
            framework_expected.get("webserver_parsing_sha256") == constants.get("PATCHED_SHA256"),
            "framework qualification must require the warning-clean WebServer source")


def main() -> int:
    check_production_flags()
    check_open_font_render_patch()
    check_webserver_patch()
    print("[production-warnings] strict builds and fingerprinted vendor repairs validated")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

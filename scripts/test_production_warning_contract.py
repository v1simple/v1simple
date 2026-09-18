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


def check_arduino_overload_compatibility() -> None:
    parser = configparser.ConfigParser(interpolation=None)
    parser.read(ROOT / "platformio.ini", encoding="utf-8")
    production_scripts = shlex.split(parser.get("env:waveshare-349", "extra_scripts"))
    device_scripts = shlex.split(parser.get("env:device", "extra_scripts"))
    fs_script = "pre:scripts/patch_arduino_fs_overload.py"
    gfx_script = "pre:scripts/patch_arduino_gfx_overloads.py"
    nimble_stream_script = "pre:scripts/patch_nimble_stream_overload.py"
    framework_check = "pre:scripts/verify_esp32s3_framework.py"
    for environment, scripts in (
        ("production", production_scripts),
        ("device", device_scripts),
    ):
        require(scripts.count(fs_script) == 1,
                f"{environment} builds must apply the FS overload repair exactly once")
        require(framework_check in scripts and scripts.index(fs_script) < scripts.index(framework_check),
                f"{environment} FS overload repair must precede framework qualification")
    require(production_scripts.count(gfx_script) == 1,
            "production builds must apply the Arduino_GFX overload repair exactly once")
    require(gfx_script not in device_scripts,
            "device tests must not require the undeclared Arduino_GFX dependency")
    require(production_scripts.count(nimble_stream_script) == 1,
            "production builds must repair the NimBLE Stream overload exactly once")

    fs_source, fs_constants = literal_constants(
        ROOT / "scripts" / "patch_arduino_fs_overload.py"
    )
    gfx_source, gfx_constants = literal_constants(
        ROOT / "scripts" / "patch_arduino_gfx_overloads.py"
    )
    nimble_source, nimble_constants = literal_constants(
        ROOT / "scripts" / "patch_nimble_stream_overload.py"
    )
    for source in (fs_source, gfx_source, nimble_source):
        require("-Wno-" not in source and "#pragma GCC diagnostic" not in source,
                "Arduino overload compatibility must repair declarations, not suppress warnings")
    for prefix, constants in (("FS", fs_constants), ("GFX", gfx_constants)):
        upstream_hash = constants.get(f"{prefix}_UPSTREAM_SHA256")
        patched_hash = constants.get(f"{prefix}_PATCHED_SHA256")
        require(isinstance(upstream_hash, str) and SHA256.fullmatch(upstream_hash) is not None,
                f"{prefix} upstream overload source is not fingerprinted")
        require(isinstance(patched_hash, str) and SHA256.fullmatch(patched_hash) is not None,
                f"{prefix} patched overload source is not fingerprinted")
        require(upstream_hash != patched_hash, f"{prefix} overload fingerprints must differ")
    gfx_flush = gfx_constants.get("GFX_FLUSH_PATCHED")
    require(isinstance(gfx_flush, str) and
            "void flush() override { flush(false); }" in gfx_flush and
            "virtual void flush(bool force_flush);" in gfx_flush,
            "Arduino_GFX must implement Print::flush without changing force-flush dispatch")
    gfx_write = gfx_constants.get("GFX_WRITE_PATCHED")
    require(isinstance(gfx_write, str) and "using Print::write;" in gfx_write,
            "Arduino_GFX must retain buffered Print writes")
    fs_read = fs_constants.get("FS_READ_PATCHED")
    require(isinstance(fs_read, str) and "using Stream::readBytes;" in fs_read,
            "FS File must retain Stream byte-buffer reads")
    nimble_upstream_hash = nimble_constants.get("UPSTREAM_SHA256")
    nimble_patched_hash = nimble_constants.get("PATCHED_SHA256")
    require(isinstance(nimble_upstream_hash, str) and
            SHA256.fullmatch(nimble_upstream_hash) is not None,
            "NimBLE Stream upstream source is not fingerprinted")
    require(isinstance(nimble_patched_hash, str) and
            SHA256.fullmatch(nimble_patched_hash) is not None and
            nimble_patched_hash != nimble_upstream_hash,
            "NimBLE Stream patched source is not uniquely fingerprinted")
    nimble_declaration = nimble_constants.get("PATCHED_DECLARATION")
    require(isinstance(nimble_declaration, str) and
            "int availableForWrite() override" in nimble_declaration and
            "size_t availableForWrite() const;" in nimble_declaration,
            "NimBLE Stream must implement Print while preserving its const query")

    display_source = (ROOT / "include" / "display_driver.h").read_text(encoding="utf-8")
    display_exception = '''#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverloaded-virtual"
#endif
#include <Arduino_GFX_Library.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif'''
    require(display_source.count(display_exception) == 1,
            "Arduino_GFX overload exception must wrap only its vendor include")
    require(display_source.count('#pragma GCC diagnostic ignored "-Woverloaded-virtual"') == 1,
            "display overload exception may be used only at the Arduino_GFX include")

    project_inheritance = re.compile(r"\bpublic\s+(?:Print|Stream)\b")
    reviewed_inheritance = {"include/json_stream_response.h"}
    found_inheritance: set[str] = set()
    for directory in (ROOT / "include", ROOT / "src"):
        for path in directory.rglob("*"):
            if path.suffix not in {".h", ".hpp", ".cpp"}:
                continue
            if project_inheritance.search(path.read_text(encoding="utf-8")):
                found_inheritance.add(path.relative_to(ROOT).as_posix())
    require(found_inheritance == reviewed_inheritance,
            "project-owned Print/Stream inheritance changed; review its complete overload sets: "
            f"{sorted(found_inheritance)}")

    _, framework_constants = literal_constants(ROOT / "scripts" / "verify_esp32s3_framework.py")
    framework_expected = framework_constants.get("EXPECTED")
    require(isinstance(framework_expected, dict) and
            framework_expected.get("fs_header_sha256") == fs_constants.get("FS_PATCHED_SHA256"),
            "framework qualification must require the overload-compatible FS header")


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
    check_arduino_overload_compatibility()
    check_open_font_render_patch()
    check_webserver_patch()
    print("[production-warnings] strict builds and fingerprinted vendor repairs validated")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

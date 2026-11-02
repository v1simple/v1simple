#!/usr/bin/env python3
"""Exercise pinned NimBLE NVS startup through real BLE admission and bond backup.

The dependency is populated by the preceding production static-analysis build.
NVS reads, radio and FreeRTOS are host substitutes; this does not inject physical
flash errors. Extracted owners are unmodified, including their failure paths.
"""
from pathlib import Path
import hashlib
import os
import runpy
import shlex
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(source: str, signature: str) -> str:
    if source.count(signature) != 1:
        raise AssertionError(f"Owner boundary missing or ambiguous: {signature}")
    start = source.index(signature)
    opening = source.index("{", start)
    depth, end = 1, opening + 1
    while depth and end < len(source):
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    if depth:
        raise AssertionError(f"Unclosed owner: {signature}")
    return source[start:end]


def build_and_run(build: Path) -> None:
    patch = runpy.run_path(str(ROOT / "scripts/patch_nimble_bond_bootstrap.py"))
    vendor = (ROOT / ".pio/libdeps/waveshare-349/NimBLE-Arduino/src/nimble/nimble/host/"
              "store/config/src/ble_store_nvs.c")
    text = vendor.read_text(encoding="utf-8")
    repaired = patch["patch_text"](text)
    assert patch["patch_text"](repaired) == repaired
    upstream = repaired
    for original, replacement in patch["REPLACEMENTS"]:
        upstream = upstream.replace(replacement, original, 1)
    assert hashlib.sha256(upstream.encode()).hexdigest() == patch["UPSTREAM_SHA256"]
    assert patch["patch_text"](upstream) == repaired
    assert hashlib.sha256(repaired.encode()).hexdigest() == patch["PATCHED_SHA256"]
    for drift in (repaired + "\n", repaired.replace("static int v1simple_bond_bootstrap_result = -1;", "")):
        try:
            patch["patch_text"](drift)
        except RuntimeError:
            pass
        else:
            raise AssertionError("Patch accepted changed or incomplete source")
    loader = "\n\n".join(function(repaired, signature) for signature in (
        "static int\nget_nvs_db_value(",
        "static int\nget_nvs_peer_record(",
        "static int\npopulate_db_from_nvs(",
        "static int\nble_nvs_restore_sec_keys(",
        "static int\nble_nvs_restore_peer_records(",
        "/* v1simple-nimble-bond-bootstrap-v1: publish complete NVS initialization. */",
    ))
    # The last extraction ends at the status query, before the initializer.
    loader += "\n\n" + function(repaired, "void ble_store_config_conf_init(")
    (build / "pinned_loader.inc").write_text(loader, encoding="utf-8")
    client = (ROOT / "src/ble_client.cpp").read_text(encoding="utf-8")
    (build / "ble_init.inc").write_text(function(client, "bool V1BLEClient::initBLE("), encoding="utf-8")
    compiler = shlex.split(os.environ.get("CXX", "clang++" if shutil.which("clang++") else "c++"))
    deps = ROOT / ".pio/libdeps/native-sanitized"
    includes = [build, ROOT, ROOT / "test/mocks", ROOT / "include", ROOT / "src", deps / "Unity/src"]
    executable = build / "bootstrap"
    subprocess.run(compiler + [
        "-std=c++17", "-DUNIT_TEST", '-DPROJECT_DIR="' + str(ROOT) + '"',
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer", "-g",
        *["-I" + str(path) for path in includes],
        str(ROOT / "test/fixtures/ble_bond_bootstrap/bootstrap.cpp"),
        "-x", "c++", str(deps / "Unity/src/unity.c"), "-o", str(executable),
    ], check=True)
    cases = [("healthy", 0, 0), ("empty", 0, 0), ("our-only", 0, 0),
             ("peer-only", 0, 0), ("init-failure", 0, 0), ("open", 1, 1), ("payload", 1, 2)]
    # Both a first read/open error and a later error after successful records,
    # plus every remaining table participating in complete NVS initialization.
    cases += [("our", 1, index) for index in (1, 2)]
    cases += [("peer", 2, index) for index in (1, 2)]
    cases += [(name, kind, 1) for name, kind in (
        ("cccd", 3), ("csfc", 4), ("local-irk", 5), ("rpa", 6), ("peer-record", 7), ("ead", 8))]
    for name, kind, index in cases:
        print(f"[bond-bootstrap] {name} fault-index={index}", flush=True)
        subprocess.run([str(executable), name, str(kind), str(index)], check=True)


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="ble-bond-bootstrap-") as temporary:
        build_and_run(Path(temporary))

#!/usr/bin/env python3
"""Emit firmware memory headroom JSON and fail on hard overflows."""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_REPORT_DIR = ROOT / ".artifacts" / "test_reports" / "memory-headroom"
PIO_MEMORY_RE = re.compile(
    r"^(RAM|Flash):.*?used\s+(\d+)\s+bytes\s+from\s+(\d+)\s+bytes",
    re.MULTILINE,
)
MAP_MEMORY_RE = re.compile(
    r"^(?P<name>\S+)\s+0x[0-9a-fA-F]+\s+0x(?P<length>[0-9a-fA-F]+)\s+(?P<attrs>\S+)\s*$",
    re.MULTILINE,
)
SIZE_SECTION_RE = re.compile(r"^(?P<section>\.\S+)\s+(?P<size>\d+)\s+\d+\s*$")


def run(cmd: list[str]) -> str:
    result = subprocess.run(
        cmd,
        cwd=ROOT,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    if result.returncode != 0:
        raise RuntimeError(
            f"{' '.join(cmd)} failed with exit code {result.returncode}\n{result.stdout}"
        )
    return result.stdout


def read_platformio_value(env: str, key: str) -> str | None:
    text = (ROOT / "platformio.ini").read_text(encoding="utf-8")
    match = re.search(
        rf"^\[env:{re.escape(env)}\]\s*$([\s\S]*?)(?=^\[|\Z)",
        text,
        re.MULTILINE,
    )
    if not match:
        return None
    key_match = re.search(rf"^\s*{re.escape(key)}\s*=\s*(.+?)\s*$", match.group(1), re.MULTILINE)
    return key_match.group(1).strip() if key_match else None


def parse_pio_memory(output: str) -> dict[str, dict[str, int]]:
    out: dict[str, dict[str, int]] = {}
    for label, used, limit in PIO_MEMORY_RE.findall(output):
        key = "ram" if label == "RAM" else "flash"
        used_i = int(used)
        limit_i = int(limit)
        out[key] = {
            "used_bytes": used_i,
            "limit_bytes": limit_i,
            "headroom_bytes": limit_i - used_i,
        }
    return out


def parse_esp_idf_size_memory(output: str) -> dict[str, dict[str, int]]:
    """Extract exclusive IRAM and shared DIRAM rows from esp-idf-size JSON."""

    try:
        report = json.loads(output)
    except json.JSONDecodeError as exc:
        raise ValueError(f"invalid esp-idf-size JSON: {exc}") from exc
    if not isinstance(report, dict):
        raise ValueError("esp-idf-size JSON root must be an object")

    layout = report.get("layout")
    if not isinstance(layout, list):
        raise ValueError("esp-idf-size JSON missing layout array")

    rows: dict[str, dict[str, int]] = {}
    expected = {"IRAM": "iram", "DIRAM": "diram"}
    for entry in layout:
        if not isinstance(entry, dict) or entry.get("name") not in expected:
            continue

        name = str(entry["name"])
        try:
            limit = int(entry["total"])
            used = int(entry["used"])
            headroom = int(entry["free"])
        except (KeyError, TypeError, ValueError) as exc:
            raise ValueError(f"esp-idf-size {name} row is incomplete") from exc

        if limit - used != headroom:
            raise ValueError(
                f"esp-idf-size {name} row is inconsistent: "
                f"total={limit} used={used} free={headroom}"
            )
        rows[expected[name]] = {
            "used_bytes": used,
            "limit_bytes": limit,
            "headroom_bytes": headroom,
        }

    missing = sorted(set(expected.values()) - rows.keys())
    if missing:
        raise ValueError(f"esp-idf-size JSON missing memory rows: {', '.join(missing)}")
    return rows


def platformio_python_candidates(pio_cmd: str) -> list[Path]:
    """Return likely interpreters for the PlatformIO-installed esp-idf-size."""

    candidates: list[Path] = []
    pio_path = Path(pio_cmd) if "/" in pio_cmd else None
    if pio_path is None:
        resolved = shutil.which(pio_cmd)
        pio_path = Path(resolved) if resolved else None
    if pio_path is not None:
        candidates.append(pio_path.resolve().parent / "python")

    configured = os.environ.get("PLATFORMIO_PYTHON")
    if configured:
        candidates.append(Path(configured))
    candidates.extend(
        [
            ROOT / ".artifacts" / "pio-core-6.1.19" / "bin" / "python",
            Path.home() / ".platformio" / "penv" / "bin" / "python",
            Path(sys.executable),
        ]
    )

    unique: list[Path] = []
    for candidate in candidates:
        if candidate not in unique and candidate.is_file() and os.access(candidate, os.X_OK):
            unique.append(candidate)
    return unique


def load_esp_idf_size_memory(map_path: Path, pio_cmd: str) -> dict[str, dict[str, int]]:
    """Run esp-idf-size from PlatformIO's environment and parse its JSON."""

    failures: list[str] = []
    for python in platformio_python_candidates(pio_cmd):
        result = subprocess.run(
            [
                str(python),
                "-m",
                "esp_idf_size",
                "--format",
                "json2",
                "--no-color",
                str(map_path),
            ],
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        if result.returncode != 0:
            detail = result.stderr.strip().splitlines()
            failures.append(detail[-1] if detail else f"exit {result.returncode}")
            continue
        try:
            return parse_esp_idf_size_memory(result.stdout)
        except ValueError as exc:
            failures.append(str(exc))

    detail = f" ({'; '.join(failures)})" if failures else ""
    raise RuntimeError(
        "unable to run esp-idf-size from the PlatformIO Python environment" + detail
    )


def find_xtensa_size() -> str:
    exe = "xtensa-esp32s3-elf-size"
    found = shutil.which(exe)
    if found:
        return found

    candidates = [
        ROOT / ".pio" / "packages" / "toolchain-xtensa-esp-elf" / "bin" / exe,
        Path.home() / ".platformio" / "packages" / "toolchain-xtensa-esp-elf" / "bin" / exe,
    ]
    candidates.extend(
        sorted((Path.home() / ".platformio" / "packages").glob(f"toolchain-xtensa-esp32s3*/bin/{exe}"))
    )
    for candidate in candidates:
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return str(candidate)
    raise FileNotFoundError(f"{exe} not found")


def parse_memory_configuration(map_path: Path) -> dict[str, int]:
    text = map_path.read_text(encoding="utf-8", errors="replace")
    marker = "Memory Configuration"
    marker_idx = text.find(marker)
    if marker_idx < 0:
        raise ValueError(f"{map_path} missing Memory Configuration")

    next_marker = text.find("Linker script and memory map", marker_idx)
    block = text[marker_idx: next_marker if next_marker > marker_idx else len(text)]

    lengths: dict[str, int] = {}
    for match in MAP_MEMORY_RE.finditer(block):
        lengths[match.group("name")] = int(match.group("length"), 16)
    return lengths


def parse_size_sections(elf_path: Path) -> dict[str, int]:
    output = run([find_xtensa_size(), "-A", str(elf_path)])
    sections: dict[str, int] = {}
    for line in output.splitlines():
        match = SIZE_SECTION_RE.match(line.strip())
        if match:
            sections[match.group("section")] = int(match.group("size"))
    return sections


def evaluate_headroom(
    memory: dict[str, dict[str, int]],
    *,
    warn_diram_zero: bool,
    fail_diram_zero: bool,
) -> tuple[list[str], list[str], list[str]]:
    """Classify capacity findings without treating exclusive IRAM as a limit."""

    infos: list[str] = []
    warnings: list[str] = []
    errors: list[str] = []
    for name, row in memory.items():
        if row["headroom_bytes"] < 0:
            errors.append(f"{name} over limit by {-row['headroom_bytes']} bytes")

    iram = memory["iram"]
    diram = memory["diram"]
    if iram["headroom_bytes"] == 0:
        infos.append(
            "exclusive IRAM window is full; additional IRAM code uses shared "
            f"DIRAM ({diram['headroom_bytes']} bytes headroom)"
        )

    if diram["headroom_bytes"] == 0:
        message = "shared DIRAM has zero headroom"
        if fail_diram_zero:
            errors.append(message)
        elif warn_diram_zero:
            warnings.append(message)

    return infos, warnings, errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--env", default="waveshare-349")
    parser.add_argument("--pio", default=os.environ.get("PIO_CMD", "pio"))
    parser.add_argument("--no-build", action="store_true", help="reuse existing build artifacts")
    parser.add_argument("--build-log", type=Path, help="parse memory rows from a captured pio build log")
    parser.add_argument("--warn-diram-zero", action="store_true")
    parser.add_argument("--fail-diram-zero", action="store_true")
    parser.add_argument("--report-dir", type=Path, default=DEFAULT_REPORT_DIR)
    args = parser.parse_args(argv)

    build_dir = ROOT / ".pio" / "build" / args.env
    elf_path = build_dir / "firmware.elf"
    map_path = build_dir / "firmware.map"
    bin_path = build_dir / "firmware.bin"

    pio_memory: dict[str, dict[str, int]] = {}
    if args.build_log:
        build_log = args.build_log if args.build_log.is_absolute() else ROOT / args.build_log
        if not build_log.is_file():
            print(f"[memory] missing build log: {build_log}", file=sys.stderr)
            return 1
        build_log_text = build_log.read_text(encoding="utf-8", errors="replace")
        pio_memory.update(parse_pio_memory(build_log_text))

    if args.no_build:
        if not elf_path.is_file() or not map_path.is_file():
            print(
                f"[memory] missing build artifacts for {args.env}; run pio build first",
                file=sys.stderr,
            )
            return 1
    else:
        try:
            build_output = run([args.pio, "run", "-e", args.env])
            pio_memory.update(parse_pio_memory(build_output))
        except (OSError, RuntimeError) as exc:
            print(f"[memory] {exc}", file=sys.stderr)
            return 1

    try:
        memory_lengths = parse_memory_configuration(map_path)
        sections = parse_size_sections(elf_path)
        memory = dict(pio_memory)
        memory.update(load_esp_idf_size_memory(map_path, args.pio))
        if "ram" not in memory:
            ram_used = sections.get(".dram0.data", 0) + sections.get(".dram0.bss", 0) + sections.get(".noinit", 0)
            ram_limit = memory_lengths.get("dram0_0_seg", 0)
            memory["ram"] = {
                "used_bytes": ram_used,
                "limit_bytes": ram_limit,
                "headroom_bytes": ram_limit - ram_used,
            }
        if "flash" not in memory:
            flash_used = bin_path.stat().st_size if bin_path.is_file() else (
                sections.get(".flash.text", 0)
                + sections.get(".flash.rodata", 0)
                + sections.get(".flash.appdesc", 0)
            )
            max_size = read_platformio_value(args.env, "board_upload.maximum_size")
            flash_limit = int(max_size) if max_size and max_size.isdigit() else (
                memory_lengths.get("iram0_2_seg", 0) + memory_lengths.get("drom0_0_seg", 0)
            )
            memory["flash"] = {
                "used_bytes": flash_used,
                "limit_bytes": flash_limit,
                "headroom_bytes": flash_limit - flash_used,
            }
    except (OSError, RuntimeError, ValueError) as exc:
        print(f"[memory] {exc}", file=sys.stderr)
        return 1

    report = {
        "env": args.env,
        "memory": memory,
    }
    report_dir = args.report_dir if args.report_dir.is_absolute() else ROOT / args.report_dir
    report_dir.mkdir(parents=True, exist_ok=True)
    report_path = report_dir / f"{args.env}.json"
    report_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    infos, warnings, errors = evaluate_headroom(
        memory,
        warn_diram_zero=args.warn_diram_zero,
        fail_diram_zero=args.fail_diram_zero,
    )

    for info in infos:
        print(f"[memory] INFO: {info}")
    for warning in warnings:
        print(f"[memory] WARN: {warning}")

    print(
        "[memory] "
        + " ".join(
            f"{name}={row['used_bytes']}/{row['limit_bytes']} headroom={row['headroom_bytes']}"
            for name, row in sorted(memory.items())
        )
    )
    try:
        report_display = report_path.relative_to(ROOT)
    except ValueError:
        report_display = report_path
    print(f"[memory] wrote {report_display}")

    if errors:
        print("[memory] headroom check failed:")
        for error in errors:
            print(f"  - {error}")
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())

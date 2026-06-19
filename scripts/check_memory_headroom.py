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
ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")
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


def parse_advanced_memory(output: str) -> dict[str, dict[str, int]]:
    out: dict[str, dict[str, int]] = {}
    clean = ANSI_RE.sub("", output)
    for raw_line in clean.splitlines():
        if "│" not in raw_line:
            continue
        fields = [field.strip() for field in raw_line.split("│")]
        if len(fields) < 6:
            continue
        label = fields[1]
        if label != "IRAM":
            continue
        try:
            used = int(fields[2])
            remain = int(fields[4])
            limit = int(fields[5])
        except ValueError:
            continue
        out["iram"] = {
            "used_bytes": used,
            "limit_bytes": limit,
            "headroom_bytes": remain,
        }
    return out


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


def iram_usage(sections: dict[str, int], memory_lengths: dict[str, int]) -> dict[str, int]:
    used = sum(
        size
        for section, size in sections.items()
        if section.startswith(".iram0.") and not section.endswith(".dummy")
    )
    limit = memory_lengths.get("iram0_0_seg")
    if limit is None:
        raise ValueError("firmware map missing iram0_0_seg length")
    return {
        "used_bytes": used,
        "limit_bytes": limit,
        "headroom_bytes": limit - used,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--env", default="waveshare-349")
    parser.add_argument("--no-build", action="store_true", help="reuse existing build artifacts")
    parser.add_argument("--build-log", type=Path, help="parse memory rows from a captured pio build log")
    parser.add_argument("--warn-iram-zero", action="store_true")
    parser.add_argument("--fail-iram-zero", action="store_true")
    parser.add_argument("--report-dir", type=Path, default=DEFAULT_REPORT_DIR)
    args = parser.parse_args()

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
        pio_memory.update(parse_advanced_memory(build_log_text))

    if args.no_build:
        if not elf_path.is_file() or not map_path.is_file():
            print(
                f"[memory] missing build artifacts for {args.env}; run pio build first",
                file=sys.stderr,
            )
            return 1
    else:
        try:
            build_output = run(["pio", "run", "-e", args.env])
            pio_memory.update(parse_pio_memory(build_output))
            pio_memory.update(parse_advanced_memory(build_output))
        except (OSError, RuntimeError) as exc:
            print(f"[memory] {exc}", file=sys.stderr)
            return 1

    try:
        memory_lengths = parse_memory_configuration(map_path)
        sections = parse_size_sections(elf_path)
        memory = dict(pio_memory)
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
        if "iram" not in memory:
            memory["iram"] = iram_usage(sections, memory_lengths)
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

    errors: list[str] = []
    warnings: list[str] = []
    for name, row in memory.items():
        if row["headroom_bytes"] < 0:
            errors.append(f"{name} over limit by {-row['headroom_bytes']} bytes")

    if memory["iram"]["headroom_bytes"] == 0:
        message = "iram has zero headroom"
        if args.fail_iram_zero:
            errors.append(message)
        elif args.warn_iram_zero:
            warnings.append(message)

    for warning in warnings:
        print(f"[memory] WARN: {warning}")

    print(
        "[memory] "
        + " ".join(
            f"{name}={row['used_bytes']}/{row['limit_bytes']} headroom={row['headroom_bytes']}"
            for name, row in sorted(memory.items())
        )
    )
    print(f"[memory] wrote {report_path.relative_to(ROOT)}")

    if errors:
        print("[memory] headroom check failed:")
        for error in errors:
            print(f"  - {error}")
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())

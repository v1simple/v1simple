#!/usr/bin/env python3
"""Read partition geometry and the explicitly selected PlatformIO build inputs."""

from __future__ import annotations

import argparse
import csv
from dataclasses import asdict, dataclass
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
from typing import Iterator

ROOT = Path(__file__).resolve().parents[1]


@dataclass(frozen=True)
class Partition:
    name: str
    offset: int
    size: int

    @property
    def end(self) -> int:
        return self.offset + self.size


def partition_rows(path: Path) -> Iterator[list[str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        for row in csv.reader(handle):
            if row and not row[0].strip().startswith("#"):
                yield row


def read_partitions(path: Path) -> dict[str, Partition]:
    """Full release geometry: all rows parsed, last duplicate name wins."""
    partitions = {}
    for row in partition_rows(path):
        if len(row) < 5:
            raise ValueError(f"malformed partition row in {path}: {row!r}")
        name = row[0].strip()
        partitions[name] = Partition(name, int(row[3].strip(), 0), int(row[4].strip(), 0))
    for required in ("nvs", "app", "storage"):
        if required not in partitions:
            raise ValueError(f"partition table missing {required} partition")
    return partitions


def read_storage_partition_size(path: Path) -> int:
    """Storage-only consumers historically use the first matching size."""
    for row in partition_rows(path):
        if row[0].strip() == "storage":
            return int(row[4].strip(), 0)
    raise RuntimeError(f"storage partition not found in {path}")


def _output(command: list[str], root: Path, environment=None) -> str:
    result = subprocess.run(command, cwd=root, env=environment, text=True, capture_output=True, check=False)
    if result.returncode:
        raise RuntimeError(f"selected tool failed ({result.returncode}): {command[0]}\n{result.stderr.strip()}")
    return result.stdout


@dataclass(frozen=True)
class SelectedTools:
    pio: str | None
    python: str
    core_dir: str


def selected_tools(root: Path, pio: str) -> SelectedTools:
    executable = shutil.which(pio)
    if not executable:
        raise FileNotFoundError(f"selected PlatformIO executable not found: {pio}")
    info = json.loads(_output([executable, "system", "info", "--json-output"], root))
    if not isinstance(info, dict):
        raise ValueError("selected PlatformIO system info must be an object")
    values = []
    for key in ("python_exe", "core_dir"):
        entry = info.get(key)
        value = entry.get("value") if isinstance(entry, dict) else None
        if not isinstance(value, str) or not value:
            raise ValueError(f"selected PlatformIO did not report {key}")
        values.append(value)
    return SelectedTools(executable, *values)


@dataclass(frozen=True)
class FlashPolicy:
    mode: str
    frequency: str
    size: str

    def image_fields(self) -> dict[str, str]:
        return {"Flash mode": self.mode.upper(), "Flash freq": self.frequency, "Flash size": self.size.upper()}


def flash_policy(board_value) -> FlashPolicy:
    memory_type = str(board_value("build.arduino.memory_type", "")).lower()
    mode = str(board_value("build.flash_mode")).lower()
    if memory_type in ("opi_opi", "opi_qspi"):
        mode = "dout"
    elif mode == "none":
        raise ValueError("selected board is missing build.flash_mode")
    elif mode in ("qio", "qout"):
        mode = "dio"
    raw = str(board_value("build.f_flash")).rstrip("Ll")
    hz = int(raw)
    if hz % 1_000_000:
        raise ValueError(f"unsupported board_build.f_flash value: {raw}")
    size = board_value("upload.flash_size", None) or board_value("build.flash_size")
    if size is None:
        raise ValueError("selected board is missing flash size")
    return FlashPolicy(mode, f"{hz // 1_000_000}m", str(size))


@dataclass(frozen=True)
class BuildContext:
    root: str
    environment: str
    tools: SelectedTools
    build_dir: str
    partitions: str
    platform_dir: str
    arduino_dir: str
    libs_dir: str
    toolchain_dir: str
    memory_type: str
    maximum_size: str
    flash: FlashPolicy

    @property
    def sdkconfig(self) -> Path:
        return Path(self.libs_dir) / "esp32s3" / self.memory_type / "include/sdkconfig.h"

    def filesystem_block_size(self) -> int:
        builder = Path(self.platform_dir) / "builder/main.py"
        source = builder.read_text(encoding="utf-8")
        match = re.search(r'env\["FS_BLOCK"\]\s*=\s*int\("([^"]+)",\s*16\)', source)
        if not match:
            raise ValueError(f"could not determine PlatformIO LittleFS block size from {builder}")
        return int(match.group(1), 16)

    def filesystem_name_max(self) -> int:
        source = self.sdkconfig.read_text(encoding="utf-8")
        match = re.search(r"#define\s+CONFIG_LITTLEFS_OBJ_NAME_LEN\s+(\d+)", source)
        if not match:
            raise ValueError(f"could not determine runtime LittleFS name limit from {self.sdkconfig}")
        return int(match.group(1))

    def to_json(self) -> str:
        return json.dumps(asdict(self), sort_keys=True)

    @classmethod
    def from_json(cls, value: str) -> BuildContext:
        data = json.loads(value)
        data["tools"] = SelectedTools(**data["tools"])
        data["flash"] = FlashPolicy(**data["flash"])
        return cls(**data)


def _resolved_context(root: Path, environment: str, tools: SelectedTools,
                      build_dir: str, platform, board_value, expected_flash: FlashPolicy, package_dir=None) -> BuildContext:
    def package(name: str) -> str:
        value = (package_dir or platform.get_package_dir)(name)
        if not value:
            raise ValueError(f"selected platform package is unavailable: {name}")
        return str(value)

    partitions = Path(str(board_value("build.partitions")))
    if not partitions.is_absolute():
        partitions = root / partitions
    return BuildContext(str(root), environment, tools, str(build_dir),
                        str(partitions), str(platform.get_dir()),
                        package("framework-arduinoespressif32"), package("framework-arduinoespressif32-libs"),
                        package("toolchain-xtensa-esp-elf"), str(board_value("build.arduino.memory_type", "qio_qspi")),
                        str(board_value("upload.maximum_size", "")), expected_flash)


def installed_package_dir(platform, name: str, overrides: list[str]):
    from platformio.package.meta import PackageSpec

    value = platform.get_package_dir(name)
    if value or name != "toolchain-xtensa-esp-elf":
        return value
    if any(platform.pm.ensure_spec(item.split("@", 1)[0]).name == name for item in overrides):
        return None
    # The pinned platform remaps its default toolchain archive to an installed
    # local IDF tool only when package-version matches exactly. Read the same
    # selected package metadata without running its installer/cleanup hook.
    version = platform.packages[name].get("package-version")
    if not version:
        return None
    package = platform.pm.get_package(PackageSpec(name=name, requirements=version))
    return package.path if package and str(package.metadata.version) == version else None


def resolved_platform_context(root: Path, environment: str, tools: SelectedTools) -> BuildContext:
    from platformio.package.manager.platform import PlatformPackageManager
    from platformio.platform.base import PlatformBase
    from platformio.project.config import ProjectConfig

    config = ProjectConfig.get_instance(str(root / "platformio.ini"))
    section = "env:" + environment
    spec = config.get(section, "platform", None)
    if not spec:
        raise ValueError(f"selected environment has no platform: {environment}")
    directory = Path(spec)
    if not directory.is_absolute():
        directory = root / directory
    if not directory.is_dir():
        package = PlatformPackageManager().get_package(spec)
        if not package:
            raise ValueError(f"selected platform is not installed: {spec}")
        directory = Path(package.path)
    # Construct only the core metadata reader. Factory.from_env dispatches the
    # platform subclass's package hooks even with autoinstall=False; those hooks
    # can install tools, modify environments and patch framework sources.
    platform = PlatformBase(str(directory / "platform.json"))
    platform.project_env = environment
    platform.configure_project_packages(environment)
    board = platform.board_config(config.get(section, "board"))
    overrides = config.get(section, "platform_packages", [])

    def board_value(key, default=None):
        return config.get(section, "board_" + key, board.get(key, default))

    return _resolved_context(root, environment, tools, str(Path(config.get("platformio", "build_dir")) / environment), platform, board_value,
                             configured_flash_policy(root, environment),
                             lambda name: installed_package_dir(platform, name, overrides))


def configured_flash_policy(root: Path, environment: str) -> FlashPolicy:
    from platformio.project.config import ProjectConfig

    config = ProjectConfig.get_instance(str(root / "platformio.ini"))
    def board_value(key, default=None):
        return config.get("env:" + environment, "board_" + key, default)
    return flash_policy(board_value)


def resolve_flash_policy(root: Path, environment: str, pio: str) -> FlashPolicy:
    root = root.resolve()
    tools = selected_tools(root, pio)
    command = [tools.python, str(Path(__file__).resolve()), "--resolve-platform", "--flash-policy",
               "--root", str(root), "--env", environment]
    value = _output(command, root, dict(os.environ, PLATFORMIO_CORE_DIR=tools.core_dir))
    return FlashPolicy(**json.loads(value))


def resolve(root: Path, environment: str, pio: str) -> BuildContext:
    root = root.resolve()
    tools = selected_tools(root, pio)
    command = [tools.python, str(Path(__file__).resolve()), "--resolve-platform", "--root", str(root),
               "--env", environment, "--tools-json", json.dumps(asdict(tools))]
    return BuildContext.from_json(_output(command, root, dict(os.environ, PLATFORMIO_CORE_DIR=tools.core_dir)))


def from_scons(env) -> BuildContext:
    # SCons has already resolved the board and platform; do not rediscover them.
    tools = SelectedTools(os.environ.get("PIO_CMD"), env.subst("$PYTHONEXE"), env.subst("$PROJECT_CORE_DIR"))
    board = env.BoardConfig()
    context = _resolved_context(Path(env.subst("$PROJECT_DIR")), env["PIOENV"], tools,
                                env.subst("$BUILD_DIR"), env.PioPlatform(), board.get,
                                flash_policy(lambda key, default=None: env.GetProjectOption("board_" + key, default)))
    return context


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--env", default=os.environ.get("PIO_ENV", "waveshare-349"))
    parser.add_argument("--pio", default=os.environ.get("PIO_CMD", "pio"))
    parser.add_argument("--flash-policy", action="store_true")
    parser.add_argument("--get", choices=("build_dir", "partitions", "python", "core_dir"))
    parser.add_argument("--resolve-platform", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--tools-json", help=argparse.SUPPRESS)
    args = parser.parse_args()
    try:
        if args.flash_policy:
            policy = (configured_flash_policy(args.root.resolve(), args.env) if args.resolve_platform
                      else resolve_flash_policy(args.root, args.env, args.pio))
            print(json.dumps(asdict(policy)))
            return 0
        if args.resolve_platform:
            context = resolved_platform_context(args.root.resolve(), args.env, SelectedTools(**json.loads(args.tools_json)))
        else:
            context = resolve(args.root, args.env, args.pio)
        if args.get in ("python", "core_dir"):
            print(getattr(context.tools, args.get))
        elif args.get:
            print(getattr(context, args.get))
        else:
            print(context.to_json())
    except (OSError, RuntimeError, ValueError, KeyError, TypeError) as exc:
        print(f"[build-context] {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

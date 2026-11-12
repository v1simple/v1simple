#!/usr/bin/env python3
"""Read qualified host requirements using only an available bootstrap Python."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
from pathlib import Path
import re
import sys

DEFAULT_MANIFEST = Path(__file__).resolve().parents[1] / "config" / "host_tools.json"


@dataclass(frozen=True)
class HostTools:
    values: dict[str, str]
    package_requirements: dict[str, str]
    profiles: dict[str, tuple[str, ...]]

    def get(self, name: str) -> str:
        try:
            return self.values[name]
        except KeyError as exc:
            raise ValueError("unknown host-tool value: " + name) from exc

    def required_tools(self, profile: str) -> tuple[str, ...]:
        if profile not in ("ci", "release"):
            raise ValueError("unknown host-tool profile: " + profile)
        return self.profiles["common"] + self.profiles[profile]

    def packages(self, profile: str) -> tuple[str, ...]:
        return tuple(self.package_requirements[name] for name in self.required_tools(profile)
                     if name in self.package_requirements)


def _object(value: object, keys: set[str], label: str) -> dict:
    if not isinstance(value, dict) or set(value) != keys:
        raise ValueError(label + " must contain exactly: " + ", ".join(sorted(keys)))
    return value


def _text(value: object, pattern: str, label: str) -> str:
    if not isinstance(value, str) or not re.fullmatch(pattern, value):
        raise ValueError("invalid host-tool " + label)
    return value


def _unique_object(pairs: list[tuple[str, object]]) -> dict:
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate host-tool key: " + key)
        result[key] = value
    return result


def load(path: Path = DEFAULT_MANIFEST) -> HostTools:
    document = json.loads(Path(path).read_text(encoding="utf-8"), object_pairs_hook=_unique_object)
    data = _object(document, {"schema", "python", "node", "python_packages", "shellcheck",
                              "swift", "linux", "macos", "profiles"}, "host tools")
    if type(data["schema"]) is not int or data["schema"] != 1:
        raise ValueError("unsupported host-tool schema")
    node = _object(data["node"], {"version", "npm", "linux_x64_sha256"}, "node")
    swift = _object(data["swift"], {"version", "ubuntu_archive_sha256"}, "swift")
    linux = _object(data["linux"], {"id", "version", "image"}, "linux")
    macos = _object(data["macos"], {"developer_dir"}, "macos")
    package_names = {"pioarduino", "esptool", "esp-idf-size", "Pillow", "littlefs-python", "ruff", "cryptography", "numpy"}
    packages = _object(data["python_packages"], package_names, "python_packages")
    profiles = _object(data["profiles"], {"common", "ci", "release"}, "profiles")
    version = r"[0-9]+\.[0-9]+\.[0-9]+"
    values = {
        "python": _text(data["python"], r"[0-9]+\.[0-9]+", "Python version"),
        "node": _text(node["version"], version, "Node version"),
        "npm": _text(node["npm"], version, "npm version"),
        "shellcheck": _text(data["shellcheck"], version, "ShellCheck version"),
        "swift": _text(swift["version"], version, "Swift version"),
        "node_sha256": _text(node["linux_x64_sha256"], r"[0-9a-f]{64}", "Node digest"),
        "swift_sha256": _text(swift["ubuntu_archive_sha256"], r"[0-9a-f]{64}", "Swift digest"),
        "linux_id": _text(linux["id"], r"ubuntu", "Linux distribution"),
        "linux_version": _text(linux["version"], r"[0-9]{2}\.[0-9]{2}", "Linux version"),
        "linux_image": _text(linux["image"], r"docker\.io/library/ubuntu@sha256:[0-9a-f]{64}", "Linux image"),
        "macos_developer_dir": _text(macos["developer_dir"],
            r"/Applications/Xcode_[0-9.]+\.app/Contents/Developer", "Xcode selection"),
    }
    requirements = {}
    for name, spec in packages.items():
        exact = name in {"pioarduino", "esptool", "esp-idf-size", "littlefs-python", "ruff"}
        pattern = r"==" + version if exact else r"(?:>=|<)[0-9]+(?:\.[0-9]+)*(?:,(?:>=|<)[0-9]+(?:\.[0-9]+)*)*"
        requirements[name] = name + _text(spec, pattern, name + " requirement")
        if exact:
            values[name] = spec[2:]
        values[name + "_requirement"] = requirements[name]
    values["pio_distribution"] = "pioarduino"
    values["pio_venv_name"] = "pio-core-" + values["pioarduino"]
    values["linux_runner"] = values["linux_id"] + "-" + values["linux_version"]
    known_tools = package_names | {"python", "node", "npm", "shellcheck", "swift", "ffmpeg", "libpcre3"}
    validated_profiles = {}
    for profile, tools in profiles.items():
        if (not isinstance(tools, list) or any(not isinstance(tool, str) or tool not in known_tools for tool in tools)
                or len(tools) != len(set(tools))):
            raise ValueError("invalid host-tool membership: " + profile)
        validated_profiles[profile] = tuple(tools)
    if any(set(validated_profiles["common"]) & set(validated_profiles[profile]) for profile in ("ci", "release")):
        raise ValueError("profile repeats common host tools")
    return HostTools(values, requirements, validated_profiles)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("get").add_argument("name")
    commands.add_parser("packages").add_argument("profile", choices=("ci", "release"))
    requires = commands.add_parser("requires")
    requires.add_argument("profile", choices=("ci", "release"))
    requires.add_argument("tool")
    commands.add_parser("workflow").add_argument("platform", choices=("linux", "macos"))
    args = parser.parse_args()
    try:
        tools = load(args.manifest)
        if args.command == "get":
            print(tools.get(args.name))
        elif args.command == "requires":
            return 0 if args.tool in tools.required_tools(args.profile) else 1
        elif args.command == "packages":
            print("\n".join(tools.packages(args.profile)))
        else:
            names = ("python", "node") if args.platform == "linux" else ("macos_developer_dir",)
            for name in names:
                print(name + "=" + tools.get(name))
    except (OSError, ValueError) as exc:
        print("[host-tools] " + str(exc), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

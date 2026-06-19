#!/usr/bin/env python3
"""Validate the ESP Web Tools installer page and generated release assets."""

from __future__ import annotations

import argparse
import json
import sys
from html.parser import HTMLParser
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class InstallerParser(HTMLParser):
    def __init__(self) -> None:
        super().__init__()
        self.install_manifests: list[str] = []
        self.module_scripts: list[str] = []

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        attr = {name: value or "" for name, value in attrs}
        if tag == "esp-web-install-button" and "manifest" in attr:
            self.install_manifests.append(attr["manifest"])
        if tag == "script" and attr.get("type") == "module" and "src" in attr:
            self.module_scripts.append(attr["src"])


def display_path(path: Path) -> str:
    try:
        return str(path.relative_to(ROOT))
    except ValueError:
        return str(path)


def validate_manifest(site_dir: Path, manifest_path: str, require_assets: bool) -> list[str]:
    errors: list[str] = []
    manifest_file = site_dir / manifest_path
    if not manifest_file.is_file():
        if require_assets:
            return [f"manifest not found: {display_path(manifest_file)}"]
        return []

    try:
        manifest = json.loads(manifest_file.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        return [f"{display_path(manifest_file)} is invalid JSON: {exc}"]

    if manifest.get("name") != "V1-Simple":
        errors.append("manifest name must be V1-Simple")
    if not isinstance(manifest.get("version"), str) or not manifest["version"]:
        errors.append("manifest version must be a non-empty string")

    builds = manifest.get("builds")
    if not isinstance(builds, list) or len(builds) != 1:
        errors.append("manifest must contain exactly one build")
        return errors

    build = builds[0]
    if build.get("chipFamily") != "ESP32-S3":
        errors.append("manifest build chipFamily must be ESP32-S3")

    parts = build.get("parts")
    if not isinstance(parts, list) or len(parts) != 1:
        errors.append("manifest build must contain exactly one merged-image part")
        return errors

    part = parts[0]
    if part.get("path") != "merged-firmware.bin":
        errors.append("manifest part path must be merged-firmware.bin")
    if part.get("offset") != 0:
        errors.append("manifest merged-firmware.bin offset must be 0")

    if require_assets:
        firmware_path = site_dir / str(part.get("path", ""))
        if not firmware_path.is_file():
            errors.append(f"merged firmware not found: {display_path(firmware_path)}")
        elif firmware_path.stat().st_size == 0:
            errors.append(f"merged firmware is empty: {display_path(firmware_path)}")

    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--site-dir", required=True, type=Path)
    parser.add_argument(
        "--template-only",
        action="store_true",
        help="Validate page wiring without requiring generated manifest/binary assets.",
    )
    args = parser.parse_args()

    site_dir = args.site_dir if args.site_dir.is_absolute() else ROOT / args.site_dir
    index = site_dir / "index.html"
    if not index.is_file():
        print(f"[web-installer] missing {display_path(index)}", file=sys.stderr)
        return 1

    parser_obj = InstallerParser()
    parser_obj.feed(index.read_text(encoding="utf-8"))

    errors: list[str] = []
    if parser_obj.install_manifests != ["manifest.json"]:
        errors.append(
            "index.html must contain exactly one esp-web-install-button pointing at manifest.json"
        )

    has_install_script = any(
        src.startswith("https://unpkg.com/esp-web-tools@")
        and "dist/web/install-button.js" in src
        for src in parser_obj.module_scripts
    )
    if not has_install_script:
        errors.append("index.html must load the pinned ESP Web Tools install-button module")

    if parser_obj.install_manifests:
        errors.extend(
            validate_manifest(
                site_dir,
                parser_obj.install_manifests[0],
                require_assets=not args.template_only,
            )
        )

    if errors:
        print("[web-installer] contract failed:", file=sys.stderr)
        for error in errors:
            print(f"  - {error}", file=sys.stderr)
        return 1

    mode = "template" if args.template_only else "release site"
    print(f"[web-installer] {mode} contract OK: {display_path(site_dir)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

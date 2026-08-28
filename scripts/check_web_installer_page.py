#!/usr/bin/env python3
"""Validate the ESP Web Tools installer page and generated release assets."""

from __future__ import annotations

import argparse
import json
import sys
from html.parser import HTMLParser
from pathlib import Path

import write_release_manifests as release_manifests


ROOT = Path(__file__).resolve().parents[1]
PARTITIONS_PATH = ROOT / "partitions_v1.csv"
PROJECT_LICENSE_PATH = "LICENSE"
NOTICE_PATH = "THIRD_PARTY_NOTICES.md"
BRANDING_PATHS = (
    "../interface/static/branding/v1simple-logo-transparent.png",
    "v1simple-logo-transparent.png",
)
LICENSE_PATHS = (
    "licenses/ArduinoJson-LICENSE.txt",
    "licenses/NimBLE-Arduino-LICENSE.txt",
    "licenses/NimBLE-Arduino-NOTICE.txt",
    "licenses/Arduino-GFX-LICENSE.txt",
    "licenses/OpenFontRender-LICENSE.txt",
    "licenses/FreeType-FTL.txt",
    "licenses/GNU-FreeFont-COPYING.txt",
    "licenses/GNU-FreeFont-README.txt",
    "licenses/OFL-1.1.txt",
    "licenses/Geist-OFL-1.1.txt",
    "licenses/CC-BY-4.0.txt",
    "licenses/Svelte-LICENSE.md",
    "licenses/SvelteKit-LICENSE.txt",
    "licenses/daisyUI-LICENSE.txt",
    "licenses/Tailwind-CSS-LICENSE.txt",
)


class InstallerParser(HTMLParser):
    def __init__(self) -> None:
        super().__init__()
        self.install_manifests: list[str] = []
        self.install_modes: list[str] = []
        self.module_scripts: list[str] = []
        self.links: list[str] = []
        self.images: list[str] = []

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        attr = {name: value or "" for name, value in attrs}
        if tag == "esp-web-install-button" and "manifest" in attr:
            self.install_manifests.append(attr["manifest"])
            self.install_modes.append(attr.get("data-install-mode", ""))
        if tag == "script" and attr.get("type") == "module" and "src" in attr:
            self.module_scripts.append(attr["src"])
        if tag == "a" and "href" in attr:
            self.links.append(attr["href"])
        if tag == "img" and "src" in attr:
            self.images.append(attr["src"])


def display_path(path: Path) -> str:
    try:
        return str(path.relative_to(ROOT))
    except ValueError:
        return str(path)


def validate_manifest(
    site_dir: Path,
    manifest_path: str,
    require_assets: bool,
    expected_version: str | None,
) -> tuple[list[str], str | None]:
    errors: list[str] = []
    manifest_file = site_dir / manifest_path
    if not manifest_file.is_file():
        if require_assets:
            return [f"manifest not found: {display_path(manifest_file)}"], None
        return [], None

    try:
        manifest = json.loads(manifest_file.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        return [f"{display_path(manifest_file)} is invalid JSON: {exc}"], None

    if manifest.get("name") != "V1-Simple":
        errors.append("manifest name must be V1-Simple")
    version = manifest.get("version")
    if not isinstance(version, str) or not version:
        errors.append("manifest version must be a non-empty string")
        version = None
    elif expected_version is not None and version != expected_version:
        errors.append(
            f"{manifest_path} version must be {expected_version}, got {version}"
        )
    if manifest.get("new_install_prompt_erase") is not True:
        errors.append(
            f"{manifest_path} must require an explicit ESP Web Tools erase choice"
        )

    builds = manifest.get("builds")
    if not isinstance(builds, list) or len(builds) != 1:
        errors.append("manifest must contain exactly one build")
        return errors, version

    build = builds[0]
    if build.get("chipFamily") != "ESP32-S3":
        errors.append("manifest build chipFamily must be ESP32-S3")

    parts = build.get("parts")
    if not isinstance(parts, list) or len(parts) != 1:
        errors.append("manifest build must contain exactly one firmware part")
        return errors, version

    part = parts[0]
    partitions = release_manifests.read_partitions(PARTITIONS_PATH)
    if manifest_path == release_manifests.UPDATE_MANIFEST:
        expected_path = release_manifests.UPDATE_IMAGE
        expected_offset = partitions["app"].offset
        description = "app-only update"
    elif manifest_path == release_manifests.FRESH_MANIFEST:
        expected_path = release_manifests.FRESH_IMAGE
        expected_offset = 0
        description = "destructive fresh-install image"
    else:
        errors.append(f"unsupported installer manifest: {manifest_path}")
        return errors, version

    if part.get("path") != expected_path:
        errors.append(f"{manifest_path} part path must be {expected_path}")
    if part.get("offset") != expected_offset:
        errors.append(
            f"{manifest_path} {expected_path} offset must be {expected_offset:#x}"
        )

    if require_assets:
        firmware_path = site_dir / str(part.get("path", ""))
        if not firmware_path.is_file():
            errors.append(f"{description} not found: {display_path(firmware_path)}")
        elif firmware_path.stat().st_size == 0:
            errors.append(f"{description} is empty: {display_path(firmware_path)}")
        elif manifest_path == release_manifests.UPDATE_MANIFEST:
            start = expected_offset
            end = start + firmware_path.stat().st_size
            app = partitions["app"]
            if end > app.end:
                errors.append(f"{expected_path} exceeds the app partition")
            for name, partition in partitions.items():
                if name != "app" and release_manifests.overlaps(start, end, partition):
                    errors.append(f"{expected_path} overlaps {name} partition")
        else:
            storage = partitions["storage"]
            if firmware_path.stat().st_size < storage.end:
                errors.append(
                    f"{expected_path} does not contain the complete LittleFS partition"
                )

    return errors, version


def validate_notices(site_dir: Path, require_assets: bool) -> list[str]:
    errors: list[str] = []
    project_license = site_dir / PROJECT_LICENSE_PATH
    notice_file = site_dir / NOTICE_PATH
    if not require_assets:
        project_license = ROOT / PROJECT_LICENSE_PATH
        notice_file = ROOT / NOTICE_PATH

    if not project_license.is_file():
        errors.append(f"project license not found: {display_path(project_license)}")
    elif project_license.stat().st_size == 0:
        errors.append(f"project license is empty: {display_path(project_license)}")

    if not notice_file.is_file():
        errors.append(f"third-party notices not found: {display_path(notice_file)}")
        return errors

    notice_text = notice_file.read_text(encoding="utf-8")
    for relative_path in LICENSE_PATHS:
        if relative_path not in notice_text:
            errors.append(
                f"{display_path(notice_file)} must reference {relative_path}"
            )

        license_file = (site_dir if require_assets else ROOT) / relative_path
        if not license_file.is_file():
            errors.append(f"runtime license not found: {display_path(license_file)}")
        elif license_file.stat().st_size == 0:
            errors.append(f"runtime license is empty: {display_path(license_file)}")

    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--site-dir", required=True, type=Path)
    parser.add_argument(
        "--template-only",
        action="store_true",
        help="Validate page wiring without requiring generated manifest/binary assets.",
    )
    parser.add_argument(
        "--expected-version",
        help="Require both generated manifests to carry this release tag.",
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
    branding_references = [path for path in BRANDING_PATHS if path in parser_obj.images]
    if len(branding_references) != 1 or len(parser_obj.images) != 1:
        errors.append("index.html must contain exactly one approved V1-Simple branding image")
    else:
        branding_file = site_dir / branding_references[0]
        if not branding_file.is_file():
            errors.append(f"installer branding not found: {display_path(branding_file)}")
        elif branding_file.stat().st_size == 0:
            errors.append(f"installer branding is empty: {display_path(branding_file)}")

    expected_manifests = [
        release_manifests.UPDATE_MANIFEST,
        release_manifests.FRESH_MANIFEST,
    ]
    if parser_obj.install_manifests != expected_manifests:
        errors.append(
            "index.html must present the app-only update before the destructive fresh install"
        )
    if parser_obj.install_modes != ["preserve", "destructive"]:
        errors.append(
            "installer buttons must explicitly identify preserve and destructive modes"
        )

    index_text = index.read_text(encoding="utf-8")
    for required_text in (
        "Update firmware — preserve device data",
        "Writes only the app partition.",
        "existing LittleFS web interface are not part of this update image",
        "Fresh install — erase and rebuild",
        "destructive even if ESP Web Tools’ optional erase box is off",
        "Physical SD-card contents are not part of this flash operation.",
        "Checking “Erase device” changes this into a destructive erase.",
    ):
        if required_text not in index_text:
            errors.append(f"index.html must explain installer consequence: {required_text!r}")

    has_install_script = any(
        src.startswith("https://unpkg.com/esp-web-tools@")
        and "dist/web/install-button.js" in src
        for src in parser_obj.module_scripts
    )
    if not has_install_script:
        errors.append("index.html must load the pinned ESP Web Tools install-button module")

    for required_link in (PROJECT_LICENSE_PATH, NOTICE_PATH, *LICENSE_PATHS):
        if parser_obj.links.count(required_link) != 1:
            errors.append(
                f"index.html must contain exactly one visible link to {required_link}"
            )

    manifest_versions: list[str] = []
    for manifest_path in expected_manifests:
        manifest_errors, version = validate_manifest(
            site_dir,
            manifest_path,
            require_assets=not args.template_only,
            expected_version=args.expected_version,
        )
        errors.extend(manifest_errors)
        if version is not None:
            manifest_versions.append(version)
    if len(set(manifest_versions)) > 1:
        errors.append("fresh-install and app-only update manifests must use the same version")

    errors.extend(validate_notices(site_dir, require_assets=not args.template_only))

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

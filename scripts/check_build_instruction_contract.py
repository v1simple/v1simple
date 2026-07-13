#!/usr/bin/env python3
"""Validate tracked build and deploy instructions stay aligned with the repo policy."""

from __future__ import annotations

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DOC_PATHS = [ROOT / "README.md", *sorted((ROOT / "docs").glob("*.md"))]
README_PATH = ROOT / "README.md"
DEPLOY_SCRIPT_PATH = ROOT / "interface" / "scripts" / "deploy.js"
CONFIG_PATH = ROOT / "include" / "config.h"
CHANGELOG_PATH = ROOT / "CHANGELOG.md"
AUTHORITATIVE_UPLOAD_POLICY = (
    "Authoritative filesystem upload path: `./build.sh --upload-fs` or `./build.sh --all`."
)
FIRMWARE_VERSION_RE = re.compile(r'^\s*#define\s+FIRMWARE_VERSION\s+"([^"]+)"\s*$', re.MULTILINE)


def main() -> int:
    errors: list[str] = []
    usage_pattern = re.compile(r"\./build\.sh\b[^\n]*\s-n(\s|$)")

    for path in DOC_PATHS:
        if not path.exists():
            continue
        text = path.read_text(encoding="utf-8")
        if usage_pattern.search(text):
            errors.append(f"{path.relative_to(ROOT)} mentions unsupported build.sh -n flag")

    if README_PATH.exists():
        readme_text = README_PATH.read_text(encoding="utf-8")
        if AUTHORITATIVE_UPLOAD_POLICY not in readme_text:
            errors.append("README.md missing authoritative filesystem upload policy")
        if "pio run -e waveshare-349 -t uploadfs" in readme_text:
            errors.append("README.md should not advertise raw uploadfs usage")

    if DEPLOY_SCRIPT_PATH.exists():
        deploy_text = DEPLOY_SCRIPT_PATH.read_text(encoding="utf-8")
        if "./build.sh --upload-fs" not in deploy_text:
            errors.append("interface/scripts/deploy.js should point next steps at build.sh --upload-fs")
        if "uploadfs" in deploy_text:
            errors.append("interface/scripts/deploy.js should not recommend raw uploadfs commands")

    if not CHANGELOG_PATH.exists():
        errors.append("CHANGELOG.md is required by release docs")

    if CONFIG_PATH.exists():
        config_text = CONFIG_PATH.read_text(encoding="utf-8")
        version_match = FIRMWARE_VERSION_RE.search(config_text)
        if not version_match:
            errors.append('include/config.h missing plain #define FIRMWARE_VERSION "x.y.z"')
        elif CHANGELOG_PATH.exists():
            version = version_match.group(1)
            changelog_text = CHANGELOG_PATH.read_text(encoding="utf-8")
            entry_re = re.compile(rf"^##\s+\[?{re.escape(version)}\]?(?:\s|$)", re.MULTILINE)
            if not entry_re.search(changelog_text):
                errors.append(f"CHANGELOG.md missing entry for firmware version {version}")

    if errors:
        print("[contract] build-docs mismatch:")
        for error in errors:
            print(f"  - {error}")
        return 1

    print("[contract] tracked build docs match supported build.sh usage")
    return 0


if __name__ == "__main__":
    sys.exit(main())

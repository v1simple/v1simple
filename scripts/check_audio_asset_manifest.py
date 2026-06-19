#!/usr/bin/env python3
"""Verify voice clips match the curated manifest exactly.

Checks the committed *source* clips in ``tools/freq_audio/mulaw`` (always), and
the *deployed* clips in ``data/audio`` when present (i.e. after ``npm run
deploy``). The source check is the version-controlled guardrail; the deployed
check catches a stale or partial LittleFS deploy.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
MANIFEST_PATH = ROOT / "config" / "audio_asset_manifest.json"
SOURCE_DIR = ROOT / "tools" / "freq_audio" / "mulaw"
DEPLOY_DIR = ROOT / "data" / "audio"


def expand_manifest() -> list[str]:
    manifest = json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))
    files = set(manifest.get("files", []))
    for entry in manifest.get("ranges", []):
        prefix = entry.get("prefix", "")
        suffix = entry.get("suffix", "")
        width = int(entry.get("width", 0))
        start = int(entry.get("start", 0))
        end = int(entry.get("end", -1))
        for value in range(start, end + 1):
            files.add(f"{prefix}{value:0{width}d}{suffix}")
    return sorted(files)


def check_dir(label: str, directory: Path, expected: list[str]) -> bool:
    actual = sorted(path.name for path in directory.glob("*.mul"))
    missing = sorted(set(expected) - set(actual))
    unexpected = sorted(set(actual) - set(expected))
    if missing or unexpected:
        for name in missing:
            print(f"[audio-manifest] {label}: missing clip - {name}")
        for name in unexpected:
            print(f"[audio-manifest] {label}: unexpected clip - {name}")
        print(f"[audio-manifest] {label}: clips do NOT match manifest")
        return False
    print(f"[audio-manifest] {label}: {len(actual)} clips match manifest")
    return True


def main() -> int:
    if not MANIFEST_PATH.exists():
        print(f"[audio-manifest] manifest not found: {MANIFEST_PATH}")
        return 1

    expected = expand_manifest()
    ok = True

    # Source clips are committed: they must always be present and correct.
    if not SOURCE_DIR.exists():
        print(f"[audio-manifest] source dir not found: {SOURCE_DIR}")
        return 1
    ok = check_dir("source tools/freq_audio/mulaw", SOURCE_DIR, expected) and ok

    # Deployed clips only exist after `npm run deploy`; check them when present.
    if DEPLOY_DIR.exists():
        ok = check_dir("deployed data/audio", DEPLOY_DIR, expected) and ok
    else:
        print("[audio-manifest] deployed data/audio not present (run npm run deploy to verify)")

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

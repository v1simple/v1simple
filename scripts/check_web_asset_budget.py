#!/usr/bin/env python3
"""Check web/UI packaging guardrails for embedded flash efficiency.

Guards:
1) No duplicate raw + .gz runtime assets under data/_app, except tiny
   direct-route compatibility assets that must serve clients without gzip.
2) No legacy ghz_*.mul clips under deployed data.
3) data/ total size stays within budget.
"""

from __future__ import annotations

import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
DATA_DIR = ROOT / "data"
APP_DIR = DATA_DIR / "_app"

# Keep a modest margin under current footprint for future UI polish assets.
DATA_SIZE_BUDGET_BYTES = 2_100_000
COMPRESSIBLE_EXTENSIONS = {".js", ".css", ".json"}
ALLOWED_APP_DUPLICATES = {
    APP_DIR / "env.js",
    APP_DIR / "version.json",
}


def all_files(root: Path) -> list[Path]:
    if not root.exists():
        return []
    return [p for p in root.rglob("*") if p.is_file()]


def compute_total_size(root: Path) -> int:
    return sum(p.stat().st_size for p in all_files(root))


def find_app_duplicates(app_dir: Path) -> list[tuple[Path, Path]]:
    duplicates: list[tuple[Path, Path]] = []
    if not app_dir.exists():
        return duplicates
    for p in all_files(app_dir):
        name = p.name
        if name.endswith(".gz"):
            continue
        if p.suffix not in COMPRESSIBLE_EXTENSIONS:
            continue
        if p in ALLOWED_APP_DUPLICATES:
            continue
        gz_path = p.with_name(f"{name}.gz")
        if gz_path.exists():
            duplicates.append((p, gz_path))
    return duplicates


def find_legacy_ghz_clips(data_dir: Path) -> list[Path]:
    if not data_dir.exists():
        return []
    return sorted(data_dir.rglob("ghz_*.mul"))


def rel(path: Path) -> str:
    return str(path.relative_to(ROOT))


def main() -> int:
    if not DATA_DIR.exists():
        print("[guardrail] data directory not found; run interface deploy first")
        return 1

    duplicates = find_app_duplicates(APP_DIR)
    legacy_ghz = find_legacy_ghz_clips(DATA_DIR)
    data_size = compute_total_size(DATA_DIR)

    failed = False

    if duplicates:
        failed = True
        print("[guardrail] duplicate raw+gz assets found under data/_app")
        for raw, gz in duplicates[:20]:
            print(f"  - {rel(raw)} (+ {rel(gz)})")
        if len(duplicates) > 20:
            print(f"  ... and {len(duplicates) - 20} more")

    if legacy_ghz:
        failed = True
        bytes_total = sum(p.stat().st_size for p in legacy_ghz)
        print(
            "[guardrail] legacy ghz clips found "
            f"({len(legacy_ghz)} files, {bytes_total} bytes)"
        )
        for p in legacy_ghz:
            print(f"  - {rel(p)}")

    if data_size > DATA_SIZE_BUDGET_BYTES:
        failed = True
        over = data_size - DATA_SIZE_BUDGET_BYTES
        print(
            "[guardrail] data size budget exceeded "
            f"({data_size} bytes > {DATA_SIZE_BUDGET_BYTES} bytes, +{over})"
        )

    if failed:
        print(
            "[guardrail] web asset guardrails failed; "
            "trim assets or update guardrails intentionally"
        )
        return 1

    print(
        "[guardrail] web asset guardrails pass "
        f"(data={data_size} bytes, budget={DATA_SIZE_BUDGET_BYTES}, "
        f"duplicates={len(duplicates)}, legacy_ghz={len(legacy_ghz)})"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())

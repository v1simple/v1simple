#!/usr/bin/env python3
"""Preview or remove bench camera videos older than 14 days; keep all other evidence."""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import stat
import subprocess
import tempfile
from datetime import datetime, timedelta, timezone
from pathlib import Path

DEFAULT_ROOT = Path(__file__).resolve().parents[1] / ".artifacts" / "bench"
RUN_NAME = re.compile(r"(\d{8}_\d{6})_[0-9a-f]+(?:_[2-9]\d*|_1\d+)?")
VIDEO_NAME = re.compile(r"(?:\.camera_preflight|evidence_exp\d+)\.mov")


def regular_file(path: Path) -> bool:
    return path.resolve() == path and stat.S_ISREG(path.lstat().st_mode)


def identity(path: Path) -> tuple[int, int, int, int]:
    info = path.lstat()
    return info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns


def videos_are_closed(paths: list[Path]) -> bool:
    """An unavailable or incomplete open-file check must never permit deletion."""
    lsof = shutil.which("lsof")
    if not lsof:
        raise RuntimeError("lsof is unavailable; no videos removed")
    result = subprocess.run(
        [lsof, "-n", "-P", "-F", "n", "--", *(str(path) for path in paths)],
        capture_output=True,
        text=True,
        timeout=30,
        check=False,
    )
    if result.stderr or result.returncode not in (0, 1):
        raise RuntimeError("open-file check failed; no videos removed from this run")
    return result.returncode == 1 and not result.stdout


def verify_open_file_check(root: Path) -> None:
    # Some restricted environments hide processes without an lsof error.
    # Prove it can see this deliberately open file before trusting empty results.
    with tempfile.NamedTemporaryFile(dir=root, prefix=".cleanup-open-check-") as stream:
        if videos_are_closed([Path(stream.name)]):
            raise RuntimeError("lsof cannot see an open file; no videos removed")


def write_marker(path: Path, value: dict) -> None:
    # Refuse links, and replace atomically so interruption cannot truncate the record.
    if path.is_symlink() or path.resolve() != path:
        raise RuntimeError("cleanup marker is a symlink")
    with tempfile.NamedTemporaryFile(mode="w", dir=path.parent, delete=False) as stream:
        temporary = Path(stream.name)
        json.dump(value, stream, indent=2)
        stream.write("\n")
    try:
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def cleanup(root: Path, *, apply: bool = False, now: datetime | None = None) -> dict:
    root = root.absolute()
    if root.resolve() != root or not root.is_dir():
        raise ValueError("bench root must be an existing directory without symlinks")
    now = now or datetime.now(timezone.utc)
    cutoff = now - timedelta(days=14)
    summary = {"apply": apply, "cutoff_utc": cutoff.isoformat(), "candidates": [],
               "deleted_files": 0, "deleted_bytes": 0, "skipped_runs": []}
    open_check_verified = False
    for camera in sorted(root.glob("*/runs/*/replay/camera")):
        run = camera.parent.parent
        match = RUN_NAME.fullmatch(run.name)
        if camera.resolve() != camera or not camera.is_dir() or not match:
            continue
        try:
            run_time = datetime.strptime(match[1], "%Y%m%d_%H%M%S").replace(tzinfo=timezone.utc)
        except ValueError:
            continue
        if run_time >= cutoff:
            continue
        videos = [path for path in sorted(camera.iterdir())
                  if VIDEO_NAME.fullmatch(path.name) and regular_file(path)
                  and path.stat().st_mtime < cutoff.timestamp()]
        if not videos:
            continue
        label = str(run.relative_to(root))
        result_path = camera / "camera_result.json"
        try:
            if not regular_file(result_path):
                raise ValueError("missing or unsafe camera result")
            result = json.loads(result_path.read_text())
            if result.get("result") not in {"CAPTURED", "CAPTURE_FAILED"}:
                raise ValueError("camera run has no terminal result")
        except (OSError, ValueError, AttributeError) as error:
            summary["skipped_runs"].append({"run": label, "reason": str(error)})
            continue
        snapshots = {path: identity(path) for path in videos}
        summary["candidates"].extend(
            {"path": str(path.relative_to(root)), "bytes": snapshots[path][2]}
            for path in videos
        )
        if not apply:
            continue
        if not open_check_verified:
            verify_open_file_check(root)
            open_check_verified = True
        if not videos_are_closed(videos):
            summary["skipped_runs"].append({"run": label, "reason": "video is open"})
            continue
        # A recorder or reviewer may have changed this run since selection.
        if any(not regular_file(path) or identity(path) != snapshots[path] for path in videos):
            summary["skipped_runs"].append({"run": label, "reason": "video changed"})
            continue
        marker_path = run / "camera_media_removed.json"
        previous = {}
        if marker_path.exists() or marker_path.is_symlink():
            if not regular_file(marker_path):
                raise RuntimeError("unsafe existing cleanup marker")
            previous = json.loads(marker_path.read_text())
            if (not isinstance(previous, dict)
                    or not isinstance(previous.get("removed_files"), list)
                    or not all(isinstance(name, str) for name in previous["removed_files"])
                    or not isinstance(previous.get("pending_files", []), list)
                    or not all(isinstance(name, str) for name in previous.get("pending_files", []))
                    or type(previous.get("removed_bytes")) is not int
                    or previous["removed_bytes"] < 0):
                raise ValueError("invalid existing cleanup marker; no videos removed from this run")
        marker = {
            "kind": "camera_media_cleanup", "timestamp_utc": now.isoformat(),
            "reason": "14-day camera video retention.",
            "removed_files": previous.get("removed_files", []),
            "removed_bytes": previous.get("removed_bytes", 0),
            # Unlink and marker replacement cannot be atomic together. Keep
            # earlier uncertain entries even if their files are now absent;
            # never silently claim a complete record after an interruption.
            "pending_files": list(dict.fromkeys(
                previous.get("pending_files", []) + [str(path.relative_to(run)) for path in videos]
            )),
            "pending_note": "Pending files may already be absent if cleanup stopped before recording removal.",
            "retained": "All reports, capture manifests, logs, timing sidecars, and still images.",
            "verification_limit": "Removed original videos cannot be replayed or revalidated.",
        }
        write_marker(marker_path, marker)
        for path in videos:
            if not regular_file(path) or identity(path) != snapshots[path]:
                raise RuntimeError("video changed before deletion")
            path.unlink()
            name = str(path.relative_to(run))
            marker["pending_files"].remove(name)
            marker["removed_files"].append(name)
            marker["removed_bytes"] += snapshots[path][2]
            write_marker(marker_path, marker)
            summary["deleted_files"] += 1
            summary["deleted_bytes"] += snapshots[path][2]
    return summary


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=DEFAULT_ROOT, help="bench artifact root")
    parser.add_argument("--apply", action="store_true", help="delete eligible videos (default: preview)")
    args = parser.parse_args()
    try:
        result = cleanup(args.root, apply=args.apply)
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(json.dumps({"error": str(error)}))
        return 1
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

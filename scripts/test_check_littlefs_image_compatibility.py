#!/usr/bin/env python3
"""Regression tests for scripts/check_littlefs_image_compatibility.py."""

from __future__ import annotations

from contextlib import contextmanager, redirect_stderr, redirect_stdout
import io
from pathlib import Path
import subprocess
import sys
import tempfile
from typing import Any, Iterator

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

import check_littlefs_image_compatibility as checker  # type: ignore  # noqa: E402


def assert_equal(actual: Any, expected: Any, message: str) -> None:
    if actual != expected:
        raise AssertionError(f"{message}: expected {expected!r}, got {actual!r}")


def write_file(path: Path, data: bytes | str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if isinstance(data, bytes):
        path.write_bytes(data)
    else:
        path.write_text(data, encoding="utf-8")


def write_partition_table(path: Path, storage_bytes: int) -> None:
    write_file(
        path,
        "# Name, Type, SubType, Offset, Size, Flags\n"
        f"storage, data, spiffs, 0xDA0000, 0x{storage_bytes:x},\n",
    )


@contextmanager
def patched_checker(**replacements: Any) -> Iterator[None]:
    originals = {name: getattr(checker, name) for name in replacements}
    try:
        for name, replacement in replacements.items():
            setattr(checker, name, replacement)
        yield
    finally:
        for name, original in originals.items():
            setattr(checker, name, original)


def successful_list(*_: Any, **__: Any) -> subprocess.CompletedProcess[str]:
    return subprocess.CompletedProcess(["littlefs-python", "list"], 0, "/ok.txt\n", "")


def run_checker(tmpdir: Path, extra_args: list[str]) -> int:
    stdout = io.StringIO()
    stderr = io.StringIO()
    with redirect_stdout(stdout), redirect_stderr(stderr):
        return checker.main(
            [
                "--data-dir",
                str(tmpdir / "data"),
                "--partition-table",
                str(tmpdir / "partitions.csv"),
                "--runtime-name-max",
                "64",
                *extra_args,
            ]
        )


def test_clean_candidate_passes_with_stubbed_mount(tmpdir: Path) -> None:
    write_file(tmpdir / "data" / "ok.txt", "ok\n")
    write_partition_table(tmpdir / "partitions.csv", 16)
    write_file(tmpdir / "candidate.bin", b"\xff" * 16)

    with patched_checker(
        find_littlefs_python=lambda explicit: "/tmp/fake-littlefs-python",
        list_with_littlefs_python=successful_list,
    ):
        rc = run_checker(tmpdir, ["--candidate", str(tmpdir / "candidate.bin")])

    assert_equal(rc, 0, "clean candidate should pass")


def test_long_basename_fails(tmpdir: Path) -> None:
    write_file(tmpdir / "data" / ("a" * 65), "too long\n")
    write_partition_table(tmpdir / "partitions.csv", 16)

    rc = run_checker(tmpdir, ["--storage-bytes", "16"])

    assert_equal(rc, 1, "basename longer than runtime max should fail")


def test_candidate_size_mismatch_fails(tmpdir: Path) -> None:
    write_file(tmpdir / "data" / "ok.txt", "ok\n")
    write_partition_table(tmpdir / "partitions.csv", 16)
    write_file(tmpdir / "candidate.bin", b"\xff" * 8)

    with patched_checker(
        find_littlefs_python=lambda explicit: "/tmp/fake-littlefs-python",
        list_with_littlefs_python=successful_list,
    ):
        rc = run_checker(tmpdir, ["--candidate", str(tmpdir / "candidate.bin")])

    assert_equal(rc, 1, "candidate size mismatch should fail")


def test_missing_partition_table_fails_without_explicit_size(tmpdir: Path) -> None:
    write_file(tmpdir / "data" / "ok.txt", "ok\n")

    rc = run_checker(tmpdir, [])

    assert_equal(rc, 1, "missing partition table should fail closed")


def test_missing_littlefs_python_fails(tmpdir: Path) -> None:
    write_file(tmpdir / "data" / "ok.txt", "ok\n")
    write_partition_table(tmpdir / "partitions.csv", 16)
    write_file(tmpdir / "candidate.bin", b"\xff" * 16)

    with patched_checker(
        find_littlefs_python=lambda explicit: (_ for _ in ()).throw(FileNotFoundError("missing cli"))
    ):
        rc = run_checker(tmpdir, ["--candidate", str(tmpdir / "candidate.bin")])

    assert_equal(rc, 1, "missing littlefs-python should fail closed")


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="littlefs_image_check_") as tmp:
        tmpdir = Path(tmp)
        test_clean_candidate_passes_with_stubbed_mount(tmpdir / "clean")
        test_long_basename_fails(tmpdir / "long_name")
        test_candidate_size_mismatch_fails(tmpdir / "size_mismatch")
        test_missing_partition_table_fails_without_explicit_size(tmpdir / "missing_partition")
        test_missing_littlefs_python_fails(tmpdir / "missing_cli")

    print("[littlefs-image-compatibility] regression tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

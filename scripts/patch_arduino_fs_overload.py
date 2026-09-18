"""Retain the pinned Arduino FS byte-buffer overload.

Arduino-ESP32 3.3.11's ``fs::File::readBytes(char *, size)`` hides
``Stream::readBytes(uint8_t *, size)`` under GCC 14's overloaded-virtual
diagnostic.  Repair the owning framework declaration for every ESP32
environment before framework qualification.  Whole-file fingerprints make
the mutation fail closed when the pinned framework changes.
"""

Import("env")  # noqa: F821  (SCons construction environment)

import hashlib
from pathlib import Path


FS_UPSTREAM_SHA256 = "b0f42cd30763dbcd70d1a465f407a8b257749459b8169f603d92df4a8ef9100f"
FS_PATCHED_SHA256 = "53c9bb70cf573010434c05a546131f7831b448f51797a0c2badaec1c92b7784c"

FS_READ_UPSTREAM = """  size_t read(uint8_t *buf, size_t size);
  size_t readBytes(char *buffer, size_t length) {"""
FS_READ_PATCHED = """  size_t read(uint8_t *buf, size_t size);
  // v1simple-arduino-overload-compat-v1: retain Stream byte-buffer reads.
  using Stream::readBytes;
  size_t readBytes(char *buffer, size_t length) {"""


def fail(message: str) -> None:
    print(f"[patch_arduino_fs_overload] ERROR: {message}")
    env.Exit(1)  # noqa: F821


framework_root = Path(
    env.PioPlatform().get_package_dir("framework-arduinoespressif32") or ""  # noqa: F821
)
fs_header = framework_root / "libraries" / "FS" / "src" / "FS.h"
if not fs_header.is_file():
    fail(f"missing pinned dependency source: {fs_header}")

text = fs_header.read_text(encoding="utf-8")
actual = hashlib.sha256(text.encode("utf-8")).hexdigest()
if actual == FS_PATCHED_SHA256:
    if text.count(FS_READ_PATCHED) != 1:
        fail("existing overload repair is incomplete: FS.h")
    print("[patch_arduino_fs_overload] already applied: FS.h")
elif actual == FS_UPSTREAM_SHA256:
    if text.count(FS_READ_UPSTREAM) != 1 or FS_READ_PATCHED in text:
        fail("pinned source does not contain the unique expected overload: FS.h")
    patched = text.replace(FS_READ_UPSTREAM, FS_READ_PATCHED, 1)
    generated = hashlib.sha256(patched.encode("utf-8")).hexdigest()
    if generated != FS_PATCHED_SHA256:
        fail(
            "generated FS.h identity mismatch: "
            f"expected {FS_PATCHED_SHA256}, got {generated}"
        )
    fs_header.write_text(patched, encoding="utf-8")
    print("[patch_arduino_fs_overload] applied: FS.h")
else:
    fail(
        "FS.h identity mismatch: expected pristine or patched source, "
        f"got {actual}"
    )

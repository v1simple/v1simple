"""Repair pinned Arduino overload sets without weakening project warnings.

Arduino-ESP32 3.3.11 and Arduino_GFX 1.6.7 expose three intentional overload
sets that GCC 14 diagnoses under ``-Woverloaded-virtual``:

* Arduino_GFX::flush(bool) hides Print::flush().
* Arduino_GFX::write(uint8_t) hides Print::write(buffer, size).
* fs::File::readBytes(char *, size) hides Stream::readBytes(uint8_t *, size).

The production build treats every warning as an error.  Add the missing
overload bridges at their owning vendor declarations instead of suppressing a
warning class for project code.  Whole-file fingerprints make the patch fail
closed when either pinned dependency changes.
"""

Import("env")  # noqa: F821  (SCons construction environment)

import hashlib
from pathlib import Path


GFX_UPSTREAM_SHA256 = "c3bb932c99e59b536d92bef0a7ae767ea85a9c72a5de704f9d0f8e1a8fd0c2d4"
GFX_PATCHED_SHA256 = "932912cbb6d873fff74df5127763e44f3e89a1a03e9304f142500c0bc51b27a2"
FS_UPSTREAM_SHA256 = "b0f42cd30763dbcd70d1a465f407a8b257749459b8169f603d92df4a8ef9100f"
FS_PATCHED_SHA256 = "53c9bb70cf573010434c05a546131f7831b448f51797a0c2badaec1c92b7784c"

GFX_FLUSH_UPSTREAM = "  virtual void flush(bool force_flush = false);"
GFX_FLUSH_PATCHED = """  // v1simple-arduino-overload-compat-v1: implement the Print contract
  // explicitly while retaining the library force-flush overload.
  void flush() override { flush(false); }
  virtual void flush(bool force_flush);"""

GFX_WRITE_UPSTREAM = "  virtual size_t write(uint8_t);"
GFX_WRITE_PATCHED = """  // v1simple-arduino-overload-compat-v1: retain buffered Print writes.
  using Print::write;
  virtual size_t write(uint8_t);"""

FS_READ_UPSTREAM = """  size_t read(uint8_t *buf, size_t size);
  size_t readBytes(char *buffer, size_t length) {"""
FS_READ_PATCHED = """  size_t read(uint8_t *buf, size_t size);
  // v1simple-arduino-overload-compat-v1: retain Stream byte-buffer reads.
  using Stream::readBytes;
  size_t readBytes(char *buffer, size_t length) {"""


def fail(message: str) -> None:
    print(f"[patch_arduino_overloads] ERROR: {message}")
    env.Exit(1)  # noqa: F821


def patch_file(path: Path, upstream_hash: str, patched_hash: str,
               replacements: tuple[tuple[str, str], ...]) -> None:
    if not path.is_file():
        fail(f"missing pinned dependency source: {path}")

    text = path.read_text(encoding="utf-8")
    actual = hashlib.sha256(text.encode("utf-8")).hexdigest()
    if actual == patched_hash:
        if any(text.count(patched) != 1 for _, patched in replacements):
            fail(f"existing overload repair is incomplete: {path.name}")
        print(f"[patch_arduino_overloads] already applied: {path.name}")
        return
    if actual != upstream_hash:
        fail(
            f"{path.name} identity mismatch: expected pristine or patched source, got {actual}"
        )
    for upstream, patched in replacements:
        if text.count(upstream) != 1 or patched in text:
            fail(f"pinned source does not contain the unique expected overload: {path.name}")
        text = text.replace(upstream, patched, 1)
    generated = hashlib.sha256(text.encode("utf-8")).hexdigest()
    if generated != patched_hash:
        fail(
            f"generated {path.name} identity mismatch: expected {patched_hash}, got {generated}"
        )
    path.write_text(text, encoding="utf-8")
    print(f"[patch_arduino_overloads] applied: {path.name}")


gfx_header = (
    Path(env.subst("$PROJECT_LIBDEPS_DIR"))  # noqa: F821
    / env["PIOENV"]  # noqa: F821
    / "GFX Library for Arduino"
    / "src"
    / "Arduino_GFX.h"
)
framework_root = Path(
    env.PioPlatform().get_package_dir("framework-arduinoespressif32") or ""  # noqa: F821
)
fs_header = framework_root / "libraries" / "FS" / "src" / "FS.h"

patch_file(
    gfx_header,
    GFX_UPSTREAM_SHA256,
    GFX_PATCHED_SHA256,
    ((GFX_FLUSH_UPSTREAM, GFX_FLUSH_PATCHED), (GFX_WRITE_UPSTREAM, GFX_WRITE_PATCHED)),
)
patch_file(
    fs_header,
    FS_UPSTREAM_SHA256,
    FS_PATCHED_SHA256,
    ((FS_READ_UPSTREAM, FS_READ_PATCHED),),
)

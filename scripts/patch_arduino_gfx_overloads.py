"""Retain the pinned Arduino_GFX overload sets without global suppression.

Arduino_GFX 1.6.7's ``flush(bool)`` and ``write(uint8_t)`` hide overloads from
Arduino ``Print`` under GCC 14's overloaded-virtual diagnostic.  Repair the
owning vendor declarations only in environments that declare Arduino_GFX.
Whole-file fingerprints make the mutation fail closed when the dependency
changes.
"""

Import("env")  # noqa: F821  (SCons construction environment)

import hashlib
from pathlib import Path


GFX_UPSTREAM_SHA256 = "c3bb932c99e59b536d92bef0a7ae767ea85a9c72a5de704f9d0f8e1a8fd0c2d4"
GFX_PATCHED_SHA256 = "932912cbb6d873fff74df5127763e44f3e89a1a03e9304f142500c0bc51b27a2"

GFX_FLUSH_UPSTREAM = "  virtual void flush(bool force_flush = false);"
GFX_FLUSH_PATCHED = """  // v1simple-arduino-overload-compat-v1: implement the Print contract
  // explicitly while retaining the library force-flush overload.
  void flush() override { flush(false); }
  virtual void flush(bool force_flush);"""

GFX_WRITE_UPSTREAM = "  virtual size_t write(uint8_t);"
GFX_WRITE_PATCHED = """  // v1simple-arduino-overload-compat-v1: retain buffered Print writes.
  using Print::write;
  virtual size_t write(uint8_t);"""


def fail(message: str) -> None:
    print(f"[patch_arduino_gfx_overloads] ERROR: {message}")
    env.Exit(1)  # noqa: F821


gfx_header = (
    Path(env.subst("$PROJECT_LIBDEPS_DIR"))  # noqa: F821
    / env["PIOENV"]  # noqa: F821
    / "GFX Library for Arduino"
    / "src"
    / "Arduino_GFX.h"
)
if not gfx_header.is_file():
    fail(f"missing pinned dependency source: {gfx_header}")

text = gfx_header.read_text(encoding="utf-8")
actual = hashlib.sha256(text.encode("utf-8")).hexdigest()
if actual == GFX_PATCHED_SHA256:
    if text.count(GFX_FLUSH_PATCHED) != 1 or text.count(GFX_WRITE_PATCHED) != 1:
        fail("existing overload repair is incomplete: Arduino_GFX.h")
    print("[patch_arduino_gfx_overloads] already applied: Arduino_GFX.h")
elif actual == GFX_UPSTREAM_SHA256:
    replacements = (
        (GFX_FLUSH_UPSTREAM, GFX_FLUSH_PATCHED),
        (GFX_WRITE_UPSTREAM, GFX_WRITE_PATCHED),
    )
    for upstream, patched in replacements:
        if text.count(upstream) != 1 or patched in text:
            fail(
                "pinned source does not contain the unique expected overload: "
                "Arduino_GFX.h"
            )
        text = text.replace(upstream, patched, 1)
    generated = hashlib.sha256(text.encode("utf-8")).hexdigest()
    if generated != GFX_PATCHED_SHA256:
        fail(
            "generated Arduino_GFX.h identity mismatch: "
            f"expected {GFX_PATCHED_SHA256}, got {generated}"
        )
    gfx_header.write_text(text, encoding="utf-8")
    print("[patch_arduino_gfx_overloads] applied: Arduino_GFX.h")
else:
    fail(
        "Arduino_GFX.h identity mismatch: expected pristine or patched source, "
        f"got {actual}"
    )

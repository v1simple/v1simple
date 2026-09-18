"""Implement Print's write-availability contract in pinned NimBLE-Arduino.

NimBLE-Arduino 2.5.1 adds a const ``availableForWrite`` query whose signature
hides the non-const virtual declared by Arduino ``Print``.  Keep the const API
and add a forwarding override so calls through either type report the same TX
buffer capacity.  Whole-file fingerprints make the mutation fail closed when
the pinned dependency changes.
"""

Import("env")  # noqa: F821  (SCons construction environment)

import hashlib
from pathlib import Path


UPSTREAM_SHA256 = "6b5e0d4e75fbbb4a5bc84ce5892ed207b300122545e40598f0af84c9e499b22d"
PATCHED_SHA256 = "dc3e03909d2387e800ac8be18bc392f23496074a3f32dabc67aaa15ce01a5fc1"

UPSTREAM_DECLARATION = "    size_t availableForWrite() const;"
PATCHED_DECLARATION = """    // v1simple-nimble-stream-overload-v1: implement the Print contract.
    int availableForWrite() override {
        const auto& stream = static_cast<const NimBLEStream&>(*this);
        return static_cast<int>(stream.availableForWrite());
    }
    size_t availableForWrite() const;"""


def fail(message: str) -> None:
    print(f"[patch_nimble_stream_overload] ERROR: {message}")
    env.Exit(1)  # noqa: F821


header = (
    Path(env.subst("$PROJECT_LIBDEPS_DIR"))  # noqa: F821
    / env["PIOENV"]  # noqa: F821
    / "NimBLE-Arduino"
    / "src"
    / "NimBLEStream.h"
)
if not header.is_file():
    fail(f"missing pinned dependency source: {header}")

text = header.read_text(encoding="utf-8")
actual = hashlib.sha256(text.encode("utf-8")).hexdigest()
if actual == PATCHED_SHA256:
    if text.count(PATCHED_DECLARATION) != 1:
        fail("existing overload repair is incomplete: NimBLEStream.h")
    print("[patch_nimble_stream_overload] already applied: NimBLEStream.h")
elif actual == UPSTREAM_SHA256:
    if text.count(UPSTREAM_DECLARATION) != 1 or PATCHED_DECLARATION in text:
        fail("pinned source does not contain the unique expected overload")
    patched = text.replace(UPSTREAM_DECLARATION, PATCHED_DECLARATION, 1)
    generated = hashlib.sha256(patched.encode("utf-8")).hexdigest()
    if generated != PATCHED_SHA256:
        fail(
            "generated NimBLEStream.h identity mismatch: "
            f"expected {PATCHED_SHA256}, got {generated}"
        )
    header.write_text(patched, encoding="utf-8")
    print("[patch_nimble_stream_overload] applied: NimBLEStream.h")
else:
    fail(
        "NimBLEStream.h identity mismatch: expected pristine or patched source, "
        f"got {actual}"
    )

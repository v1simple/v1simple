#!/usr/bin/env python3
"""Pin the privacy scanner identical across the public and internal repositories.

The two repositories each carry their own copy of the snapshot scanner so that
neither depends on the other being checked out. That duplication is what let the
rulesets drift apart once already: the internal copy grew the operational
identifier rules, the public copy grew the replay-boundary rules, and for a
while the repository that actually gets published was the one that could not
recognise a real MAC address or a credential literal.

Nothing about that drift was visible in either repository on its own — each copy
looked deliberate and each test suite passed. So the guard is a pinned digest.
Editing the scanner in one repository fails this test until the digest is
updated, and updating the digest is the moment someone has to carry the same
edit across. When both repositories happen to be checked out side by side the
comparison is made directly as well.

    python3 scripts/test_scanner_parity.py
"""

from __future__ import annotations

import hashlib
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SIBLING_NAMES = ("v1simple", "v1simple-internal")

# Update together with any scanner change, in BOTH repositories. The failure
# message prints the digest to paste in.
CANONICAL = {
    "scripts/check_public_snapshot_privacy.py":
        "3cdb62486014f0ab00d2934dc0e212429781a5f5c72011a4711229e4ac162d53",
    "scripts/test_check_public_snapshot_privacy.py":
        "6a76d924de6b683b001e0c9a2c32761b0b16f39164c70a1abe123bc67206310c",
}


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def sibling_root() -> Path | None:
    """The other repository, when it is checked out beside this one."""
    for name in SIBLING_NAMES:
        if name == ROOT.name:
            continue
        candidate = ROOT.parent / name
        if (candidate / "scripts").is_dir():
            return candidate
    return None


def test_local_copies_match_the_pinned_digest() -> None:
    for relative, expected in CANONICAL.items():
        path = ROOT / relative
        assert path.is_file(), f"{relative} is missing from {ROOT.name}"
        actual = digest(path)
        assert actual == expected, (
            f"{relative} in {ROOT.name} does not match the pinned digest.\n"
            f"  expected {expected}\n"
            f"  actual   {actual}\n"
            "If this change is intended, apply the SAME file to the other "
            "repository and update CANONICAL in both copies of this test."
        )


def test_sibling_repository_is_byte_identical() -> None:
    sibling = sibling_root()
    if sibling is None:
        print("  (sibling repository not checked out — digest pin still enforced)")
        return
    for relative in CANONICAL:
        other = sibling / relative
        assert other.is_file(), f"{relative} is missing from {sibling.name}"
        assert digest(other) == digest(ROOT / relative), (
            f"{relative} differs between {ROOT.name} and {sibling.name}. "
            "The scanner must stay byte-identical in both repositories."
        )


def main() -> int:
    tests = (
        test_local_copies_match_the_pinned_digest,
        test_sibling_repository_is_byte_identical,
    )
    for test in tests:
        test()
        print(f"PASS {test.__name__}")
    print(f"PASS {len(tests)} scanner parity tests")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

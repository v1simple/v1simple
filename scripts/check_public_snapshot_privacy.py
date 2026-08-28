#!/usr/bin/env python3
"""Scan a tracked public snapshot for high-confidence privacy leaks."""

from __future__ import annotations

import argparse
from collections import defaultdict
from dataclasses import dataclass
import hashlib
import ipaddress
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys
from typing import Iterable, Iterator


# Optional site-local blocklist. Deliberately outside both repositories so the
# terms themselves are never tracked, never published, and never present in CI.
# One term per line, matched case-insensitively; blank lines and '#' comments
# are ignored. Absent file (the normal case in public CI) means no local terms.
#
# Findings from this list are reported by BLOCKLIST LINE NUMBER only. The terms
# are private, so echoing a match — or the source line containing it — would
# make the guard leak the very thing it protects.
LOCAL_TERMS_ENV = "V1SIMPLE_PRIVACY_TERMS"
LOCAL_TERMS_DEFAULT = Path.home() / ".config" / "v1simple" / "privacy_terms.txt"


EMAIL = re.compile(rb"(?<![A-Za-z0-9._%+-])([A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,})(?![A-Za-z0-9.-])")
UNIX_HOME = re.compile(rb"/(?:Users|home)/([^/\\\x00\r\n\t \"']+)")
WINDOWS_HOME = re.compile(rb"[A-Za-z]:\\Users\\([^\\\x00\r\n\t \"']+)")
SECRET_PATTERNS = (
    ("private-key", re.compile(rb"-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----")),
    ("github-token", re.compile(rb"gh[pousr]_[A-Za-z0-9_]{20,}")),
    ("aws-access-key", re.compile(rb"AKIA[0-9A-Z]{16}")),
    ("google-api-key", re.compile(rb"AIza[0-9A-Za-z_-]{30,}")),
    ("stripe-live-key", re.compile(rb"sk_live_[0-9A-Za-z]{16,}")),
)

# --- Operational identifiers -------------------------------------------------
# These rules catch a REAL identifier pasted in from live hardware or a live
# network. A fixture and a real value are structurally different, so each rule
# tests that structure rather than matching a denylist of known-fake strings —
# a denylist would need a new entry for every fixture anyone ever writes, and
# would say nothing about the value that actually matters.
#
# Nothing site-specific belongs here. A personal name, a home SSID or a known
# device address goes in the site-local blocklist, which lives outside every
# checkout and is reported by position only.
MAC_CANDIDATE = re.compile(rb"(?i)(?<![0-9a-f:])(?:[0-9a-f]{2}:){5}[0-9a-f]{2}(?![0-9a-f:])")
IPV4_CANDIDATE = re.compile(rb"(?<![0-9.])(?:[0-9]{1,3}\.){3}[0-9]{1,3}(?![0-9.])")
DEVICE_PATH = re.compile(rb"/dev/(?:cu|tty)\.([A-Za-z0-9._*?\[\]-]+)")
CREDENTIAL_LITERAL = re.compile(
    rb"(?i)\b(?:password|passwd|api[_-]?key|access[_-]?token|client[_-]?secret)\b"
    rb"\s*[:=]\s*(['\"])([^'\"\r\n]{4,})\1"
)
# Possessive prose only. A bare "<Capitalised> requested" matches ordinary
# release notes ("Reboot requested"), so it is not a privacy signal; real names
# are the site-local blocklist's job.
PERSONAL_PROSE = re.compile(
    rb"\b[A-Z][a-z]+(?:'|\xe2\x80\x99)s "
    rb"(?:account|car|correction|device|home|network|request|vehicle)\b"
)

# Values a human types when they mean "fill this in".
CREDENTIAL_PLACEHOLDERS = frozenset(
    {
        "changeme", "example", "none", "null", "passwd", "password", "placeholder",
        "redacted", "secret", "token", "unset", "value", "your_password", "yourpassword",
    }
)
# SHA-256 of fixture credential values that are already tracked in plain sight.
# Digests, not the values: an allowlist that quoted what it exempted would grow
# into a second copy of the thing this scanner exists to keep out. Add one with
#     python3 scripts/check_public_snapshot_privacy.py --digest '<value>'
ALLOWED_CREDENTIAL_DIGESTS = frozenset(
    {
        # interface/src/routes/settings/page.test.js — WiFi form fixtures
        "fcf730b6d95236ecd3c9fc2d92d7b6b2bb061514961aec041d6c7a7192f592e4",
        "0159438a9235d6abde38e49fb98944660d067d6b9b03d8a8f4ee4e522feb62cb",
    }
)
# Public IPv4 that is legitimately quoted in documentation and configuration.
WELL_KNOWN_PUBLIC_IPS = frozenset(
    {"1.1.1.1", "8.8.8.8", "8.8.4.4", "9.9.9.9", "1.0.0.1", "208.67.222.222"}
)

SAFE_EMAILS = frozenset({"noreply@github.com"})
SAFE_EMAIL_SUFFIXES = (
    "@example.invalid",
    "@users.noreply.github.com",
    ".invalid",
    ".test",
    ".example",
)
SAFE_HOME_NAMES = frozenset({"example", "private", "public", "runner", "user"})
PNG_METADATA_CHUNKS = frozenset({b"eXIf", b"iTXt", b"tEXt", b"zTXt"})

# Private runtime data is never valid public source, even if it is force-added
# past .gitignore. The replay tool has an intentionally closed source-only tree;
# additions require an explicit guard update and review.
PRIVATE_DATA_PATH_PARTS = frozenset(
    {
        ".artifacts",
        ".private",
        "captures",
        "encounters",
        "recordings",
        "replay-input",
        "replay-exports",
    }
)
REPLAY_ROOT = PurePosixPath("tools/v1replay")
REPLAY_EXACT_ALLOWED = frozenset(
    {
        REPLAY_ROOT / ".gitignore",
        REPLAY_ROOT / "LIGHTBLUE_CRIB.md",
        REPLAY_ROOT / "Package.swift",
        REPLAY_ROOT / "README.md",
        REPLAY_ROOT / "Resources/Info.plist",
        REPLAY_ROOT / "scripts/build.sh",
        REPLAY_ROOT / "verify/check_publication_safety.py",
        REPLAY_ROOT / "verify/verify_protocol.py",
    }
)
REPLAY_CAPTURE_FIELDS = (
    b'"samples"',
    b'"strength"',
    b'"direction"',
    b'"frequencyMHz"',
    b'"frequencyGHz"',
    b'"muteState"',
    b'"offsetSeconds"',
    b'"timestamp"',
)
PRIVATE_BINARY_SUFFIXES = frozenset(
    {".btsnoop", ".heic", ".jpeg", ".jpg", ".m4v", ".mov", ".mp4", ".pcap", ".pcapng", ".png"}
)
PUBLIC_BINARY_MEDIA_ALLOWED = frozenset(
    {
        PurePosixPath("interface/static/branding/v1simple-logo-transparent.png"),
        # Human-verified calibration crops; camera_reference.json binds each
        # exact image digest to its expected visible frequency and signature.
        PurePosixPath("scripts/bench/camera_reference_24150.png"),
        PurePosixPath("scripts/bench/camera_reference_34700.png"),
        PurePosixPath("scripts/bench/camera_reference_35500.png"),
    }
)


@dataclass(frozen=True)
class Finding:
    path: str
    rule: str
    line: int | None = None
    redacted: bool = False

    def render(self) -> str:
        location = f"{self.path}:{self.line}" if self.line is not None else self.path
        if self.redacted:
            # Rule already identifies the blocklist entry by position only.
            return f"{self.rule} matched at {location}"
        return f"{location}: {self.rule}"


def load_local_terms(path: Path | None = None) -> list[bytes]:
    """Read the site-local blocklist. Missing or unreadable file -> no terms."""
    if path is None:
        override = os.environ.get(LOCAL_TERMS_ENV)
        path = Path(override) if override else LOCAL_TERMS_DEFAULT
    try:
        raw = path.read_text(encoding="utf-8")
    except OSError:
        return []
    terms: list[bytes] = []
    for entry in raw.splitlines():
        stripped = entry.strip()
        if not stripped or stripped.startswith("#"):
            terms.append(b"")  # keep line numbers aligned with the file
            continue
        terms.append(stripped.lower().encode("utf-8"))
    return terms


def local_term_findings(path: str, data: bytes, terms: list[bytes]) -> list[Finding]:
    if not terms:
        return []
    lowered = data.lower()
    findings: list[Finding] = []
    for index, term in enumerate(terms, start=1):
        if not term:
            continue
        start = lowered.find(term)
        if start != -1:
            # Report position in the blocklist, never the term or the line text.
            findings.append(
                Finding(path, f"local-term #{index}", line_number(data, start), redacted=True)
            )
    return findings


def git_environment() -> dict[str, str]:
    environment = os.environ.copy()
    # Replacement refs can otherwise make a safe replacement object conceal the
    # real object that a push publishes. Likewise, a privacy gate must not fetch
    # missing objects on demand or silently inspect an incomplete object graph.
    environment["GIT_NO_REPLACE_OBJECTS"] = "1"
    environment["GIT_NO_LAZY_FETCH"] = "1"
    return environment


def run_git(repo: Path, *arguments: str, input_data: bytes | None = None) -> bytes:
    completed = subprocess.run(
        ["git", "-C", str(repo), *arguments],
        check=False,
        input=input_data,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        env=git_environment(),
    )
    if completed.returncode != 0:
        raise RuntimeError("Git object inspection failed")
    return completed.stdout


def validated_oid(raw: bytes) -> str:
    value = raw.strip().decode("ascii", errors="strict")
    if not re.fullmatch(r"[0-9a-fA-F]{40}|[0-9a-fA-F]{64}", value):
        raise RuntimeError("Git returned an invalid object identifier")
    return value.lower()


def resolve_object(repo: Path, revision: str) -> str:
    return validated_oid(run_git(repo, "rev-parse", "--verify", "--end-of-options", revision))


def resolve_commit(repo: Path, revision: str) -> str:
    object_id = resolve_object(repo, revision)
    return validated_oid(run_git(repo, "rev-parse", "--verify", f"{object_id}^{{commit}}"))


def repository_is_shallow(repo: Path) -> bool:
    result = run_git(repo, "rev-parse", "--is-shallow-repository").strip()
    if result not in {b"true", b"false"}:
        raise RuntimeError("Git returned an invalid shallow-repository status")
    return result == b"true"


def tree_entries(repo: Path, treeish: str) -> list[tuple[str, bytes]]:
    """Return every (blob oid, raw path) association from one complete tree."""
    output = run_git(repo, "ls-tree", "-r", "-z", "--full-tree", treeish)
    entries: list[tuple[str, bytes]] = []
    for record in output.split(b"\x00"):
        if not record:
            continue
        try:
            header, raw_path = record.split(b"\t", 1)
            mode, object_type, raw_oid = header.split(b" ", 2)
        except ValueError as exc:
            raise RuntimeError("Git returned a malformed tree entry") from exc
        if object_type == b"commit":
            continue  # gitlink/submodule: no file blob is published by this repository
        if object_type != b"blob" or not re.fullmatch(rb"[0-7]{6}", mode):
            raise RuntimeError("Git returned an unexpected tree entry")
        entries.append((validated_oid(raw_oid), raw_path))
    return entries


def _read_exact(stream: object, size: int) -> bytes:
    data = stream.read(size)  # type: ignore[attr-defined]
    if data is None or len(data) != size:
        raise RuntimeError("Git returned a truncated object")
    return data


def iter_git_objects(repo: Path, object_ids: Iterable[str]) -> Iterator[tuple[str, str, bytes]]:
    """Read each requested object once, streaming through one cat-file process."""
    process = subprocess.Popen(
        ["git", "-C", str(repo), "cat-file", "--batch"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        env=git_environment(),
    )
    if process.stdin is None or process.stdout is None:  # pragma: no cover - Popen contract
        process.kill()
        raise RuntimeError("Git object reader could not start")
    try:
        for requested_oid in object_ids:
            process.stdin.write(requested_oid.encode("ascii") + b"\n")
            process.stdin.flush()
            header = process.stdout.readline()
            parts = header.rstrip(b"\n").split(b" ")
            if len(parts) != 3 or parts[1] == b"missing":
                raise RuntimeError("A required Git object is missing")
            returned_oid = validated_oid(parts[0])
            try:
                object_type = parts[1].decode("ascii", errors="strict")
                size = int(parts[2])
            except (UnicodeDecodeError, ValueError) as exc:
                raise RuntimeError("Git returned a malformed object header") from exc
            if returned_oid != requested_oid or size < 0:
                raise RuntimeError("Git returned an unexpected object")
            data = _read_exact(process.stdout, size)
            if _read_exact(process.stdout, 1) != b"\n":
                raise RuntimeError("Git returned a malformed object boundary")
            yield returned_oid, object_type, data
        process.stdin.close()
        if process.wait() != 0:
            raise RuntimeError("Git object inspection failed")
    except Exception:
        process.kill()
        process.wait()
        raise
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()


def history_commits(repo: Path, tip: str, base: str | None = None) -> list[str]:
    if repository_is_shallow(repo):
        raise RuntimeError("History privacy scans require a complete, non-shallow repository")
    tip_oid = resolve_commit(repo, tip)
    arguments = ["rev-list", "--reverse", "--topo-order", tip_oid]
    if base is not None:
        base_oid = resolve_commit(repo, base)
        arguments.append(f"^{base_oid}")
    output = run_git(repo, *arguments)
    return [validated_oid(line) for line in output.splitlines() if line]


def all_history_commits(repo: Path) -> list[str]:
    if repository_is_shallow(repo):
        raise RuntimeError("History privacy scans require a complete, non-shallow repository")
    output = run_git(repo, "rev-list", "--reverse", "--topo-order", "--all")
    return [validated_oid(line) for line in output.splitlines() if line]


def reference_records(repo: Path) -> list[tuple[str, str, bytes]]:
    output = run_git(
        repo,
        "for-each-ref",
        "--format=%(objectname)%09%(objecttype)%09%(refname)",
    )
    records: list[tuple[str, str, bytes]] = []
    for line in output.splitlines():
        try:
            raw_oid, raw_type, raw_name = line.split(b"\t", 2)
            object_type = raw_type.decode("ascii", errors="strict")
        except (ValueError, UnicodeDecodeError) as exc:
            raise RuntimeError("Git returned a malformed reference") from exc
        records.append((validated_oid(raw_oid), object_type, raw_name))
    return records


def object_type(repo: Path, object_id: str) -> str:
    try:
        value = run_git(repo, "cat-file", "-t", object_id).strip().decode("ascii", errors="strict")
    except UnicodeDecodeError as exc:
        raise RuntimeError("Git returned an invalid object type") from exc
    if value not in {"blob", "commit", "tag", "tree"}:
        raise RuntimeError("Git returned an invalid object type")
    return value


def annotated_tag_target(repo: Path, object_id: str) -> str:
    data = run_git(repo, "cat-file", "tag", object_id)
    target_line = next(
        (line.removeprefix(b"object ") for line in data.splitlines() if line.startswith(b"object ")),
        None,
    )
    if target_line is None:
        raise RuntimeError("Annotated tag has no target object")
    return validated_oid(target_line)


def annotated_tag_chain(repo: Path, revision: str) -> tuple[list[str], str, str]:
    """Return every reachable tag object before the final peeled target."""

    object_id = resolve_object(repo, revision)
    tags: list[str] = []
    seen: set[str] = set()
    while object_type(repo, object_id) == "tag":
        if object_id in seen:
            raise RuntimeError("Annotated tag chain contains a cycle")
        seen.add(object_id)
        tags.append(object_id)
        object_id = annotated_tag_target(repo, object_id)
    return tags, object_id, object_type(repo, object_id)


def object_message(data: bytes) -> bytes:
    separator = data.find(b"\n\n")
    return b"" if separator == -1 else data[separator + 2 :]


def worktree_blobs(repo: Path) -> list[tuple[bytes, bytes]]:
    paths = run_git(repo, "ls-files", "-z").split(b"\x00")
    blobs: list[tuple[bytes, bytes]] = []
    for raw_path in paths:
        if not raw_path:
            continue
        path = os.fsdecode(raw_path)
        candidate = repo / PurePosixPath(path)
        if candidate.is_symlink():
            blobs.append((raw_path, os.fsencode(candidate.readlink())))
        elif candidate.is_file():
            blobs.append((raw_path, candidate.read_bytes()))
    return blobs


def line_number(data: bytes, offset: int) -> int:
    return data.count(b"\n", 0, offset) + 1


def safe_email(path: str, value: bytes) -> bool:
    if path.startswith("licenses/"):
        return True
    normalized = value.decode("ascii").lower()
    return normalized in SAFE_EMAILS or normalized.endswith(SAFE_EMAIL_SUFFIXES)


def png_metadata_findings(path: str, data: bytes) -> list[Finding]:
    if not data.startswith(b"\x89PNG\r\n\x1a\n"):
        return []
    findings: list[Finding] = []
    offset = 8
    while offset + 12 <= len(data):
        length = int.from_bytes(data[offset : offset + 4], "big")
        chunk_type = data[offset + 4 : offset + 8]
        chunk_end = offset + 12 + length
        if chunk_end > len(data):
            break
        if chunk_type in PNG_METADATA_CHUNKS:
            findings.append(Finding(path, f"embedded-png-{chunk_type.decode('ascii')}-metadata"))
        offset = chunk_end
        if chunk_type == b"IEND":
            break
    return findings


def binary_metadata_findings(path: str, data: bytes) -> list[Finding]:
    findings = png_metadata_findings(path, data)
    lower_path = path.lower()
    if lower_path.endswith((".jpg", ".jpeg")):
        if b"Exif\x00\x00" in data:
            findings.append(Finding(path, "embedded-jpeg-exif-metadata"))
        if b"\xff\xfe" in data:
            findings.append(Finding(path, "embedded-jpeg-comment"))
    if lower_path.endswith(".pdf") and (
        b"/Author" in data or b"<dc:creator" in data or b"<pdf:Author" in data
    ):
        findings.append(Finding(path, "embedded-pdf-author-metadata"))
    return findings


def _repeated_nibble(value: int) -> bool:
    return (value >> 4) == (value & 0x0F)


def mac_is_synthetic(raw: bytes) -> bool:
    """True for a hand-written fixture address, False for a real adapter.

    Fixtures are typed by a human, so their bytes come from the sixteen values
    whose nibbles repeat (00, 11, ... FF) — AA:BB:CC:DD:EE:FF, and the A4:C1:38
    vendor prefix with a 00:11:22 tail. A real NIC portion has no such bias, so
    the chance a live address is mistaken for a fixture is about 1 in 4096.
    A known-real address that must never appear belongs in the site-local
    blocklist, which does not depend on this heuristic at all.
    """
    try:
        octets = [int(part, 16) for part in raw.decode("ascii").split(":")]
    except ValueError:  # pragma: no cover - the regex already constrains this
        return False
    if len(set(octets)) == 1:
        return True  # 00:00:00:00:00:00 and friends
    if all(_repeated_nibble(value) for value in octets[3:]):
        return True  # synthetic device tail behind any vendor prefix
    return sum(_repeated_nibble(value) for value in octets) >= 4


def ip_is_publicly_routable(raw: bytes, preceding: bytes = b"") -> bool:
    """True only for a globally routable address.

    RFC1918 space is deliberately NOT a finding: 192.168.x.x is shared by
    hundreds of millions of networks, says nothing about a person, and is
    ordinary in device documentation. A public address is different — it places
    someone with an ISP and a rough location. IPv4Address.is_global already
    excludes loopback, link-local and the three RFC5737 documentation ranges.
    """
    text = raw.decode("ascii")
    if text in WELL_KNOWN_PUBLIC_IPS:
        return False
    if any(part != "0" and part.startswith("0") for part in text.split(".")):
        return False  # zero-padded: an identifier, not an address
    if b"version" in preceding.lower():
        return False  # a four-part version string, e.g. an assembly version
    try:
        address = ipaddress.IPv4Address(text)
    except ValueError:
        return False
    return address.is_global and not address.is_multicast


def device_path_is_generic(token: bytes) -> bool:
    """True for a port pattern, False for one concrete piece of hardware."""
    if any(character in token for character in b"*?[]"):
        return True  # a glob over whatever is plugged in
    text = token.decode("ascii", errors="replace")
    if re.fullmatch(r"[A-Za-z_]+", text):
        return True  # a bare driver family with no unit suffix
    return bool(re.search(r"[Xx]{3,}$", text))  # documentation placeholder


def credential_value_is_fixture(value: bytes) -> bool:
    text = value.decode("utf-8", errors="replace")
    if text.strip().lower() in CREDENTIAL_PLACEHOLDERS:
        return True
    if text.startswith("<") and text.endswith(">"):
        return True
    if re.fullmatch(r"[Xx.*\-_]+", text):
        return True
    if "${" in text or "{{" in text or text.startswith("$"):
        return True  # an interpolation, not a value
    digest = hashlib.sha256(text.encode("utf-8")).hexdigest()
    return digest in ALLOWED_CREDENTIAL_DIGESTS


def operational_findings(path: str, data: bytes) -> list[Finding]:
    findings: list[Finding] = []
    for match in MAC_CANDIDATE.finditer(data):
        if not mac_is_synthetic(match.group(0)):
            findings.append(
                Finding(path, "hardware-mac-address", line_number(data, match.start()))
            )
    for match in IPV4_CANDIDATE.finditer(data):
        if ip_is_publicly_routable(match.group(0), data[max(0, match.start() - 24) : match.start()]):
            findings.append(
                Finding(path, "public-ip-address", line_number(data, match.start()))
            )
    for match in DEVICE_PATH.finditer(data):
        if not device_path_is_generic(match.group(1)):
            findings.append(
                Finding(path, "local-device-path", line_number(data, match.start()))
            )
    for match in CREDENTIAL_LITERAL.finditer(data):
        if not credential_value_is_fixture(match.group(2)):
            # The value is the secret; report only that one is present.
            findings.append(
                Finding(path, "credential-literal", line_number(data, match.start()))
            )
    for match in PERSONAL_PROSE.finditer(data):
        findings.append(
            Finding(path, "personal-prose-attribution", line_number(data, match.start()))
        )
    return findings


def replay_source_path_allowed(path: PurePosixPath) -> bool:
    if path in REPLAY_EXACT_ALLOWED:
        return True
    return (
        len(path.parts) == 5
        and path.parts[:4] in {
            ("tools", "v1replay", "Sources", "v1replay"),
            ("tools", "v1replay", "Tests", "v1replayTests"),
        }
        and path.suffix == ".swift"
    )


def private_data_findings(path: str, data: bytes) -> list[Finding]:
    candidate = PurePosixPath(path)
    findings: list[Finding] = []

    if any(part.lower() in PRIVATE_DATA_PATH_PARTS for part in candidate.parts):
        findings.append(Finding(path, "tracked-private-data-path"))

    if (
        candidate.suffix.lower() in PRIVATE_BINARY_SUFFIXES
        and candidate not in PUBLIC_BINARY_MEDIA_ALLOWED
    ):
        findings.append(Finding(path, "unreviewed-binary-media"))

    if candidate.is_relative_to(REPLAY_ROOT) and not replay_source_path_allowed(candidate):
        findings.append(Finding(path, "replay-source-only-boundary"))

    stripped = data.lstrip()
    if stripped.startswith((b"{", b"[")):
        field_count = sum(marker in data for marker in REPLAY_CAPTURE_FIELDS)
        if field_count >= 4:
            findings.append(Finding(path, "replay-capture-content"))

    return findings


def scan_blob(path: str, data: bytes, local_terms: list[bytes] | None = None) -> list[Finding]:
    findings = private_data_findings(path, data)
    findings.extend(binary_metadata_findings(path, data))
    findings.extend(local_term_findings(path, data, local_terms or []))
    findings.extend(operational_findings(path, data))
    for match in EMAIL.finditer(data):
        if not safe_email(path, match.group(1)):
            findings.append(Finding(path, "non-public-email", line_number(data, match.start())))
    for rule, pattern in SECRET_PATTERNS:
        for match in pattern.finditer(data):
            findings.append(Finding(path, rule, line_number(data, match.start())))
    for pattern in (UNIX_HOME, WINDOWS_HOME):
        for match in pattern.finditer(data):
            home_name = match.group(1).decode("utf-8", errors="replace").lower()
            if home_name not in SAFE_HOME_NAMES:
                findings.append(Finding(path, "personal-home-path", line_number(data, match.start())))
    return findings


def display_path(raw_path: bytes) -> str:
    """Render a Git path without allowing control characters to forge log lines."""
    decoded = raw_path.decode("utf-8", errors="backslashreplace")
    return decoded.encode("unicode_escape").decode("ascii")


def scan_path_and_blob(
    raw_path: bytes,
    data: bytes,
    local_terms: list[bytes],
) -> list[Finding]:
    real_path = display_path(raw_path)
    path_policy_findings = private_data_findings(real_path, b"")
    path_findings = scan_blob("<tracked-path>", raw_path, local_terms)
    content_findings = scan_blob(real_path, data, local_terms)
    path_rules = {finding.rule for finding in path_policy_findings}
    content_findings = [
        finding
        for finding in content_findings
        if not (finding.line is None and finding.rule in path_rules)
    ]
    combined = path_policy_findings + path_findings + content_findings
    if path_policy_findings or path_findings:
        # A path can itself contain private data. Do not repeat that value while
        # reporting a separate content finding from the same file.
        combined = [
            Finding("<tracked-file-path-redacted>", finding.rule, finding.line, finding.redacted)
            for finding in combined
        ]
    return combined


def scan_messages(
    repo: Path,
    object_ids: Iterable[str],
    expected_type: str,
    label: str,
    local_terms: list[bytes],
) -> list[Finding]:
    findings: list[Finding] = []
    for object_id, actual_type, data in iter_git_objects(repo, object_ids):
        if actual_type != expected_type:
            raise RuntimeError("Git returned an unexpected message object")
        findings.extend(scan_blob(f"<{label}:{object_id[:12]}>", object_message(data), local_terms))
    return findings


def scan_tag_objects(
    repo: Path,
    object_ids: Iterable[str],
    local_terms: list[bytes],
) -> list[Finding]:
    findings: list[Finding] = []
    for object_id, actual_type, data in iter_git_objects(repo, object_ids):
        if actual_type != "tag":
            raise RuntimeError("Git returned an unexpected annotated-tag object")
        findings.extend(
            scan_blob(f"<annotated-tag:{object_id[:12]}>", data, local_terms)
        )
    return findings


def scan_repository(
    repo: Path,
    *,
    revision: str | None = None,
    index: bool = False,
    history_tip: str | None = None,
    history_base: str | None = None,
    all_history: bool = False,
    ref_names: list[str] | None = None,
    local_terms: list[bytes] | None = None,
) -> list[Finding]:
    selected_modes = sum((revision is not None, index, history_tip is not None, all_history))
    if selected_modes > 1:
        raise ValueError("privacy scan source modes are mutually exclusive")
    if history_base is not None and history_tip is None:
        raise ValueError("history-base requires history-tip")
    if local_terms is None:
        local_terms = load_local_terms()

    findings: list[Finding] = []
    commit_ids: list[str] = []
    annotated_tag_ids: set[str] = set()
    paths_by_blob: dict[str, set[bytes]] = defaultdict(set)
    revision_treeish: str | None = None

    if all_history:
        commit_ids = all_history_commits(repo)
        for object_id, ref_type, raw_name in reference_records(repo):
            findings.extend(scan_blob("<reference-name>", raw_name, local_terms))
            if ref_type == "tag":
                tags, _target, _target_type = annotated_tag_chain(repo, object_id)
                annotated_tag_ids.update(tags)
    elif history_tip is not None:
        tags, tip_object, tip_type = annotated_tag_chain(repo, history_tip)
        annotated_tag_ids.update(tags)
        if tip_type != "commit":
            raise RuntimeError("History tip does not peel to a commit")
        commit_ids = history_commits(repo, tip_object, history_base)
    elif revision is not None:
        tags, revision_treeish, _revision_type = annotated_tag_chain(repo, revision)
        annotated_tag_ids.update(tags)

    for ref_name in ref_names or []:
        findings.extend(scan_blob("<reference-name>", os.fsencode(ref_name), local_terms))

    if commit_ids:
        for commit_id in commit_ids:
            for blob_id, raw_path in tree_entries(repo, commit_id):
                paths_by_blob[blob_id].add(raw_path)
    elif revision is not None or index:
        treeish = (
            validated_oid(run_git(repo, "write-tree"))
            if index
            else revision_treeish
        )
        if treeish is None:
            raise RuntimeError("Revision did not resolve to a treeish object")
        for blob_id, raw_path in tree_entries(repo, treeish):
            paths_by_blob[blob_id].add(raw_path)

    if paths_by_blob:
        for blob_id, actual_type, data in iter_git_objects(repo, paths_by_blob):
            if actual_type != "blob":
                raise RuntimeError("Git tree referenced a non-blob object")
            for raw_path in sorted(paths_by_blob[blob_id]):
                findings.extend(scan_path_and_blob(raw_path, data, local_terms))
    elif not (all_history or history_tip is not None or revision is not None or index):
        for raw_path, data in worktree_blobs(repo):
            findings.extend(scan_path_and_blob(raw_path, data, local_terms))

    if commit_ids:
        findings.extend(scan_messages(repo, commit_ids, "commit", "commit-message", local_terms))
    if annotated_tag_ids:
        findings.extend(scan_tag_objects(repo, sorted(annotated_tag_ids), local_terms))
    return findings


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path.cwd())
    parser.add_argument(
        "--digest",
        metavar="VALUE",
        help="print the SHA-256 of VALUE for ALLOWED_CREDENTIAL_DIGESTS, then exit",
    )
    source = parser.add_mutually_exclusive_group()
    source.add_argument("--revision")
    source.add_argument("--index", action="store_true")
    source.add_argument("--history-tip")
    source.add_argument("--all-history", action="store_true")
    source.add_argument("--message-file", type=Path)
    parser.add_argument("--history-base")
    parser.add_argument("--ref-name", action="append", default=[])
    args = parser.parse_args()

    if args.digest is not None:
        print(hashlib.sha256(args.digest.encode("utf-8")).hexdigest())
        return 0

    try:
        if args.message_file is not None:
            if args.history_base is not None:
                raise ValueError("history-base is not valid with message-file")
            message = args.message_file.read_bytes()
            findings = scan_blob("<commit-message>", message, load_local_terms())
        else:
            findings = scan_repository(
                args.repo.resolve(),
                revision=args.revision,
                index=args.index,
                history_tip=args.history_tip,
                history_base=args.history_base,
                all_history=args.all_history,
                ref_names=args.ref_name,
            )
    except (OSError, RuntimeError, ValueError):
        # Never echo a private path, ref, revision, message, or Git diagnostic.
        exc = "privacy scan input could not be inspected safely"
        print(f"[public-snapshot-privacy] ERROR: {exc}", file=sys.stderr)
        return 2
    if findings:
        for finding in findings:
            print(f"[public-snapshot-privacy] ERROR: {finding.render()}", file=sys.stderr)
        return 1
    if args.all_history or args.history_tip is not None:
        print("[public-snapshot-privacy] publication history is privacy-safe")
    elif args.message_file is not None:
        print("[public-snapshot-privacy] commit message is privacy-safe")
    else:
        print("[public-snapshot-privacy] tracked snapshot is privacy-safe")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

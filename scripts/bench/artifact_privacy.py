#!/usr/bin/env python3
"""Redact private operational values before bench text leaves a process."""

from __future__ import annotations

import ipaddress
import hashlib
import hmac
import os
import re
import stat
from pathlib import Path
from typing import Any


REDACTED_MAC = "<redacted-mac>"
REDACTED_DEVICE_PATH = "/dev/<redacted-device>"
REDACTED_HOME_USER = "<redacted-user>"
REDACTED_HOST_PATH = "<redacted-host-path>"
REDACTED_NETWORK = "<redacted-network>"
REDACTED_PROFILE = "<redacted-profile>"
REDACTED_BODY = "<redacted-body>"
REDACTED_CREDENTIAL = "<redacted-credential>"
REDACTED_EMAIL = "<redacted-email>"
REDACTED_NAME = "<redacted-name>"
REDACTED_PRIVATE_TERM = "<redacted-private-term>"
REDACTED_PUBLIC_IP = "<redacted-public-ip>"


LOCAL_TERMS_ENV = "V1SIMPLE_PRIVACY_TERMS"
LOCAL_TERMS_DEFAULT = Path.home() / ".config" / "v1simple" / "privacy_terms.txt"
LOCAL_IDENTITY_KEY_ENV = "V1SIMPLE_PRIVACY_IDENTITY_KEY_FILE"


_REDACTIONS: tuple[tuple[str, str], ...] = (
    (
        r"(?i)(?<![0-9a-f:])(?:[0-9a-f]{2}:){5}[0-9a-f]{2}(?![0-9a-f:])",
        REDACTED_MAC,
    ),
    (r"/dev/(?:cu|tty)\.[A-Za-z0-9._*?\[\]-]+", REDACTED_DEVICE_PATH),
    (
        r"(/(?:Users|home)/)[^/\\\x00\r\n\t \"']+(?:/[^\\\x00\r\n\t \"']*)?",
        rf"\1{REDACTED_HOME_USER}/<redacted-path>",
    ),
    (
        r"/(?:private/var/folders|var/folders|private/tmp|tmp)/[^\\\x00\r\n\t \"']+",
        REDACTED_HOST_PATH,
    ),
    (r"(?i)(\bssid\s*=\s*')[^'\r\n]*'", rf"\1{REDACTED_NETWORK}'"),
    (r'(?i)(\bssid\s*=\s*")[^"\r\n]*"', rf'\1{REDACTED_NETWORK}"'),
    (
        r"(?i)(\b(?:profile|slot0profile)\s*=\s*')[^'\r\n]*'",
        rf"\1{REDACTED_PROFILE}'",
    ),
    (
        r'(?i)(\b(?:profile|slot0profile)\s*=\s*")[^"\r\n]*"',
        rf'\1{REDACTED_PROFILE}"',
    ),
    (
        r"(?i)(SSID mismatch \(want=')[^'\r\n]*(' got=')[^'\r\n]*(')",
        rf"\1{REDACTED_NETWORK}\2{REDACTED_NETWORK}\3",
    ),
    (
        r"(?m)^(\[Settings\] HEAL: recovered WiFi SSID from wifi_secret \(')[^'\r\n]*('\))",
        rf"\1{REDACTED_NETWORK}\2",
    ),
    (
        r"(?m)^(\[WiFiClient\] Connecting to: ).*$",
        rf"\1{REDACTED_NETWORK}",
    ),
    (
        r"(?m)^(\[SetupMode\] STA connect queued for ')[^'\r\n]*(')$",
        rf"\1{REDACTED_NETWORK}\2",
    ),
    (
        r"(?m)^(\[WiFiClient\] Maintenance STA auto-connect trying slot \d+ SSID ')[^'\r\n]*(')$",
        rf"\1{REDACTED_NETWORK}\2",
    ),
    (
        r"(?m)^(\[V1Settings\] (?:Save request body|Push request): ).*$",
        rf"\1{REDACTED_BODY}",
    ),
    (
        r"(?m)^(\[V1Settings\] Pushing profile ')[^'\r\n]*(':.*)$",
        rf"\1{REDACTED_PROFILE}\2",
    ),
    (
        r"(?m)^(\[V1Profiles\]\s+- ).*$",
        rf"\1{REDACTED_PROFILE}",
    ),
    (
        r"(?m)^(\[V1Profiles\] (?:Loaded profile|Deleted profile): ).*$",
        rf"\1{REDACTED_PROFILE}",
    ),
    (
        r"(?m)^(\[V1Profiles\] Saved profile: ).*?( \(\d+ bytes, CRC: [0-9A-Fa-f]+\))$",
        rf"\1{REDACTED_PROFILE}\2",
    ),
    (
        r"(?m)^(\[V1Profiles\] (?:Removing incomplete temp file|Migrated profile file): ).*$",
        rf"\1{REDACTED_PROFILE}",
    ),
    (
        r"(?m)^(Starting BLE scan for V1 \(proxy: [^,\r\n]+, name: ).*(\))$",
        rf"\1{REDACTED_NAME}\2",
    ),
    (
        r"(?m)^(\[BLE\] Proxy advertising name auto-set from V1: ).*$",
        rf"\1{REDACTED_NAME}",
    ),
)

_TEXT_REDACTIONS = tuple((re.compile(pattern), replacement) for pattern, replacement in _REDACTIONS)
_EMAIL = re.compile(
    r"(?<![A-Za-z0-9._%+-])[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}"
    r"(?![A-Za-z0-9.-])"
)
_IPV4 = re.compile(r"(?<![0-9.])(?:[0-9]{1,3}\.){3}[0-9]{1,3}(?![0-9.])")
_QUOTED_CREDENTIAL = re.compile(
    r"(?i)(\b(?:password|passwd|api[_-]?key|access[_-]?token|client[_-]?secret)\b"
    r"\s*[:=]\s*)(?P<quote>['\"])(?P<value>[^'\"\r\n]{4,})(?P=quote)"
)
_UNQUOTED_CREDENTIAL = re.compile(
    r"(?i)(\b(?:password|passwd|api[_-]?key|access[_-]?token|client[_-]?secret)\b"
    r"\s*[:=]\s*)(?!['\"])([^\s,;}\]\r\n]{4,})"
)
_CREDENTIAL_FIELD_SUFFIXES = {
    "access_token",
    "api_key",
    "auth_token",
    "authorization",
    "bearer_token",
    "client_secret",
    "credential",
    "credentials",
    "private_key",
    "refresh_token",
    "secret",
}
_UNTRUSTED_CREDENTIAL_FIELD_SUFFIXES = {
    "auth_header",
    "authorization_header",
    "cookie",
    "cookies",
    "set_cookie",
    "token",
}
_SECRET_PATTERNS = tuple(
    re.compile(pattern, re.IGNORECASE)
    for pattern in (
        r"gh[pousr]_[A-Za-z0-9_]{20,}",
        r"AKIA[0-9A-Z]{16}",
        r"AIza[0-9A-Za-z_-]{30,}",
        r"sk_live_[0-9A-Za-z]{16,}",
        r"-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----[\s\S]*?"
        r"-----END (?:RSA |EC |OPENSSH )?PRIVATE KEY-----",
        r"-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----",
    )
)
_HEX_DIGEST = re.compile(r"[0-9A-Fa-f]{64}")
_GIT_REVISION = re.compile(r"[0-9A-Fa-f]{7,64}")
_RUNTIME_IMAGE_ID = re.compile(r"[0-9A-Fa-f]{9}")
_CONTENT_ADDRESSED_SHEET = re.compile(
    r"investigation_sheets/[0-9A-Fa-f]{64}\.(?:jpe?g|png)"
)

_LOCAL_TERMS_CACHE_KEY: tuple[str, int, int] | None = None
_LOCAL_TERMS_CACHE: tuple[tuple[int, re.Pattern[str]], ...] = ()


def _local_term_patterns() -> tuple[tuple[int, re.Pattern[str]], ...]:
    """Load the private local blocklist without ever returning or logging it."""
    global _LOCAL_TERMS_CACHE_KEY, _LOCAL_TERMS_CACHE

    override = os.environ.get(LOCAL_TERMS_ENV)
    path = Path(override) if override else LOCAL_TERMS_DEFAULT
    try:
        stat_result = path.stat()
        cache_key = (str(path), stat_result.st_mtime_ns, stat_result.st_size)
        if cache_key == _LOCAL_TERMS_CACHE_KEY:
            return _LOCAL_TERMS_CACHE
        raw = path.read_text(encoding="utf-8")
    except OSError:
        return ()

    terms: dict[str, tuple[int, str]] = {}
    for line_number, entry in enumerate(raw.splitlines(), start=1):
        term = entry.strip()
        if not term or term.startswith("#"):
            continue
        terms.setdefault(term.casefold(), (line_number, term))
    patterns = tuple(
        (line_number, re.compile(re.escape(term), re.IGNORECASE))
        for line_number, term in sorted(terms.values(), key=lambda item: len(item[1]), reverse=True)
    )
    _LOCAL_TERMS_CACHE_KEY = cache_key
    _LOCAL_TERMS_CACHE = patterns
    return patterns


def _local_terms_path() -> Path:
    override = os.environ.get(LOCAL_TERMS_ENV)
    return Path(override) if override else LOCAL_TERMS_DEFAULT


def _identity_key_path() -> Path:
    override = os.environ.get(LOCAL_IDENTITY_KEY_ENV)
    return Path(override) if override else _local_terms_path().with_name("privacy_identity.key")


def _read_private_identity_key(path: Path) -> bytes:
    flags = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0)
    descriptor = os.open(path, flags)
    try:
        metadata = os.fstat(descriptor)
        if (
            not stat.S_ISREG(metadata.st_mode)
            or metadata.st_uid != os.geteuid()
            or stat.S_IMODE(metadata.st_mode) & 0o077
        ):
            raise RuntimeError("private identity key is not an owner-only regular file")
        key = os.read(descriptor, 4096)
    finally:
        os.close(descriptor)
    if len(key) != 32:
        raise RuntimeError("private identity key has an invalid length")
    return key


def _private_identity_key() -> bytes:
    """Load or atomically create the owner-only key for non-reversible aliases."""
    path = _identity_key_path()
    try:
        return _read_private_identity_key(path)
    except FileNotFoundError:
        path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        key = os.urandom(32)
        flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_NOFOLLOW", 0)
        try:
            descriptor = os.open(path, flags, 0o600)
        except FileExistsError:
            return _read_private_identity_key(path)
        try:
            written = os.write(descriptor, key)
            if written != len(key):
                raise RuntimeError("private identity key could not be written completely")
            os.fsync(descriptor)
        finally:
            os.close(descriptor)
        return _read_private_identity_key(path)


def _redact_public_ipv4(match: re.Match[str]) -> str:
    try:
        address = ipaddress.IPv4Address(match.group(0))
    except ipaddress.AddressValueError:
        return match.group(0)
    if address.is_global and not address.is_multicast:
        return REDACTED_PUBLIC_IP
    return match.group(0)


def _is_opaque_metadata(field: str | None, parent_field: str | None, text: str) -> bool:
    """Keep validated machine identities intact while narrative text is scrubbed."""
    normalized = (field or "").lower()
    normalized_parent = (parent_field or "").lower()
    if _HEX_DIGEST.fullmatch(text) and (
        normalized in {
            "capture_id",
            "grade_id",
            "sha256",
            "source_sha256",
        }
        or normalized.endswith(("_sha256", "_fingerprint", "_digest"))
        or normalized_parent == "hashes"
        or normalized_parent.endswith("_hashes")
    ):
        return True
    if normalized in {
        "base_revision",
        "candidate_revision",
        "inspected_revision",
        "recorded_revision",
        "revision",
    } or normalized.endswith(("_revision", "_revisions", "_sha")):
        return _GIT_REVISION.fullmatch(text) is not None
    if normalized in {
        "expected_runtime_image_id",
        "runtime_image_id",
        "runtimeimageid",
    }:
        return _RUNTIME_IMAGE_ID.fullmatch(text) is not None
    if normalized == "sheet_path":
        return _CONTENT_ADDRESSED_SHEET.fullmatch(text) is not None
    return False


def redact_artifact_text(text: str) -> str:
    """Return a persistence-safe copy without changing unrelated evidence."""
    for _line_number, pattern in _local_term_patterns():
        text = pattern.sub(REDACTED_PRIVATE_TERM, text)
    for pattern, replacement in _TEXT_REDACTIONS:
        text = pattern.sub(replacement, text)
    text = _EMAIL.sub(REDACTED_EMAIL, text)
    text = _QUOTED_CREDENTIAL.sub(
        lambda match: (
            f"{match.group(1)}{match.group('quote')}"
            f"{REDACTED_CREDENTIAL}{match.group('quote')}"
        ),
        text,
    )
    text = _UNQUOTED_CREDENTIAL.sub(
        lambda match: f"{match.group(1)}{REDACTED_CREDENTIAL}", text
    )
    for pattern in _SECRET_PATTERNS:
        text = pattern.sub(REDACTED_CREDENTIAL, text)
    text = _IPV4.sub(_redact_public_ipv4, text)
    return text


def privacy_safe_identifier(value: Any, *, namespace: str) -> str:
    """Return a non-PII identity while keeping distinct local blocklist entries distinct.

    A keyed digest keeps the alias stable when the private blocklist changes
    without publishing a reversible hash of the private value.
    """
    text = str(value or "unknown")
    safe_namespace = re.sub(r"[^a-z0-9]+", "-", namespace.casefold()).strip("-")
    safe_namespace = safe_namespace or "identifier"
    if re.fullmatch(rf"private-{re.escape(safe_namespace)}-[0-9a-f]{{64}}", text):
        return text
    contains_local_term = any(
        pattern.search(text) for _line_number, pattern in _local_term_patterns()
    )
    redacted = redact_artifact_text(text)
    unsafe_component = (
        text in {".", ".."}
        or re.fullmatch(r"[A-Za-z0-9._-]+", text) is None
    )
    if contains_local_term or redacted != text or unsafe_component:
        try:
            identity_key = _private_identity_key()
        except (OSError, RuntimeError):
            raise RuntimeError("private identity alias key is unavailable") from None
        digest = hmac.new(
            identity_key,
            text.casefold().encode("utf-8", errors="surrogatepass"),
            hashlib.sha256,
        ).hexdigest()
        return f"private-{safe_namespace}-{digest}"
    return text


def redact_artifact_bytes(data: bytes) -> bytes:
    """Redact managed process output while preserving undecodable input bytes."""
    text = data.decode("utf-8", errors="surrogateescape")
    return redact_artifact_text(text).encode("utf-8", errors="surrogateescape")


def _is_credential_field(field: str) -> bool:
    snake = re.sub(r"([a-z0-9])([A-Z])", r"\1_\2", field)
    normalized = re.sub(r"[^a-z0-9]+", "_", snake.casefold()).strip("_")
    components = normalized.split("_") if normalized else []
    if any(component in {"password", "passwd", "passphrase"} for component in components):
        return True
    return any(
        normalized == suffix or normalized.endswith(f"_{suffix}")
        for suffix in _CREDENTIAL_FIELD_SUFFIXES
    )


def _is_untrusted_credential_field(field: str) -> bool:
    snake = re.sub(r"([a-z0-9])([A-Z])", r"\1_\2", field)
    normalized = re.sub(r"[^a-z0-9]+", "_", snake.casefold()).strip("_")
    return any(
        normalized == suffix or normalized.endswith(f"_{suffix}")
        for suffix in _UNTRUSTED_CREDENTIAL_FIELD_SUFFIXES
    )


def sanitize_artifact_value(
    value: Any,
    *,
    run_dir: Path,
    sanitize_mapping_keys: bool = False,
    trusted_mapping_keys: frozenset[str] = frozenset(),
    _field: str | None = None,
    _parent_field: str | None = None,
) -> Any:
    """Recursively make a machine-document value safe to persist.

    Paths owned by this run become run-relative. Other text still passes through
    the narrow redactor so exception messages cannot bypass the log boundary.
    Callers accepting dynamic mapping keys must opt into key sanitation; fixed
    machine-document schema keys remain stable when explicitly trusted at that
    mapping level.
    """
    if _field is not None and (
        _is_credential_field(_field)
        or (sanitize_mapping_keys and _is_untrusted_credential_field(_field))
    ):
        return REDACTED_CREDENTIAL
    if isinstance(value, dict):
        if not sanitize_mapping_keys:
            return {
                key: sanitize_artifact_value(
                    item,
                    run_dir=run_dir,
                    sanitize_mapping_keys=False,
                    _field=str(key),
                    _parent_field=_field,
                )
                for key, item in value.items()
            }
        safe_mapping: dict[str, Any] = {}
        for key, item in value.items():
            source_key = str(key)
            safe_key = (
                source_key
                if source_key in trusted_mapping_keys
                else sanitize_artifact_value(source_key, run_dir=run_dir)
            )
            candidate = safe_key
            suffix = 2
            while candidate in safe_mapping:
                candidate = f"{safe_key}#{suffix}"
                suffix += 1
            safe_mapping[candidate] = sanitize_artifact_value(
                item,
                run_dir=run_dir,
                sanitize_mapping_keys=True,
                trusted_mapping_keys=frozenset(),
                _field=source_key,
                _parent_field=_field,
            )
        return safe_mapping
    if isinstance(value, list):
        return [
            sanitize_artifact_value(
                item,
                run_dir=run_dir,
                sanitize_mapping_keys=sanitize_mapping_keys,
                trusted_mapping_keys=frozenset(),
                _field=_field,
                _parent_field=_parent_field,
            )
            for item in value
        ]
    if isinstance(value, tuple):
        return [
            sanitize_artifact_value(
                item,
                run_dir=run_dir,
                sanitize_mapping_keys=sanitize_mapping_keys,
                trusted_mapping_keys=frozenset(),
                _field=_field,
                _parent_field=_parent_field,
            )
            for item in value
        ]
    if not isinstance(value, str):
        return value

    text = value
    run_prefixes = {str(run_dir), str(run_dir.resolve())}
    for prefix in sorted(run_prefixes, key=len, reverse=True):
        if text == prefix:
            text = "."
        text = text.replace(f"{prefix}/", "")
    if _is_opaque_metadata(_field, _parent_field, text):
        return text
    return redact_artifact_text(text)

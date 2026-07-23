#!/usr/bin/env python3
"""Resolve a local HIL board alias without exposing its USB identity.

The tracked inventory is a schema/template. A git-ignored local inventory may
add or replace complete board entries by alias. Resolution deliberately never
falls back to the first enumerated serial port: the configured USB serial must
match exactly.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from datetime import datetime, timezone
import hashlib
import ipaddress
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time
from typing import Callable, Iterable, Mapping, Sequence
from urllib.parse import urlsplit, urlunsplit


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_TEMPLATE = ROOT / "test" / "device" / "board_inventory.json"
DEFAULT_LOCAL_INVENTORY = ROOT / "test" / "device" / "board_inventory.local.json"
DEFAULT_LOCAL_INVENTORY_SIGNATURE = Path(f"{DEFAULT_LOCAL_INVENTORY}.sig")
DEFAULT_BOARD_TRUST_ROOT = (
    ROOT / "tools" / "bug_squash_hil_board_inventory_allowed_signers_v1"
)

SCHEMA_VERSION = 1
ATTESTATION_SCHEMA_VERSION = 1
INVENTORY_AUTHENTICATION_SCHEMA_VERSION = 1
INVENTORY_AUTHENTICATION_ALGORITHM = "ssh-ed25519-sshsig-v1"
INVENTORY_SIGNATURE_NAMESPACE = "v1simple-hil-board-inventory-v1"
INVENTORY_SIGNER_PRINCIPAL = "v1simple-board-inventory"
SSH_KEYGEN = Path("/usr/bin/ssh-keygen")
ALIAS_PATTERN = re.compile(r"^[a-z0-9][a-z0-9._-]{0,63}$")
CAPABILITY_PATTERN = re.compile(r"^[a-z0-9][a-z0-9._-]{0,63}$")
SHA256_PATTERN = re.compile(r"^[0-9a-f]{64}$")
FIRMWARE_IP_PREFIX = "[WiFiClient] Connected! IP: "
PLACEHOLDER_MARKERS = ("REPLACE_", "CHANGEME", "YOUR_")
UTC_TIMESTAMP_PATTERN = re.compile(
    r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d+)?Z$"
)


class ResolverError(Exception):
    """Expected configuration or runtime resolution failure."""

    def __init__(self, code: str, message: str) -> None:
        super().__init__(message)
        self.code = code
        self.message = message


class DuplicateJsonKeyError(ValueError):
    """Raised when a resolver input repeats a JSON object key."""


def _reject_duplicate_json_keys(
    pairs: list[tuple[str, object]],
) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise DuplicateJsonKeyError(key)
        result[key] = value
    return result


@dataclass(frozen=True)
class Board:
    alias: str
    capabilities: tuple[str, ...]
    usb_serial: str | None = None
    lan_base_url: str | None = None


@dataclass(frozen=True)
class Inventory:
    boards: Mapping[str, Board]
    authentication: "InventoryAuthentication | None" = None


@dataclass(frozen=True)
class InventoryAuthentication:
    inventory_json: str
    signature: str
    inventory_sha256: str
    trust_root_sha256: str

    def as_binding(self) -> dict[str, object]:
        return {
            "schema_version": INVENTORY_AUTHENTICATION_SCHEMA_VERSION,
            "algorithm": INVENTORY_AUTHENTICATION_ALGORITHM,
            "namespace": INVENTORY_SIGNATURE_NAMESPACE,
            "signer_principal": INVENTORY_SIGNER_PRINCIPAL,
            "trust_root_sha256": self.trust_root_sha256,
            "inventory_sha256": self.inventory_sha256,
            "inventory_json": self.inventory_json,
            "signature": self.signature,
        }


def _is_string(value: object) -> bool:
    return isinstance(value, str) and bool(value.strip())


def _read_text(path: Path, label: str) -> str:
    if path.is_symlink():
        raise ResolverError("inventory_unreadable", f"{label} must not be a symlink")
    try:
        return path.read_text(encoding="utf-8")
    except FileNotFoundError as exc:
        raise ResolverError("inventory_missing", f"{label} file does not exist") from exc
    except (OSError, UnicodeError) as exc:
        raise ResolverError("inventory_unreadable", f"{label} file could not be read") from exc


def _parse_json_text(content: str, label: str) -> object:
    try:
        return json.loads(content, object_pairs_hook=_reject_duplicate_json_keys)
    except (json.JSONDecodeError, DuplicateJsonKeyError) as exc:
        raise ResolverError("inventory_invalid_json", f"{label} is not valid JSON") from exc


def _read_json(path: Path, label: str) -> object:
    return _parse_json_text(_read_text(path, label), label)


def _read_trust_root(path: Path) -> tuple[str, str]:
    if path.is_symlink():
        raise ResolverError(
            "inventory_trust_root_invalid",
            "board inventory trust root must not be a symlink",
        )
    try:
        content = path.read_text(encoding="ascii")
    except (OSError, UnicodeError) as exc:
        raise ResolverError(
            "inventory_trust_root_missing",
            "board inventory trust root is unavailable",
        ) from exc
    lines = [line for line in content.splitlines() if line]
    expected_prefix = (
        f'{INVENTORY_SIGNER_PRINCIPAL} '
        f'namespaces="{INVENTORY_SIGNATURE_NAMESPACE}" ssh-ed25519 '
    )
    if len(lines) != 1 or not lines[0].startswith(expected_prefix):
        raise ResolverError(
            "inventory_trust_root_invalid",
            "board inventory trust root is invalid",
        )
    return content, hashlib.sha256(content.encode("ascii")).hexdigest()


def _test_trust_root_override() -> Path | None:
    if os.environ.get("V1SIMPLE_HIL_TEST_HOOKS") != "1":
        return None
    raw = os.environ.get("V1SIMPLE_HIL_TEST_BOARD_TRUST_ROOT")
    return Path(raw) if raw else None


def _verify_inventory_signature(
    inventory_json: str,
    signature: str,
    *,
    trust_root_path: Path,
) -> str:
    _content, trust_root_sha256 = _read_trust_root(trust_root_path)
    try:
        with tempfile.NamedTemporaryFile(suffix=".sshsig") as signature_file:
            signature_file.write(signature.encode("ascii"))
            signature_file.flush()
            completed = subprocess.run(
                [
                    str(SSH_KEYGEN),
                    "-Y",
                    "verify",
                    "-f",
                    str(trust_root_path),
                    "-I",
                    INVENTORY_SIGNER_PRINCIPAL,
                    "-n",
                    INVENTORY_SIGNATURE_NAMESPACE,
                    "-s",
                    signature_file.name,
                ],
                input=inventory_json.encode("utf-8"),
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
                timeout=15,
            )
    except (OSError, UnicodeError, subprocess.TimeoutExpired) as exc:
        raise ResolverError(
            "inventory_authentication_unavailable",
            "board inventory signature could not be verified",
        ) from exc
    if completed.returncode != 0:
        raise ResolverError(
            "inventory_authentication_failed",
            "board inventory does not match the pinned signing root",
        )
    if b"Good " not in completed.stdout:
        raise ResolverError(
            "inventory_authentication_failed",
            "board inventory signature verification was inconclusive",
        )
    return trust_root_sha256


def authenticate_local_inventory(
    local_path: Path,
    *,
    signature_path: Path | None = None,
    trust_root_path: Path | None = None,
) -> tuple[object, InventoryAuthentication]:
    inventory_json = _read_text(local_path, "local inventory")
    if signature_path is None:
        signature_path = Path(f"{local_path}.sig")
    try:
        signature = _read_text(signature_path, "local inventory signature")
    except ResolverError as exc:
        if exc.code == "inventory_missing":
            raise ResolverError(
                "inventory_signature_missing",
                "local inventory signature file does not exist",
            ) from exc
        raise
    if (
        len(signature) > 16_384
        or not signature.startswith("-----BEGIN SSH SIGNATURE-----\n")
        or not signature.rstrip().endswith("-----END SSH SIGNATURE-----")
    ):
        raise ResolverError(
            "inventory_signature_invalid",
            "local inventory signature is invalid",
        )
    selected_root = (
        trust_root_path
        or _test_trust_root_override()
        or DEFAULT_BOARD_TRUST_ROOT
    )
    trust_root_sha256 = _verify_inventory_signature(
        inventory_json,
        signature,
        trust_root_path=selected_root,
    )
    payload = _parse_json_text(inventory_json, "local inventory")
    return payload, InventoryAuthentication(
        inventory_json=inventory_json,
        signature=signature,
        inventory_sha256=hashlib.sha256(inventory_json.encode("utf-8")).hexdigest(),
        trust_root_sha256=trust_root_sha256,
    )


def authenticate_inventory_binding(
    payload: object,
    *,
    trust_root_path: Path | None = None,
) -> Inventory:
    if not isinstance(payload, dict) or set(payload) != {
        "schema_version",
        "algorithm",
        "namespace",
        "signer_principal",
        "trust_root_sha256",
        "inventory_sha256",
        "inventory_json",
        "signature",
    }:
        raise ResolverError(
            "inventory_authentication_invalid",
            "board inventory authentication binding is invalid",
        )
    if (
        payload.get("schema_version") != INVENTORY_AUTHENTICATION_SCHEMA_VERSION
        or payload.get("algorithm") != INVENTORY_AUTHENTICATION_ALGORITHM
        or payload.get("namespace") != INVENTORY_SIGNATURE_NAMESPACE
        or payload.get("signer_principal") != INVENTORY_SIGNER_PRINCIPAL
    ):
        raise ResolverError(
            "inventory_authentication_invalid",
            "board inventory authentication contract is invalid",
        )
    inventory_json = payload.get("inventory_json")
    signature = payload.get("signature")
    inventory_sha256 = payload.get("inventory_sha256")
    trust_root_sha256 = payload.get("trust_root_sha256")
    if (
        not isinstance(inventory_json, str)
        or not isinstance(signature, str)
        or not isinstance(inventory_sha256, str)
        or SHA256_PATTERN.fullmatch(inventory_sha256) is None
        or not isinstance(trust_root_sha256, str)
        or SHA256_PATTERN.fullmatch(trust_root_sha256) is None
    ):
        raise ResolverError(
            "inventory_authentication_invalid",
            "board inventory authentication values are invalid",
        )
    if hashlib.sha256(inventory_json.encode("utf-8")).hexdigest() != inventory_sha256:
        raise ResolverError(
            "inventory_authentication_failed",
            "board inventory content digest does not match its authentication binding",
        )
    selected_root = trust_root_path or DEFAULT_BOARD_TRUST_ROOT
    actual_root_sha256 = _verify_inventory_signature(
        inventory_json,
        signature,
        trust_root_path=selected_root,
    )
    if trust_root_sha256 != actual_root_sha256:
        raise ResolverError(
            "inventory_authentication_failed",
            "board inventory trust root identity does not match its authentication binding",
        )
    boards = _validate_document(
        _parse_json_text(inventory_json, "authenticated local inventory"),
        "authenticated local inventory",
    )
    return Inventory(
        boards=boards,
        authentication=InventoryAuthentication(
            inventory_json=inventory_json,
            signature=signature,
            inventory_sha256=inventory_sha256,
            trust_root_sha256=trust_root_sha256,
        ),
    )


def _validate_lan_base_url(value: str) -> str:
    if any(character.isspace() for character in value):
        raise ResolverError("invalid_lan_base_url", "lan_base_url must not contain whitespace")
    try:
        parsed = urlsplit(value)
        port = parsed.port
    except ValueError as exc:
        raise ResolverError("invalid_lan_base_url", "lan_base_url has an invalid port") from exc

    if parsed.scheme not in {"http", "https"} or not parsed.hostname:
        raise ResolverError(
            "invalid_lan_base_url",
            "lan_base_url must be an absolute http or https URL",
        )
    if parsed.username is not None or parsed.password is not None:
        raise ResolverError("invalid_lan_base_url", "lan_base_url must not contain credentials")
    if parsed.query or parsed.fragment:
        raise ResolverError(
            "invalid_lan_base_url",
            "lan_base_url must not contain a query or fragment",
        )

    host = parsed.hostname
    _validate_lan_host(host)
    if ":" in host and not host.startswith("["):
        host = f"[{host}]"
    netloc = host if port is None else f"{host}:{port}"
    path = parsed.path.rstrip("/")
    return urlunsplit((parsed.scheme, netloc, path, "", ""))


def _validate_lan_host(host: str) -> None:
    try:
        address = ipaddress.ip_address(host)
    except ValueError:
        if len(host) > 253 or re.fullmatch(r"[A-Za-z0-9.-]+", host) is None:
            raise ResolverError("invalid_lan_base_url", "lan_base_url has an invalid host")
        labels = host.split(".")
        if any(
            not label
            or len(label) > 63
            or label.startswith("-")
            or label.endswith("-")
            for label in labels
        ):
            raise ResolverError("invalid_lan_base_url", "lan_base_url has an invalid host")
        return

    if address.is_unspecified or address.is_loopback or address.is_multicast or address.is_reserved:
        raise ResolverError("invalid_lan_base_url", "lan_base_url has an unusable address")


def _validate_document(payload: object, label: str) -> dict[str, Board]:
    if not isinstance(payload, dict):
        raise ResolverError("invalid_inventory", f"{label} must be a JSON object")

    allowed_top_level = {"schema_version", "description", "boards"}
    unknown = set(payload) - allowed_top_level
    if unknown:
        raise ResolverError("invalid_inventory", f"{label} contains unsupported top-level fields")

    if type(payload.get("schema_version")) is not int or payload["schema_version"] != SCHEMA_VERSION:
        raise ResolverError(
            "unsupported_schema",
            f"{label} schema_version must be {SCHEMA_VERSION}",
        )
    if "description" in payload and not _is_string(payload["description"]):
        raise ResolverError("invalid_inventory", f"{label} description must be a non-empty string")

    raw_boards = payload.get("boards")
    if not isinstance(raw_boards, list):
        raise ResolverError("invalid_inventory", f"{label} boards must be an array")

    boards: dict[str, Board] = {}
    seen_usb_serials: set[str] = set()
    allowed_board_fields = {"alias", "capabilities", "usb_serial", "lan_base_url"}

    for index, raw_board in enumerate(raw_boards):
        board_label = f"{label} board #{index + 1}"
        if not isinstance(raw_board, dict):
            raise ResolverError("invalid_board", f"{board_label} must be an object")
        if set(raw_board) - allowed_board_fields:
            raise ResolverError("invalid_board", f"{board_label} contains unsupported fields")

        alias = raw_board.get("alias")
        if not _is_string(alias) or ALIAS_PATTERN.fullmatch(alias) is None:
            raise ResolverError(
                "invalid_alias",
                f"{board_label} alias must be a lowercase slug up to 64 characters",
            )
        if alias in boards:
            raise ResolverError("duplicate_alias", f"{label} contains a duplicate board alias")

        raw_capabilities = raw_board.get("capabilities")
        if not isinstance(raw_capabilities, list) or not raw_capabilities:
            raise ResolverError("invalid_capabilities", f"board '{alias}' needs capabilities")
        capabilities: list[str] = []
        for capability in raw_capabilities:
            if not _is_string(capability) or CAPABILITY_PATTERN.fullmatch(capability) is None:
                raise ResolverError(
                    "invalid_capabilities",
                    f"board '{alias}' has an invalid capability slug",
                )
            if capability in capabilities:
                raise ResolverError(
                    "duplicate_capability",
                    f"board '{alias}' contains a duplicate capability",
                )
            capabilities.append(capability)

        usb_serial = raw_board.get("usb_serial")
        if usb_serial is not None:
            if not _is_string(usb_serial):
                raise ResolverError("invalid_usb_identity", f"board '{alias}' has an invalid USB identity")
            usb_serial = usb_serial.strip()
            if any(marker in usb_serial.upper() for marker in PLACEHOLDER_MARKERS):
                raise ResolverError("invalid_usb_identity", f"board '{alias}' USB identity is still a placeholder")
            if usb_serial in seen_usb_serials:
                raise ResolverError("duplicate_usb_identity", f"{label} reuses a USB identity")
            seen_usb_serials.add(usb_serial)

        lan_base_url = raw_board.get("lan_base_url")
        if lan_base_url is not None:
            if not _is_string(lan_base_url):
                raise ResolverError("invalid_lan_base_url", f"board '{alias}' has an invalid LAN URL")
            lan_base_url = _validate_lan_base_url(lan_base_url.strip())

        capability_set = set(capabilities)
        if "serial" in capability_set and usb_serial is None:
            raise ResolverError(
                "missing_usb_identity",
                f"board '{alias}' advertises serial but has no USB identity",
            )
        if usb_serial is not None and "serial" not in capability_set:
            raise ResolverError(
                "unused_usb_identity",
                f"board '{alias}' has a USB identity but does not advertise serial",
            )
        if lan_base_url is not None and "lan" not in capability_set:
            raise ResolverError(
                "unused_lan_base_url",
                f"board '{alias}' has a LAN URL but does not advertise lan",
            )

        boards[alias] = Board(
            alias=alias,
            capabilities=tuple(sorted(capabilities)),
            usb_serial=usb_serial,
            lan_base_url=lan_base_url,
        )

    return boards


def load_inventory(
    template_path: Path,
    local_path: Path | None = None,
    *,
    trust_root_path: Path | None = None,
) -> Inventory:
    """Load the tracked template and authenticate any explicit local overlay."""

    boards = _validate_document(_read_json(template_path, "tracked inventory"), "tracked inventory")
    authentication: InventoryAuthentication | None = None
    if local_path is not None:
        local_payload, authentication = authenticate_local_inventory(
            local_path,
            trust_root_path=trust_root_path,
        )
        local_boards = _validate_document(local_payload, "local inventory")
        boards.update(local_boards)

    usb_owners: dict[str, str] = {}
    for board in boards.values():
        if board.usb_serial is None:
            continue
        if board.usb_serial in usb_owners:
            raise ResolverError("duplicate_usb_identity", "merged inventory reuses a USB identity")
        usb_owners[board.usb_serial] = board.alias

    return Inventory(boards=boards, authentication=authentication)


def _port_device(record: Mapping[str, object]) -> str | None:
    for key in ("port", "device", "name"):
        value = record.get(key)
        if _is_string(value):
            return value.strip()
    return None


def _port_usb_serial(record: Mapping[str, object]) -> str | None:
    for key in ("serial_number", "usb_serial", "serial"):
        value = record.get(key)
        if _is_string(value):
            return value.strip()

    hwid = record.get("hwid")
    if not isinstance(hwid, str):
        return None
    match = re.search(r"(?:^|\s)(?:SER|SERIAL)=([^\s]+)", hwid)
    return match.group(1) if match is not None else None


def parse_port_records(payload: object) -> tuple[Mapping[str, object], ...]:
    if isinstance(payload, dict):
        payload = payload.get("ports")
    if not isinstance(payload, list):
        raise ResolverError("invalid_port_inventory", "serial port inventory must be an array")
    if any(not isinstance(record, dict) for record in payload):
        raise ResolverError("invalid_port_inventory", "serial port inventory entries must be objects")
    return tuple(payload)


def enumerate_serial_ports(pio_command: str = "pio") -> tuple[Mapping[str, object], ...]:
    try:
        completed = subprocess.run(
            [pio_command, "device", "list", "--json-output"],
            capture_output=True,
            text=True,
            check=False,
            timeout=15,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise ResolverError("port_enumeration_failed", "serial ports could not be enumerated") from exc
    if completed.returncode != 0:
        raise ResolverError("port_enumeration_failed", "serial ports could not be enumerated")
    try:
        payload = json.loads(
            completed.stdout,
            object_pairs_hook=_reject_duplicate_json_keys,
        )
    except (json.JSONDecodeError, DuplicateJsonKeyError) as exc:
        raise ResolverError("invalid_port_inventory", "serial port enumeration returned invalid JSON") from exc
    return parse_port_records(payload)


def select_serial_port(records: Iterable[Mapping[str, object]], usb_serial: str) -> str:
    """Select an exact USB-serial match without any list-order fallback."""

    matches = {
        device
        for record in records
        if _port_usb_serial(record) == usb_serial
        for device in [_port_device(record)]
        if device is not None
    }
    if not matches:
        raise ResolverError("serial_board_not_connected", "configured board is not connected over serial")
    if len(matches) == 1:
        return next(iter(matches))

    # macOS exposes paired call-in (/dev/tty.*) and call-out (/dev/cu.*)
    # nodes for one USB CDC interface. Prefer the single call-out node by
    # semantics, never by enumeration order.
    callout_matches = {device for device in matches if device.startswith("/dev/cu.")}
    callin_matches = {device for device in matches if device.startswith("/dev/tty.")}
    if len(matches) == 2 and len(callout_matches) == 1 and len(callin_matches) == 1:
        callout = next(iter(callout_matches))
        callin = next(iter(callin_matches))
        if callout.removeprefix("/dev/cu.") == callin.removeprefix("/dev/tty."):
            return callout
    raise ResolverError("serial_board_ambiguous", "configured board maps to multiple serial ports")


def parse_firmware_lan_base_url(serial_text: str) -> str:
    """Return the most recent valid IPv4 URL from the firmware connection line."""

    candidates = [
        line[len(FIRMWARE_IP_PREFIX) :].strip()
        for line in serial_text.splitlines()
        if line.startswith(FIRMWARE_IP_PREFIX)
    ]
    if not candidates:
        raise ResolverError("firmware_ip_not_found", "firmware LAN address line was not found")
    try:
        address = ipaddress.IPv4Address(candidates[-1])
    except ipaddress.AddressValueError as exc:
        raise ResolverError("invalid_firmware_ip", "firmware reported an invalid LAN address") from exc
    if address.is_unspecified or address.is_loopback or address.is_multicast or address.is_reserved:
        raise ResolverError("invalid_firmware_ip", "firmware reported an unusable LAN address")
    return f"http://{address}"


def resolve_board(
    inventory: Inventory,
    alias: str,
    required_capabilities: Sequence[str],
    *,
    port_records: Iterable[Mapping[str, object]] = (),
    lan_collector: Callable[[str], str] | None = None,
) -> dict[str, object]:
    if ALIAS_PATTERN.fullmatch(alias) is None:
        raise ResolverError("invalid_alias", "requested alias must be a lowercase slug")
    board = inventory.boards.get(alias)
    if board is None:
        raise ResolverError("unknown_alias", "requested board alias is not configured")

    requested: list[str] = []
    for capability in required_capabilities:
        if not _is_string(capability) or CAPABILITY_PATTERN.fullmatch(capability) is None:
            raise ResolverError("invalid_capability", "requested capability must be a lowercase slug")
        if capability not in requested:
            requested.append(capability)
    if not requested:
        raise ResolverError("missing_capability", "at least one capability must be requested")

    missing = sorted(set(requested) - set(board.capabilities))
    if missing:
        raise ResolverError(
            "capability_unavailable",
            "configured board does not provide every requested capability",
        )

    endpoints: dict[str, str] = {}
    if "serial" in requested:
        assert board.usb_serial is not None  # established by inventory validation
        endpoints["serial_port"] = select_serial_port(port_records, board.usb_serial)
    if "lan" in requested:
        if board.lan_base_url is not None:
            endpoints["lan_base_url"] = board.lan_base_url
        else:
            resolved_serial_port = endpoints.get("serial_port")
            if resolved_serial_port is None:
                raise ResolverError(
                    "dynamic_lan_requires_serial",
                    "dynamic LAN discovery requires the serial capability in the same resolution",
                )
            if lan_collector is None:
                raise ResolverError(
                    "dynamic_lan_collector_required",
                    "dynamic LAN discovery requires resolver-owned collection",
                )
            endpoints["lan_base_url"] = _validate_lan_base_url(
                lan_collector(resolved_serial_port)
            )

    return {
        "schema_version": SCHEMA_VERSION,
        "alias": board.alias,
        "capabilities": sorted(requested),
        "endpoints": endpoints,
    }


def build_resolution_attestation(
    resolution: Mapping[str, object],
    *,
    observed_at_utc: str | None = None,
) -> dict[str, object]:
    """Return a sanitized digest of one successful resolver result."""

    if set(resolution) != {"schema_version", "alias", "capabilities", "endpoints"}:
        raise ResolverError(
            "invalid_resolution",
            "resolver attestation requires the exact successful resolution schema",
        )
    if resolution.get("schema_version") != SCHEMA_VERSION:
        raise ResolverError(
            "invalid_resolution",
            "resolver attestation schema does not match the resolver schema",
        )
    alias = resolution.get("alias")
    if not _is_string(alias) or ALIAS_PATTERN.fullmatch(alias) is None:
        raise ResolverError("invalid_resolution", "resolver attestation alias is invalid")
    capabilities = resolution.get("capabilities")
    if not isinstance(capabilities, list) or not capabilities:
        raise ResolverError(
            "invalid_resolution",
            "resolver attestation capabilities are invalid",
        )
    if capabilities != sorted(capabilities) or len(capabilities) != len(set(capabilities)):
        raise ResolverError(
            "invalid_resolution",
            "resolver attestation capabilities must be unique and sorted",
        )
    if any(
        not _is_string(capability)
        or CAPABILITY_PATTERN.fullmatch(capability) is None
        for capability in capabilities
    ):
        raise ResolverError(
            "invalid_resolution",
            "resolver attestation capability is invalid",
        )
    endpoints = resolution.get("endpoints")
    if not isinstance(endpoints, dict) or any(
        not _is_string(key) or not _is_string(value)
        for key, value in endpoints.items()
    ):
        raise ResolverError(
            "invalid_resolution",
            "resolver attestation endpoints are invalid",
        )
    expected_endpoints = {
        endpoint
        for capability, endpoint in (
            ("serial", "serial_port"),
            ("lan", "lan_base_url"),
        )
        if capability in capabilities
    }
    if set(endpoints) != expected_endpoints:
        raise ResolverError(
            "invalid_resolution",
            "resolver attestation endpoints do not match resolved capabilities",
        )

    if observed_at_utc is None:
        observed_at_utc = (
            datetime.now(timezone.utc)
            .isoformat(timespec="seconds")
            .replace("+00:00", "Z")
        )
    if not isinstance(observed_at_utc, str) or UTC_TIMESTAMP_PATTERN.fullmatch(
        observed_at_utc
    ) is None:
        raise ResolverError(
            "invalid_attestation_time",
            "resolver attestation time must be an RFC3339 UTC timestamp ending in Z",
        )
    try:
        observed = datetime.fromisoformat(observed_at_utc.replace("Z", "+00:00"))
    except ValueError as exc:
        raise ResolverError(
            "invalid_attestation_time",
            "resolver attestation time must be an RFC3339 UTC timestamp ending in Z",
        ) from exc
    if (
        not observed_at_utc.endswith("Z")
        or observed.tzinfo is None
        or observed.utcoffset() != timezone.utc.utcoffset(observed)
    ):
        raise ResolverError(
            "invalid_attestation_time",
            "resolver attestation time must be an RFC3339 UTC timestamp ending in Z",
        )

    canonical = json.dumps(
        resolution,
        ensure_ascii=True,
        separators=(",", ":"),
        sort_keys=True,
    ).encode("utf-8")
    return {
        "schema_version": ATTESTATION_SCHEMA_VERSION,
        "resolver_schema_version": SCHEMA_VERSION,
        "alias": alias,
        "capabilities": capabilities,
        "resolution_sha256": hashlib.sha256(canonical).hexdigest(),
        "observed_at_utc": observed_at_utc,
    }


def _load_ports_json(path: Path) -> tuple[Mapping[str, object], ...]:
    return parse_port_records(_read_json(path, "serial port inventory"))


def collect_firmware_lan_base_url(
    serial_port: str,
    *,
    timeout_seconds: float = 15.0,
) -> str:
    """Collect a firmware LAN line directly from the selected serial endpoint."""

    if timeout_seconds <= 0 or timeout_seconds > 120:
        raise ResolverError(
            "invalid_collection_timeout",
            "LAN collection timeout must be greater than zero and at most 120 seconds",
        )
    try:
        import serial  # type: ignore[import-not-found]
    except ImportError as exc:
        raise ResolverError(
            "serial_collector_unavailable",
            "resolver-owned serial collection is unavailable",
        ) from exc
    deadline = time.monotonic() + timeout_seconds
    try:
        with serial.Serial(serial_port, 115200, timeout=0.25) as connection:
            while time.monotonic() < deadline:
                raw = connection.readline()
                if not raw:
                    continue
                line = raw.decode("utf-8", errors="replace").strip()
                if not line.startswith(FIRMWARE_IP_PREFIX):
                    continue
                return parse_firmware_lan_base_url(line)
    except (OSError, ValueError, serial.SerialException) as exc:
        raise ResolverError(
            "serial_collection_failed",
            "resolver-owned serial collection failed",
        ) from exc
    raise ResolverError(
        "firmware_ip_not_found",
        "firmware LAN address line was not observed before the timeout",
    )


def _write_local_json(
    path: Path,
    payload: Mapping[str, object],
    error_code: str,
) -> None:
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("x", encoding="utf-8") as handle:
            handle.write(json.dumps(payload, indent=2, sort_keys=True) + "\n")
    except (OSError, ValueError) as exc:
        raise ResolverError(
            error_code,
            "resolver local output could not be written",
        ) from exc


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("alias", help="local board alias to resolve")
    parser.add_argument(
        "--capability",
        action="append",
        required=True,
        dest="capabilities",
        help="required capability (repeat as needed)",
    )
    parser.add_argument("--template", type=Path, default=DEFAULT_TEMPLATE)
    parser.add_argument("--inventory", type=Path, default=DEFAULT_LOCAL_INVENTORY)
    parser.add_argument(
        "--ports-json",
        type=Path,
        help="read a PlatformIO-compatible port listing instead of enumerating ports",
    )
    parser.add_argument(
        "--attestation-output",
        type=Path,
        help="write a sanitized, hashed resolver attestation to this local path",
    )
    parser.add_argument(
        "--local-resolution-output",
        type=Path,
        help="write ignored raw endpoint resolution for later digest recomputation",
    )
    parser.add_argument(
        "--lan-collection-timeout-seconds",
        type=float,
        default=15.0,
        help="resolver-owned serial LAN collection timeout (maximum 120 seconds)",
    )
    parser.add_argument("--pio-command", default=os.environ.get("PIO_CMD", "pio"))
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    parser = _build_parser()
    args = parser.parse_args(argv)
    try:
        inventory = load_inventory(args.template, args.inventory)
        port_records: tuple[Mapping[str, object], ...] = ()
        if "serial" in args.capabilities:
            port_records = (
                _load_ports_json(args.ports_json)
                if args.ports_json is not None
                else enumerate_serial_ports(args.pio_command)
            )
        resolution = resolve_board(
            inventory,
            args.alias,
            args.capabilities,
            port_records=port_records,
            lan_collector=lambda port: collect_firmware_lan_base_url(
                port,
                timeout_seconds=args.lan_collection_timeout_seconds,
            ),
        )
        attestation = build_resolution_attestation(resolution)
        if args.local_resolution_output is not None:
            _write_local_json(
                args.local_resolution_output,
                resolution,
                "resolution_write_failed",
            )
        if args.attestation_output is not None:
            _write_local_json(
                args.attestation_output,
                attestation,
                "attestation_write_failed",
            )
    except ResolverError as exc:
        error = {"error": {"code": exc.code, "message": exc.message}}
        print(json.dumps(error, sort_keys=True), file=sys.stderr)
        return 2

    print(json.dumps(attestation, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Build a versioned, uncertainty-aware bench timeline without changing inputs."""

from __future__ import annotations

import csv
import hashlib
import json
import re
from pathlib import Path
from typing import Any, Iterable, Mapping, Sequence

from camera_timing import validate_frame_sidecar
from clock_alignment import fit_clock_alignment, map_dut_timestamp


ALIGNED_TIMELINE_SCHEMA_VERSION = 1
CLOCK_ALIGNMENT_FILENAME = "clock_alignment.json"
ALIGNED_TIMELINE_FILENAME = "aligned_timeline.ndjson"
_QSYNC_SERIAL_REPLY = re.compile(
    r"^QSYNC ([0-9a-fA-F]{16}) ([0-9a-fA-F]{16}) "
    r"([0-9a-fA-F]{16}) ([0-9a-fA-F]{16})$"
)
_QUALIFICATION_SESSION_TOKEN_ALIASES = (
    "qualification_session_token",
    "qualificationSessionToken",
    "session_token",
    "sessionToken",
)
_OUTSIDE_QUALIFICATION_HOST_SOURCES = frozenset(
    {
        "v1replay_reconnect_preflight",
    }
)
_HOST_TIMESTAMP_ALIASES = (
    "host_monotonic_ns",
    "hostMonotonicNs",
    "callback_host_ns",
    "observed_host_ns",
    "requestedHostMonotonicNs",
    "requested_host_monotonic_ns",
    "observer_host_monotonic_ns",
)


def _first(record: Mapping[str, Any], names: Sequence[str]) -> Any:
    for name in names:
        if name in record and record[name] not in (None, ""):
            return record[name]
    return None


def _integer(value: Any) -> int | None:
    if isinstance(value, bool):
        return None
    if isinstance(value, int):
        return value
    if isinstance(value, str):
        try:
            return int(value, 10)
        except ValueError:
            return None
    return None


def _normalise_qualification_session_token(value: Any) -> str | None:
    if value in (None, "") or isinstance(value, bool):
        return None
    if isinstance(value, int):
        return f"{value:08x}"
    text = str(value).strip().lower()
    if text.startswith("0x"):
        text = text[2:]
    if not text:
        return None
    if len(text) <= 8 and all(character in "0123456789abcdef" for character in text):
        return text.zfill(8)
    return text


def _record_qualification_session_token(record: Mapping[str, Any]) -> str | None:
    return _normalise_qualification_session_token(
        _first(record, _QUALIFICATION_SESSION_TOKEN_ALIASES)
    )


def _json_protocol_payload(line: Any) -> Mapping[str, Any] | None:
    if not isinstance(line, str):
        return None
    prefix, separator, payload = line.partition(" ")
    if not separator or prefix not in {"QRESP", "QEVENT"}:
        return None
    try:
        decoded = json.loads(payload)
    except json.JSONDecodeError:
        return None
    return decoded if isinstance(decoded, Mapping) else None


def _current_qualification_session_token(
    host_records: Sequence[Mapping[str, Any]],
) -> str | None:
    """Find the scored QSTART session without accepting a stale pre-window token."""
    candidates: list[str] = []
    for record in host_records:
        event = str(_first(record, ("event", "kind", "state", "type")) or "").lower()
        phase = str(record.get("phase") or "").lower()
        response = record.get("response")
        if event == "qstatus_round_trip" and phase == "post_window" and isinstance(response, Mapping):
            token = _record_qualification_session_token(response)
            if token is not None and token != "00000000":
                candidates.append(token)

        payload = _json_protocol_payload(record.get("line"))
        if payload is None:
            continue
        state = str(payload.get("state") or "").lower()
        if state not in {"running", "done", "error", "finalizing"}:
            continue
        token = _record_qualification_session_token(payload)
        if token is not None and token != "00000000":
            candidates.append(token)
    return candidates[-1] if candidates else None


def _latest_session_start_token(records: Sequence[Mapping[str, Any]]) -> str | None:
    candidates = [
        token
        for record in records
        if str(_first(record, ("stage", "kind", "event")) or "").upper() == "SESSION_START"
        and (token := _record_qualification_session_token(record)) is not None
        and token != "00000000"
    ]
    return candidates[-1] if candidates else None


def _records_for_qualification_session(
    records: Sequence[Mapping[str, Any]],
    session_token: str | None,
    *,
    retain_unscoped_tail_context: bool = False,
) -> list[dict[str, Any]]:
    if session_token is None:
        return [dict(record) for record in records]
    selected_indices = [
        index
        for index, record in enumerate(records)
        if _record_qualification_session_token(record) == session_token
    ]
    selected = [dict(records[index]) for index in selected_indices]
    if not retain_unscoped_tail_context or not selected_indices:
        return selected

    # Display/encounter files are boot-prefix logs and can legitimately retain
    # token-zero rows after QEVENT. Keep the physical tail only as private
    # context so a boot-wide terminal sequence can be distinguished from an
    # actually missing final row. A later nonzero token is another qualification
    # session and must never be treated as an explanatory unscored tail.
    tail = [dict(record) for record in records[selected_indices[-1] + 1 :]]
    if tail and all(
        _record_qualification_session_token(record) in {None, "00000000"}
        for record in tail
    ):
        selected[-1]["__qualification_unscoped_tail__"] = tail
    return selected


def _qualification_host_window(
    records: Sequence[Mapping[str, Any]],
    causal_records: Sequence[Mapping[str, Any]],
    alignment: Mapping[str, Any],
    session_token: str | None,
) -> tuple[int, int, int] | None:
    """Return a definitely-in-session host interval or abstain."""
    if session_token is None:
        return None
    starts: list[int] = []
    for record in records:
        line = record.get("line")
        payload = _json_protocol_payload(line)
        if payload is None or _record_qualification_session_token(payload) != session_token:
            continue
        timestamp = _integer(_first(record, _HOST_TIMESTAMP_ALIASES))
        if timestamp is None or not isinstance(line, str):
            continue
        prefix = line.partition(" ")[0]
        state = str(payload.get("state") or "").lower()
        if prefix == "QRESP" and state == "running":
            starts.append(timestamp)

    # QEVENT receipt is later than the DUT's evidence cutoff by an unknown
    # serial-transit interval. Bound the scored host rows by the earliest host
    # time permitted for the exact persisted SESSION_END occurrence instead.
    conservative_ends: list[tuple[int, int]] = []
    for record in causal_records:
        stage = str(_first(record, ("stage", "kind", "event")) or "").upper()
        if (
            stage != "SESSION_END"
            or _record_qualification_session_token(record) != session_token
        ):
            continue
        dut_us = _integer(
            _first(
                record,
                (
                    "stage_dut_monotonic_us",
                    "stage_dut_us",
                    "stage_dut_micros",
                    "dut_monotonic_us",
                    "dut_micros",
                    "timestamp_dut_us",
                ),
            )
        )
        causal = _causal_identifiers(record)
        mapped = map_dut_timestamp(
            alignment,
            causal.get("clock_segment"),
            dut_us,
            segment_instance=_integer(causal.get("segment_instance")),
        )
        earliest = _integer(mapped.get("host_earliest_ns"))
        latest = _integer(mapped.get("host_latest_ns"))
        if (
            mapped.get("status") == "mapped"
            and earliest is not None
            and latest is not None
            and earliest <= latest
        ):
            conservative_ends.append((earliest, latest))
    if not starts or not conservative_ends:
        return None
    start = starts[0]
    end_earliest, end_latest = conservative_ends[-1]
    return (start, end_earliest, end_latest) if start <= end_latest else None


def _qualification_scope_for_host_timestamp(
    record: Mapping[str, Any],
    timestamp: Any,
    session_token: str | None,
    window: tuple[int, int, int] | None,
) -> str | None:
    if session_token is None:
        return None
    sources = {
        str(record.get(name) or "").strip().lower()
        for name in ("timeline_source", "source")
    }
    if not sources.isdisjoint(_OUTSIDE_QUALIFICATION_HOST_SOURCES):
        return "outside_current_session"
    host_ns = _integer(timestamp)
    if window is None or host_ns is None:
        return "indeterminate_current_session"
    if window[0] <= host_ns < window[1]:
        return "current_session"
    if window[1] <= host_ns <= window[2]:
        return "indeterminate_current_session"
    return "outside_current_session"


def _normalise_characteristic(value: Any) -> str | None:
    if value is None:
        return None
    text = str(value).strip().lower()
    aliases = {
        "s": "state",
        "state": "state",
        "state_characteristic": "state",
        "a": "alert",
        "alert": "alert",
        "alert_characteristic": "alert",
    }
    return aliases.get(text, text or None)


def _normalise_hex(value: Any) -> str | None:
    if value is None:
        return None
    text = str(value).strip().lower().replace(" ", "")
    if text.startswith("0x"):
        text = text[2:]
    if not text or len(text) % 2 or any(character not in "0123456789abcdef" for character in text):
        return None
    return text


def _payload_digest(record: Mapping[str, Any]) -> str | None:
    digest = _first(
        record,
        (
            "payload_sha256",
            "payloadSha256",
            "payload_digest_sha256",
            "sha256",
        ),
    )
    if digest is not None:
        text = str(digest).strip().lower()
        if len(text) == 64 and all(character in "0123456789abcdef" for character in text):
            return text
    payload_hex = _normalise_hex(
        _first(
            record,
            ("payload_hex", "payloadHex", "bytes_hex", "bytesHex", "exact_payload_hex"),
        )
    )
    if payload_hex is not None:
        return hashlib.sha256(bytes.fromhex(payload_hex)).hexdigest()
    return None


def _payload_length(record: Mapping[str, Any]) -> int | None:
    length = _integer(
        _first(record, ("payload_length", "payloadLength", "length", "byte_count"))
    )
    if length is not None:
        return length
    payload_hex = _normalise_hex(
        _first(
            record,
            ("payload_hex", "payloadHex", "bytes_hex", "bytesHex", "exact_payload_hex"),
        )
    )
    return None if payload_hex is None else len(payload_hex) // 2


def read_ndjson_records(path: Path) -> list[dict[str, Any]]:
    """Read object NDJSON while retaining physical source line numbers."""
    result: list[dict[str, Any]] = []
    with path.open(encoding="utf-8") as handle:
        for line_number, raw in enumerate(handle, start=1):
            if not raw.strip():
                continue
            try:
                record = json.loads(raw)
            except json.JSONDecodeError as exc:
                raise ValueError(f"{path.name} line {line_number} is invalid JSON") from exc
            if not isinstance(record, dict):
                raise ValueError(f"{path.name} line {line_number} is not an object")
            result.append(
                {
                    **record,
                    "__source_artifact__": path.name,
                    "__source_record__": line_number,
                }
            )
    return result


def read_csv_records(path: Path) -> list[dict[str, Any]]:
    """Read repeated-header session CSV without inventing rows."""
    metadata: dict[str, str] = {}
    session: dict[str, Any] = {}
    header: list[str] | None = None
    result: list[dict[str, Any]] = []
    with path.open(encoding="utf-8", newline="") as handle:
        for line_number, raw in enumerate(handle, start=1):
            if raw.startswith("#"):
                parsed: dict[str, Any] = {}
                for field in raw[1:].strip().split(","):
                    key, separator, value = field.partition("=")
                    if separator:
                        parsed[key.strip()] = value.strip()
                if raw.startswith("#session_start"):
                    session = {**parsed, "__source_record__": line_number}
                else:
                    metadata.update(parsed)
                continue
            if not raw.strip():
                continue
            values = next(csv.reader([raw]))
            if header is None or (values and values[0] == header[0]):
                header, session = values, {}
                continue
            if len(values) > len(header):
                raise ValueError(f"{path.name} line {line_number} has extra unnamed columns")
            values.extend([""] * (len(header) - len(values)))
            result.append(
                {
                    **dict(zip(header, values)),
                    "__source_artifact__": path.name,
                    "__source_record__": line_number,
                    "__csv_session__": dict(session),
                }
            )
    for record in result:
        record["__csv_metadata__"] = dict(metadata)
    return result


def _records(
    source: Path | Sequence[Mapping[str, Any]] | None,
    default_artifact: str,
) -> list[dict[str, Any]]:
    if source is None:
        return []
    if isinstance(source, Path):
        loaded = read_ndjson_records(source) if source.suffix in {".ndjson", ".jsonl"} else read_csv_records(source)
        return loaded
    result: list[dict[str, Any]] = []
    for index, record in enumerate(source, start=1):
        result.append(
            {
                **record,
                "__source_artifact__": record.get("__source_artifact__", default_artifact),
                "__source_record__": record.get("__source_record__", index),
            }
        )
    return result


def _evidence_loss_reasons(
    records: Sequence[Mapping[str, Any]],
    *,
    sequence_names: Sequence[str],
    counter_names: Sequence[str],
    terminal_sequence_names: Sequence[str],
) -> list[str]:
    """Summarize explicit artifact loss without guessing where the gap occurred."""
    reasons: list[str] = []
    sequences = [
        value
        for record in records
        if (value := _integer(_first(record, sequence_names))) is not None
    ]
    if any(current != previous + 1 for previous, current in zip(sequences, sequences[1:])):
        reasons.append("source_sequence_gap")

    stages = {
        str(_first(record, ("stage", "kind", "event")) or "").upper()
        for record in records
    }
    metadata_records = [
        metadata
        for record in records
        if isinstance((metadata := record.get("__csv_metadata__")), Mapping)
    ]
    for name in counter_names:
        counter_values = [
            value
            for record in records
            if (value := _integer(record.get(name))) is not None
        ]
        baseline = min(counter_values) if counter_values else 0
        if counter_values and max(counter_values) > baseline:
            reasons.append("reported_loss_counter_increased")
        elif counter_values and max(counter_values) > 0 and "SESSION_START" not in stages:
            reasons.append("nonzero_loss_counter_without_session_baseline")
        metadata_counter_values = [
            value
            for metadata in metadata_records
            if (value := _integer(metadata.get(name))) is not None
        ]
        if metadata_counter_values and max(metadata_counter_values) > baseline:
            reasons.append("terminal_loss_counter_increased")

    unscoped_tail = next(
        (
            tail
            for record in records
            if isinstance((tail := record.get("__qualification_unscoped_tail__")), list)
        ),
        [],
    )
    tail_sequences = [
        value
        for record in unscoped_tail
        if isinstance(record, Mapping)
        and (value := _integer(_first(record, sequence_names))) is not None
    ]

    for metadata in metadata_records:
        terminal_sequence = _integer(_first(metadata, terminal_sequence_names))
        tail_explains_terminal = bool(
            terminal_sequence is not None
            and sequences
            and tail_sequences
            and tail_sequences[0] == sequences[-1] + 1
            and all(
                following == current + 1
                for current, following in zip(tail_sequences, tail_sequences[1:])
            )
            and tail_sequences[-1] == terminal_sequence
        )
        if (
            terminal_sequence is not None
            and sequences
            and terminal_sequence != sequences[-1]
            and not tail_explains_terminal
        ):
            reasons.append("terminal_sequence_does_not_match_rows")

    return list(dict.fromkeys(reasons))


def _records_have_evidence_loss(records: Sequence[Mapping[str, Any]]) -> bool:
    return any(record.get("evidence_complete") is False for record in records)


def extract_qsync_exchanges(records: Iterable[Mapping[str, Any]]) -> list[dict[str, Any]]:
    """Extract QSYNC evidence in physical reply order, accepting additive columns."""
    materialized = list(records)
    request_h1_by_nonce: dict[str, Any] = {}
    for record in materialized:
        nonce = _first(record, ("nonce", "qsync_nonce", "request_nonce"))
        h1 = _first(
            record,
            ("h1_host_monotonic_ns", "h1_host_ns", "h1_ns", "host_send_ns", "request_write_host_ns"),
        )
        if nonce is not None and h1 is not None:
            request_h1_by_nonce[str(nonce).lower()] = h1

    joined_unexpected_by_record: dict[int, list[dict[str, Any]]] = {}
    represented_reply_nonces: set[str] = set()
    for record_index, record in enumerate(materialized):
        unexpected = record.get("unexpected_replies")
        joined_replies: list[dict[str, Any]] = []
        if isinstance(unexpected, list):
            for reply in unexpected:
                if not isinstance(reply, Mapping):
                    continue
                reply_nonce = _first(reply, ("reply_nonce", "nonce"))
                h1 = _first(
                    reply,
                    (
                        "h1_host_monotonic_ns",
                        "h1_host_ns",
                        "h1_ns",
                        "host_send_ns",
                        "request_write_host_ns",
                    ),
                )
                if h1 is None and reply_nonce is not None:
                    h1 = request_h1_by_nonce.get(str(reply_nonce).lower())
                joined = {
                    "status": "observed",
                    "late_reply": True,
                    "nonce": reply_nonce,
                    "clock_segment": _first(reply, ("clock_segment", "clock_segment_id")),
                    "h1_host_ns": h1,
                    "d2_dut_us": _first(reply, ("d2_dut_us", "d2_us")),
                    "d3_dut_us": _first(reply, ("d3_dut_us", "d3_us")),
                    "h4_host_ns": _first(reply, ("h4_host_ns", "h4_ns")),
                    "parent_exchange_sequence": record.get("exchange_sequence"),
                }
                if all(
                    joined[name] is not None
                    for name in (
                        "nonce",
                        "clock_segment",
                        "h1_host_ns",
                        "d2_dut_us",
                        "d3_dut_us",
                        "h4_host_ns",
                    )
                ):
                    joined_replies.append(joined)
                    represented_reply_nonces.add(str(reply_nonce).lower())
        joined_unexpected_by_record[record_index] = joined_replies

        has_complete_top_level_reply = all(
            _first(record, aliases) is not None
            for aliases in (
                ("h1_host_monotonic_ns", "h1_host_ns", "h1_ns", "host_send_ns", "request_write_host_ns"),
                ("d2_dut_monotonic_us", "d2_dut_us", "d2_us", "dut_parse_us", "request_parse_dut_us"),
                ("d3_dut_monotonic_us", "d3_dut_us", "d3_us", "dut_reply_us", "reply_enqueue_dut_us"),
                ("h4_host_monotonic_ns", "h4_host_ns", "h4_ns", "host_receive_ns", "reply_receive_host_ns"),
            )
        )
        top_level_reply_nonce = _first(record, ("reply_nonce", "nonce", "qsync_nonce"))
        if has_complete_top_level_reply and top_level_reply_nonce is not None:
            represented_reply_nonces.add(str(top_level_reply_nonce).lower())

    exchanges: list[dict[str, Any]] = []
    reconstructed_serial_nonces: set[str] = set()
    for record_index, record in enumerate(materialized):
        event = str(_first(record, ("kind", "event", "state", "type")) or "").lower()
        if event == "serial_receive":
            serial_match = _QSYNC_SERIAL_REPLY.fullmatch(str(record.get("line") or ""))
            if serial_match is not None:
                nonce, segment_wire, d2_wire, d3_wire = serial_match.groups()
                nonce = nonce.lower()
                if nonce not in represented_reply_nonces and nonce not in reconstructed_serial_nonces:
                    h1 = request_h1_by_nonce.get(nonce)
                    h4 = _first(
                        record,
                        (
                            "host_monotonic_ns",
                            "observer_host_monotonic_ns",
                            "h4_host_ns",
                            "receive_host_monotonic_ns",
                        ),
                    )
                    if h1 is not None and h4 is not None:
                        exchanges.append(
                            {
                                "status": "observed",
                                "late_reply": True,
                                "reconstructed_from_serial_receive": True,
                                "nonce": nonce,
                                "clock_segment": str(int(segment_wire, 16)),
                                "clock_segment_wire": segment_wire.lower(),
                                "h1_host_ns": h1,
                                "d2_dut_us": int(d2_wire, 16),
                                "d3_dut_us": int(d3_wire, 16),
                                "h4_host_ns": h4,
                            }
                        )
                        reconstructed_serial_nonces.add(nonce)
            continue

        has_four_timestamps = all(
            _first(record, aliases) is not None
            for aliases in (
                ("h1_host_monotonic_ns", "h1_host_ns", "h1_ns", "host_send_ns", "request_write_host_ns"),
                ("d2_dut_monotonic_us", "d2_dut_us", "d2_us", "dut_parse_us", "request_parse_dut_us"),
                ("d3_dut_monotonic_us", "d3_dut_us", "d3_us", "dut_reply_us", "reply_enqueue_dut_us"),
                ("h4_host_monotonic_ns", "h4_host_ns", "h4_ns", "host_receive_ns", "reply_receive_host_ns"),
            )
        )
        if "qsync" not in event and not has_four_timestamps:
            continue
        # QSyncCollector encounters every unexpected reply before the enclosing
        # expected reply. Preserve that wire order so a late prior-boot reply
        # cannot split the new clock segment into two mapping instances.
        exchanges.extend(joined_unexpected_by_record[record_index])
        exchanges.append({key: value for key, value in record.items() if not key.startswith("__")})
    return exchanges


def _causal_identifiers(record: Mapping[str, Any]) -> dict[str, Any]:
    aliases: dict[str, tuple[str, ...]] = {
        "stimulus_sequence": ("stimulus_sequence", "stimulusSequence"),
        "emission_ordinal": ("emission_ordinal", "emissionOrdinal", "ordinal"),
        "global_tx_sequence": ("global_tx_sequence", "globalTxSequence"),
        "clock_segment": (
            "clock_segment",
            "clock_segment_id",
            "boot_clock_id",
            "boot_clock_segment",
            "clockSegment",
        ),
        "segment_instance": ("segment_instance",),
        "qualification_session_token": ("qualification_session_token",),
        "ble_session_generation": ("ble_session_generation",),
        "rx_first_seq": ("rx_first_seq",),
        "rx_last_seq": ("rx_last_seq",),
        "event_seq": ("event_seq", "source_event_seq", "packet_event_seq"),
        "packet_id": ("packet_id", "packetId"),
        "state_revision": ("state_revision", "stateRevision"),
        "alert_revision": ("alert_revision", "alertRevision"),
        "commit_seq": ("commit_seq", "seq", "display_commit_seq"),
        "frame_seq": ("frame_seq", "frameSequence"),
    }
    result: dict[str, Any] = {}
    for canonical, names in aliases.items():
        value = _first(record, names)
        if value not in (None, ""):
            if canonical == "qualification_session_token":
                token = _normalise_qualification_session_token(value)
                if token is not None:
                    result[canonical] = token
                continue
            parsed = _integer(value)
            result[canonical] = value if parsed is None else parsed
    characteristic = _normalise_characteristic(
        _first(record, ("characteristic", "channel", "payload_unit"))
    )
    length = _payload_length(record)
    digest = _payload_digest(record)
    if characteristic is not None:
        result["characteristic"] = characteristic
    if length is not None:
        result["payload_length"] = length
    if digest is not None:
        result["payload_sha256"] = digest
    fnv = _first(record, ("payload_fnv1a32", "payloadFnv1a32"))
    if fnv not in (None, ""):
        result["payload_fnv1a32"] = fnv
    return result


def _record_id(source_artifact: str, source_record: Any, kind: str, suffix: str = "") -> str:
    identity = f"{source_artifact}\n{source_record}\n{kind}\n{suffix}"
    return hashlib.sha256(identity.encode("utf-8")).hexdigest()[:24]


def _aligned_record(
    *,
    kind: str,
    raw_clock: str,
    raw_timestamp: Any,
    source_artifact: str,
    source_record: Any,
    causal: Mapping[str, Any],
    estimate: int | None,
    earliest: int | None,
    latest: int | None,
    suffix: str = "",
    **extra: Any,
) -> dict[str, Any]:
    return {
        "schema_version": ALIGNED_TIMELINE_SCHEMA_VERSION,
        "kind": kind,
        "record_id": _record_id(source_artifact, source_record, kind, suffix),
        "raw_clock": raw_clock,
        "raw_timestamp": raw_timestamp,
        "host_estimate_ns": estimate,
        "host_earliest_ns": earliest,
        "host_latest_ns": latest,
        "source_artifact": source_artifact,
        "source_record": source_record,
        "causal_identifiers": dict(causal),
        **extra,
    }


def _host_record(
    kind: str,
    timestamp: Any,
    record: Mapping[str, Any],
    *,
    causal: Mapping[str, Any] | None = None,
    qualification_session_token: str | None = None,
    qualification_window: tuple[int, int, int] | None = None,
    suffix: str = "",
    **extra: Any,
) -> dict[str, Any]:
    host_ns = _integer(timestamp)
    artifact = str(record["__source_artifact__"])
    source_record = record["__source_record__"]
    scoped_causal = dict(causal or _causal_identifiers(record))
    qualification_scope = _qualification_scope_for_host_timestamp(
        record,
        timestamp,
        qualification_session_token,
        qualification_window,
    )
    if qualification_scope == "current_session" and qualification_session_token is not None:
        scoped_causal["qualification_session_token"] = qualification_session_token
    elif qualification_session_token is not None:
        scoped_causal.pop("qualification_session_token", None)

    limitations = list(extra.pop("limitations", []))
    if qualification_scope == "outside_current_session":
        limitations.append("outside_current_qualification_session")
    elif qualification_scope == "indeterminate_current_session":
        limitations.append("qualification_session_bounds_incomplete")
    return _aligned_record(
        kind=kind,
        raw_clock="host_monotonic_ns",
        raw_timestamp=timestamp,
        source_artifact=artifact,
        source_record=source_record,
        causal=scoped_causal,
        estimate=host_ns,
        earliest=host_ns,
        latest=host_ns,
        suffix=suffix,
        alignment_status="mapped" if host_ns is not None else "invalid_timestamp",
        qualification_scope=qualification_scope,
        limitations=limitations,
        **extra,
    )


def _dut_timestamp(record: Mapping[str, Any], names: Sequence[str]) -> tuple[Any, int | None, str]:
    value = _first(record, names)
    parsed = _integer(value)
    if parsed is not None:
        return value, parsed, "dut_monotonic_us"
    millis = _first(record, ("stage_dut_millis", "dut_millis", "millis", "commit_millis"))
    parsed_millis = _integer(millis)
    return millis, None if parsed_millis is None else parsed_millis * 1000, "dut_millis"


def _dut_record(
    kind: str,
    record: Mapping[str, Any],
    alignment: Mapping[str, Any],
    *,
    timestamp_names: Sequence[str],
    suffix: str = "",
    **extra: Any,
) -> dict[str, Any]:
    raw_timestamp, dut_us, raw_clock = _dut_timestamp(record, timestamp_names)
    causal = _causal_identifiers(record)
    mapped = map_dut_timestamp(
        alignment,
        causal.get("clock_segment"),
        dut_us,
        segment_instance=_integer(causal.get("segment_instance")),
    )
    limitations: list[str] = []
    if raw_clock == "dut_millis":
        limitations.append("millisecond_precision_source")
    if mapped.get("status") != "mapped":
        limitations.append(str(mapped.get("reason") or mapped.get("status")))
    if mapped.get("poor_fit") is True:
        limitations.append("poor_clock_fit")
    return _aligned_record(
        kind=kind,
        raw_clock=raw_clock,
        raw_timestamp=raw_timestamp,
        source_artifact=str(record["__source_artifact__"]),
        source_record=record["__source_record__"],
        causal=causal,
        estimate=mapped.get("host_estimate_ns"),
        earliest=mapped.get("host_earliest_ns"),
        latest=mapped.get("host_latest_ns"),
        suffix=suffix,
        alignment_status=mapped.get("status"),
        clock_mapping_id=mapped.get("mapping_id"),
        clock_fit_type=mapped.get("fit_type"),
        clock_fit_quality=mapped.get("fit_quality"),
        clock_fit_poor=mapped.get("poor_fit"),
        clock_uncertainty_width_ns=mapped.get("uncertainty_width_ns"),
        limitations=limitations,
        **extra,
    )


def _host_events(
    records: Sequence[Mapping[str, Any]],
    *,
    qualification_session_token: str | None = None,
    qualification_window: tuple[int, int, int] | None = None,
) -> list[dict[str, Any]]:
    result: list[dict[str, Any]] = []
    host_aliases = _HOST_TIMESTAMP_ALIASES
    deadline_aliases = (
        "intendedHostMonotonicNs",
        "intended_host_monotonic_ns",
        "intended_deadline_host_ns",
        "planned_deadline_host_ns",
        "plannedHostMonotonicNs",
        "deadline_host_monotonic_ns",
    )
    for record in records:
        state = str(_first(record, ("state", "kind", "event", "type")) or "").lower()
        timestamp = _first(record, host_aliases)
        if state in {"stimulus_requested", "replay_stimulus", "stimulus"}:
            result.append(
                _host_record(
                    "stimulus_requested",
                    timestamp,
                    record,
                    qualification_session_token=qualification_session_token,
                    qualification_window=qualification_window,
                )
            )
            notifications = record.get("notifications")
            deadline = _first(record, deadline_aliases)
            if isinstance(notifications, list):
                for ordinal, notification in enumerate(notifications):
                    if not isinstance(notification, Mapping):
                        continue
                    merged = {**record, **notification, "emission_ordinal": notification.get("emissionOrdinal", ordinal)}
                    causal = _causal_identifiers(merged)
                    result.append(
                        _host_record(
                            "replay_deadline",
                            deadline,
                            record,
                            causal=causal,
                            qualification_session_token=qualification_session_token,
                            qualification_window=qualification_window,
                            suffix=str(ordinal),
                        )
                    )
            elif deadline is not None:
                result.append(
                    _host_record(
                        "replay_deadline",
                        deadline,
                        record,
                        qualification_session_token=qualification_session_token,
                        qualification_window=qualification_window,
                    )
                )
        elif state in {"notification_requested", "requested"}:
            result.append(
                _host_record(
                    "notification_requested",
                    timestamp,
                    record,
                    qualification_session_token=qualification_session_token,
                    qualification_window=qualification_window,
                )
            )
        elif state in {"notification_accepted", "accepted"}:
            result.append(
                _host_record(
                    "notification_accepted",
                    timestamp,
                    record,
                    qualification_session_token=qualification_session_token,
                    qualification_window=qualification_window,
                )
            )
        elif state in {
            "notification_delayed",
            "notification_dropped",
            "notification_skipped",
        }:
            result.append(
                _host_record(
                    state,
                    timestamp,
                    record,
                    qualification_session_token=qualification_session_token,
                    qualification_window=qualification_window,
                )
            )
        elif "qsync" in state:
            h1 = _first(record, ("h1_host_monotonic_ns", "h1_host_ns", "h1_ns", "host_send_ns"))
            h4 = _first(record, ("h4_host_monotonic_ns", "h4_host_ns", "h4_ns", "host_receive_ns"))
            result.append(
                _aligned_record(
                    kind="clock_sync_exchange",
                    raw_clock="host_and_dut_four_timestamp",
                    raw_timestamp={
                        key: record.get(key)
                        for key in record
                        if key.lower().startswith(("h1", "h4", "d2", "d3"))
                    },
                    source_artifact=str(record["__source_artifact__"]),
                    source_record=record["__source_record__"],
                    causal=_causal_identifiers(record),
                    estimate=(None if _integer(h1) is None or _integer(h4) is None else (_integer(h1) + _integer(h4)) // 2),
                    earliest=_integer(h1),
                    latest=_integer(h4),
                    alignment_status="mapped" if _integer(h1) is not None and _integer(h4) is not None else "invalid_timestamp",
                )
            )
        elif state == "serial_receive":
            result.append(
                _host_record(
                    "serial_observation",
                    timestamp,
                    record,
                    qualification_session_token=qualification_session_token,
                    qualification_window=qualification_window,
                    limitations=["host_observation_time_only"],
                )
            )
        elif timestamp is not None:
            result.append(
                _host_record(
                    state or "host_event",
                    timestamp,
                    record,
                    qualification_session_token=qualification_session_token,
                    qualification_window=qualification_window,
                )
            )
    return result


def _causal_events(
    records: Sequence[Mapping[str, Any]], alignment: Mapping[str, Any]
) -> list[dict[str, Any]]:
    stage_kinds = {
        "BLE_RX": "dut_ble_rx",
        "PACKET_PARSE": "dut_packet_parse",
        "STATE_PUBLISH": "dut_state_publish",
        "ALERT_TABLE_PUBLISH": "dut_alert_publish",
        "RENDER_REQUEST": "render_request",
        "DISPLAY_COMMIT": "display_commit",
    }
    loss_reasons = _evidence_loss_reasons(
        records,
        sequence_names=("trace_seq",),
        counter_names=("ble_source_losses", "lost_trace_records"),
        terminal_sequence_names=("terminal_trace_seq",),
    )
    result: list[dict[str, Any]] = []
    for record in records:
        stage = str(_first(record, ("stage", "kind", "event")) or "").upper()
        kind = stage_kinds.get(stage, "dut_causal_event")
        result.append(
            _dut_record(
                kind,
                record,
                alignment,
                timestamp_names=(
                    "stage_dut_monotonic_us",
                    "stage_dut_us",
                    "stage_dut_micros",
                    "dut_monotonic_us",
                    "dut_micros",
                    "timestamp_dut_us",
                ),
                outcome=record.get("outcome"),
                evidence_complete=not loss_reasons,
                evidence_loss_reasons=loss_reasons,
            )
        )
    return result


def _display_events(
    records: Sequence[Mapping[str, Any]], alignment: Mapping[str, Any]
) -> list[dict[str, Any]]:
    loss_reasons = _evidence_loss_reasons(
        records,
        sequence_names=("seq", "commit_seq", "display_commit_seq"),
        counter_names=("dropped_commits",),
        terminal_sequence_names=("terminal_seq",),
    )
    result: list[dict[str, Any]] = []
    for record in records:
        render_time = _first(
            record,
            (
                "render_request_dut_us",
                "render_request_dut_micros",
                "render_dut_us",
                "render_start_dut_us",
            ),
        )
        if render_time is not None:
            render_record = {**record, "dut_monotonic_us": render_time}
            result.append(
                _dut_record(
                    "render_request",
                    render_record,
                    alignment,
                    timestamp_names=("dut_monotonic_us",),
                    suffix="render",
                    evidence_complete=not loss_reasons,
                    evidence_loss_reasons=loss_reasons,
                )
            )
        commit_time = _first(
            record,
            (
                "display_commit_dut_us",
                "display_commit_dut_micros",
                "commit_dut_us",
                "commit_dut_monotonic_us",
                "dut_monotonic_us",
            ),
        )
        parsed_commit_time = _integer(commit_time)
        if parsed_commit_time is not None and parsed_commit_time > 0:
            commit_record = {**record, "dut_monotonic_us": commit_time}
            result.append(
                _dut_record(
                    "display_commit",
                    commit_record,
                    alignment,
                    timestamp_names=("dut_monotonic_us",),
                    suffix="commit",
                    evidence_complete=not loss_reasons,
                    evidence_loss_reasons=loss_reasons,
                )
            )
        else:
            pushes = _integer(record.get("pushes"))
            result.append(
                _aligned_record(
                    kind="display_commit",
                    raw_clock="dut_monotonic_us",
                    raw_timestamp=commit_time,
                    source_artifact=str(record["__source_artifact__"]),
                    source_record=record["__source_record__"],
                    causal=_causal_identifiers(record),
                    estimate=None,
                    earliest=None,
                    latest=None,
                    suffix="commit",
                    alignment_status="missing_evidence",
                    evidence_complete=not loss_reasons,
                    evidence_loss_reasons=loss_reasons,
                    limitations=[
                        "no_physical_display_transfer"
                        if pushes == 0
                        else "display_commit_timestamp_unavailable"
                    ],
                )
            )
    return result


def _camera_events(records: Sequence[Mapping[str, Any]], *, evidence_usable: bool = True) -> list[dict[str, Any]]:
    result: list[dict[str, Any]] = []
    for record in records:
        status = str(record.get("status") or "timestamp_error")
        host_capture = _integer(record.get("host_capture_ns"))
        duration = _integer(record.get("duration_ns"))
        mapped = (
            evidence_usable
            and host_capture is not None
            and host_capture >= 0
            and duration is not None
            and duration > 0
        )
        limitations: list[str] = []
        if not evidence_usable:
            limitations.append("camera_evidence_unverified")
        if host_capture is None or host_capture < 0:
            limitations.append("camera_clock_conversion_unavailable")
        if duration is None or duration <= 0:
            limitations.append("camera_frame_duration_unavailable")
        source_timestamp = {
            "value": record.get("source_pts_value"),
            "timescale": record.get("source_pts_timescale"),
        }
        kind = "camera_frame" if status == "written" else "camera_drop"
        result.append(
            _aligned_record(
                kind=kind,
                raw_clock=str(record.get("source_clock") or "camera_source_pts"),
                raw_timestamp=source_timestamp,
                source_artifact=str(record["__source_artifact__"]),
                source_record=record["__source_record__"],
                causal=_causal_identifiers(record),
                estimate=host_capture if mapped else None,
                earliest=host_capture if mapped else None,
                latest=(host_capture + duration) if mapped else None,
                alignment_status="mapped" if mapped else "missing_evidence",
                limitations=limitations,
                frame_seq=record.get("frame_seq"),
                source_pts_value=record.get("source_pts_value"),
                source_pts_timescale=record.get("source_pts_timescale"),
                callback_host_ns=record.get("callback_host_ns"),
                video_pts_value=record.get("video_pts_value"),
                video_pts_timescale=record.get("video_pts_timescale"),
                duration_ns=record.get("duration_ns"),
                status=status,
                drop_reason=record.get("drop_reason"),
                source_clock=record.get("source_clock"),
            )
        )
    return result


def _metric_events(
    records: Sequence[Mapping[str, Any]], alignment: Mapping[str, Any], kind: str
) -> list[dict[str, Any]]:
    return [
        _dut_record(
            kind,
            record,
            alignment,
            timestamp_names=(
                "dut_monotonic_us",
                "dut_micros",
                "dutMicros",
                "sample_dut_micros",
                "timestamp_dut_us",
                "stage_dut_monotonic_us",
            ),
        )
        for record in records
    ]


def _metrics_for_session(
    records: Sequence[Mapping[str, Any]],
    causal: Sequence[Mapping[str, Any]],
    host: Sequence[Mapping[str, Any]],
    session_token: str,
) -> list[dict[str, Any]]:
    boundaries: dict[str, tuple[Any, int]] = {}
    for record in causal:
        stage = str(_first(record, ("stage", "kind", "event")) or "").upper()
        segment = _causal_identifiers(record).get("clock_segment")
        dut_us = _integer(_first(record, ("stage_dut_micros", "stage_dut_monotonic_us", "stage_dut_us", "dut_monotonic_us", "dut_micros", "timestamp_dut_us")))
        if stage in {"SESSION_START", "SESSION_END"} and segment is not None and dut_us is not None:
            boundaries[stage] = (segment, dut_us)
    if len(boundaries) < 2:
        for record in host:
            payload = _json_protocol_payload(record.get("line"))
            if payload is None or _record_qualification_session_token(payload) != session_token:
                continue
            state = str(payload.get("state") or "").lower()
            if state == "running":
                boundary, names = "SESSION_START", ("startedAtDutMicros", "started_at_dut_micros")
            elif state in {"finalizing", "done", "error"}:
                boundary, names = "SESSION_END", ("dutMicros", "dut_micros")
            else:
                continue
            dut_us = _integer(_first(payload, names))
            raw_segment = _first(payload, ("clockSegment", "clock_segment"))
            segment = (
                int(str(raw_segment), 16)
                if re.fullmatch(r"[0-9a-fA-F]{16}", str(raw_segment))
                else _causal_identifiers(payload).get("clock_segment")
            )
            if segment is not None and dut_us is not None:
                boundaries.setdefault(boundary, (segment, dut_us))
    start, end = boundaries.get("SESSION_START"), boundaries.get("SESSION_END")
    if start is None or end is None or start[0] != end[0]:
        return []
    return [
        dict(record)
        for record in records
        if _causal_identifiers(record).get("clock_segment") == start[0]
        and (timestamp := _dut_timestamp(record, ("dutMicros", "dut_micros"))[1]) is not None
        and start[1] <= timestamp <= end[1]
    ]


def intervals_overlap(left: Mapping[str, Any], right: Mapping[str, Any]) -> bool:
    left_start = _integer(left.get("host_earliest_ns"))
    left_end = _integer(left.get("host_latest_ns"))
    right_start = _integer(right.get("host_earliest_ns"))
    right_end = _integer(right.get("host_latest_ns"))
    return (
        left_start is not None
        and left_end is not None
        and right_start is not None
        and right_end is not None
        and left_start <= right_end
        and right_start <= left_end
    )


def _association_record(
    relation: str,
    source: Mapping[str, Any],
    status: str,
    candidates: Sequence[Mapping[str, Any]],
    reason: str,
    sequence: int,
) -> dict[str, Any]:
    estimate = source.get("host_estimate_ns")
    return _aligned_record(
        kind="causal_association",
        raw_clock="derived",
        raw_timestamp=None,
        source_artifact="derived:aligned_timeline",
        source_record=sequence,
        causal=source.get("causal_identifiers", {}),
        estimate=estimate if isinstance(estimate, int) else None,
        earliest=source.get("host_earliest_ns") if isinstance(source.get("host_earliest_ns"), int) else None,
        latest=source.get("host_latest_ns") if isinstance(source.get("host_latest_ns"), int) else None,
        suffix=f"{relation}:{source.get('record_id')}",
        relation=relation,
        association_status=status,
        from_record_id=source.get("record_id"),
        candidate_record_ids=[item.get("record_id") for item in candidates],
        reason=reason,
    )


def _identity_value(record: Mapping[str, Any], name: str) -> Any:
    causal = record.get("causal_identifiers")
    return causal.get(name) if isinstance(causal, Mapping) else None


def _relation_eligible(record: Mapping[str, Any]) -> bool:
    """Exclude known-unscored host rows while retaining indeterminate rows to abstain."""
    return record.get("qualification_scope") != "outside_current_session"


def _generic_relation(
    records: Sequence[Mapping[str, Any]],
    source_kind: str,
    target_kinds: set[str],
    relation: str,
    keys: Sequence[str],
    start_sequence: int,
) -> list[dict[str, Any]]:
    sources = [
        item
        for item in records
        if item.get("kind") == source_kind and _relation_eligible(item)
    ]
    targets = [
        item
        for item in records
        if item.get("kind") in target_kinds and _relation_eligible(item)
    ]
    scoped_keys = tuple(keys)
    if "qualification_session_token" not in scoped_keys and any(
        _identity_value(item, "qualification_session_token") is not None
        for item in (*sources, *targets)
    ):
        scoped_keys = ("qualification_session_token", *scoped_keys)
    target_evidence_incomplete = _records_have_evidence_loss(targets)
    associations: list[dict[str, Any]] = []
    for source in sources:
        values = [_identity_value(source, key) for key in scoped_keys]
        complete_targets = [
            target
            for target in targets
            if all(_identity_value(target, key) is not None for key in scoped_keys)
        ]
        incomplete_targets = [target for target in targets if target not in complete_targets]
        if any(value is None for value in values):
            status, candidates, reason = "missing_evidence", [], "source_identity_incomplete"
        elif not targets:
            status, candidates, reason = "missing_evidence", [], "target_artifact_absent"
        else:
            candidates = [
                target
                for target in complete_targets
                if all(
                    _identity_value(target, key) == value
                    for key, value in zip(scoped_keys, values)
                )
            ]
            compatible_incomplete = [
                target
                for target in incomplete_targets
                if all(
                    _identity_value(target, key) is None
                    or _identity_value(target, key) == value
                    for key, value in zip(scoped_keys, values)
                )
            ]
            if compatible_incomplete:
                candidates.extend(compatible_incomplete)
                status, reason = "missing_evidence", "compatible_target_identity_incomplete"
            elif len(candidates) == 1:
                status, reason = "matched", "identity_match"
            elif not candidates:
                status = "missing_evidence" if target_evidence_incomplete else "no_match"
                reason = (
                    "target_evidence_loss_prevents_absence_claim"
                    if target_evidence_incomplete
                    else "identity_not_observed"
                )
            else:
                status, reason = "ambiguous", "identity_not_unique"
        associations.append(
            _association_record(
                relation,
                source,
                status,
                candidates,
                reason,
                start_sequence + len(associations),
            )
        )
    return associations


def _packet_relations(
    records: Sequence[Mapping[str, Any]], start_sequence: int
) -> list[dict[str, Any]]:
    sources = [
        item
        for item in records
        if item.get("kind") == "notification_accepted" and _relation_eligible(item)
    ]
    targets = [
        item
        for item in records
        if item.get("kind") == "dut_ble_rx" and _relation_eligible(item)
    ]
    target_evidence_incomplete = _records_have_evidence_loss(targets)
    associations: list[dict[str, Any]] = []
    grouped_sources: dict[tuple[Any, ...], list[Mapping[str, Any]]] = {}
    grouped_targets: dict[tuple[Any, ...], list[Mapping[str, Any]]] = {}
    incomplete_sources: list[Mapping[str, Any]] = []
    incomplete_targets: list[Mapping[str, Any]] = []
    identity_names = ("characteristic", "payload_length", "payload_sha256")
    if any(
        _identity_value(item, "qualification_session_token") is not None
        for item in (*sources, *targets)
    ):
        identity_names = (*identity_names, "qualification_session_token")
    for source in sources:
        key = tuple(_identity_value(source, name) for name in identity_names)
        if any(value is None for value in key):
            incomplete_sources.append(source)
        else:
            grouped_sources.setdefault(key, []).append(source)
    for target in targets:
        key = tuple(_identity_value(target, name) for name in identity_names)
        if any(value is None for value in key):
            incomplete_targets.append(target)
        else:
            grouped_targets.setdefault(key, []).append(target)

    for source in incomplete_sources:
        associations.append(
            _association_record(
                "notification_accepted_to_dut_ble_rx",
                source,
                "missing_evidence",
                [],
                "collision_resistant_packet_identity_incomplete",
                start_sequence + len(associations),
            )
        )
    for key, source_group in grouped_sources.items():
        target_group = grouped_targets.get(key, [])
        compatible_incomplete = [
            target
            for target in incomplete_targets
            if all(
                _identity_value(target, name) is None
                or _identity_value(target, name) == value
                for name, value in zip(identity_names, key)
            )
        ]
        source_ordered = sorted(
            source_group,
            key=lambda item: (
                _identity_value(item, "global_tx_sequence") is None,
                _identity_value(item, "global_tx_sequence") or 0,
                item["record_id"],
            ),
        )
        target_ordered = sorted(
            target_group,
            key=lambda item: (
                _identity_value(item, "rx_first_seq") is None,
                _identity_value(item, "rx_first_seq") or 0,
                item["record_id"],
            ),
        )
        if compatible_incomplete:
            possible_targets = [*target_ordered, *compatible_incomplete]
            for source in source_ordered:
                associations.append(
                    _association_record(
                        "notification_accepted_to_dut_ble_rx",
                        source,
                        "missing_evidence",
                        possible_targets,
                        "compatible_target_packet_identity_incomplete",
                        start_sequence + len(associations),
                    )
                )
        elif target_ordered and len(source_ordered) == len(target_ordered):
            repeated = len(source_ordered) > 1
            source_order_values = [
                _integer(_identity_value(item, "global_tx_sequence")) for item in source_ordered
            ]
            target_order_values = [
                _integer(_identity_value(item, "rx_first_seq")) for item in target_ordered
            ]
            complete_unique_order = (
                all(value is not None for value in source_order_values)
                and len(set(source_order_values)) == len(source_order_values)
                and all(value is not None for value in target_order_values)
                and len(set(target_order_values)) == len(target_order_values)
            )
            if repeated and (target_evidence_incomplete or not complete_unique_order):
                reason = (
                    "packet_evidence_loss_prevents_order_match"
                    if target_evidence_incomplete
                    else "repeated_packet_order_identity_incomplete"
                )
                for source in source_ordered:
                    associations.append(
                        _association_record(
                            "notification_accepted_to_dut_ble_rx",
                            source,
                            "missing_evidence",
                            target_ordered,
                            reason,
                            start_sequence + len(associations),
                        )
                    )
            else:
                for source, target in zip(source_ordered, target_ordered):
                    associations.append(
                        _association_record(
                            "notification_accepted_to_dut_ble_rx",
                            source,
                            "matched",
                            [target],
                            "identity_and_order_match" if repeated else "collision_resistant_packet_identity_match",
                            start_sequence + len(associations),
                        )
                    )
        elif target_ordered:
            for source in source_ordered:
                associations.append(
                    _association_record(
                        "notification_accepted_to_dut_ble_rx",
                        source,
                        "missing_evidence" if target_evidence_incomplete else "ambiguous",
                        target_ordered,
                        (
                            "packet_evidence_loss_prevents_unique_subsequence"
                            if target_evidence_incomplete
                            else "ordered_repeated_packet_loss_prevents_unique_subsequence"
                        ),
                        start_sequence + len(associations),
                    )
                )
        else:
            for source in source_ordered:
                status = "missing_evidence" if not targets or target_evidence_incomplete else "no_match"
                reason = (
                    "target_artifact_absent"
                    if not targets
                    else (
                        "target_evidence_loss_prevents_absence_claim"
                        if target_evidence_incomplete
                        else "packet_identity_not_observed"
                    )
                )
                associations.append(
                    _association_record(
                        "notification_accepted_to_dut_ble_rx",
                        source,
                        status,
                        [],
                        reason,
                        start_sequence + len(associations),
                    )
                )
    return associations


def _rx_parse_relations(
    records: Sequence[Mapping[str, Any]], start_sequence: int
) -> list[dict[str, Any]]:
    receives = [
        item
        for item in records
        if item.get("kind") == "dut_ble_rx" and _relation_eligible(item)
    ]
    parses = [
        item
        for item in records
        if item.get("kind") == "dut_packet_parse" and _relation_eligible(item)
    ]
    session_scoped = any(
        _identity_value(item, "qualification_session_token") is not None
        for item in (*receives, *parses)
    )
    parse_evidence_incomplete = _records_have_evidence_loss(parses)
    associations: list[dict[str, Any]] = []
    for receive in receives:
        segment = _identity_value(receive, "clock_segment")
        session = _identity_value(receive, "ble_session_generation")
        qualification_session = _identity_value(receive, "qualification_session_token")
        rx_first = _integer(_identity_value(receive, "rx_first_seq"))
        rx_last = _integer(_identity_value(receive, "rx_last_seq"))
        if (
            segment is None
            or session is None
            or (session_scoped and qualification_session is None)
            or rx_first is None
            or rx_last is None
            or rx_first > rx_last
        ):
            status, candidates, reason = (
                "missing_evidence",
                [],
                "receive_sequence_identity_incomplete_or_invalid",
            )
        elif not parses:
            status, candidates, reason = "missing_evidence", [], "parse_artifact_absent"
        else:
            candidates = []
            compatible_incomplete = []
            for parse in parses:
                parse_first = _integer(_identity_value(parse, "rx_first_seq"))
                parse_last = _integer(_identity_value(parse, "rx_last_seq"))
                parse_segment = _identity_value(parse, "clock_segment")
                parse_session = _identity_value(parse, "ble_session_generation")
                parse_qualification_session = _identity_value(
                    parse, "qualification_session_token"
                )
                identity_complete = (
                    parse_segment is not None
                    and parse_session is not None
                    and (not session_scoped or parse_qualification_session is not None)
                    and parse_first is not None
                    and parse_last is not None
                )
                compatible = (
                    parse_segment in {None, segment}
                    and parse_session in {None, session}
                    and (
                        not session_scoped
                        or parse_qualification_session in {None, qualification_session}
                    )
                    and (parse_first is None or parse_first <= rx_first)
                    and (parse_last is None or rx_last <= parse_last)
                )
                if identity_complete and compatible:
                    candidates.append(parse)
                elif not identity_complete and compatible:
                    compatible_incomplete.append(parse)
            if compatible_incomplete:
                candidates.extend(compatible_incomplete)
                status, reason = "missing_evidence", "compatible_parse_identity_incomplete"
            elif len(candidates) == 1:
                status, reason = "matched", "ble_session_and_inclusive_rx_range"
            elif not candidates:
                status = "missing_evidence" if parse_evidence_incomplete else "no_match"
                reason = (
                    "parse_evidence_loss_prevents_absence_claim"
                    if parse_evidence_incomplete
                    else "receive_not_consumed_by_parse"
                )
            else:
                status, reason = "ambiguous", "overlapping_parse_rx_ranges"
        associations.append(
            _association_record(
                "dut_ble_rx_to_packet_parse",
                receive,
                status,
                candidates,
                reason,
                start_sequence + len(associations),
            )
        )
    return associations


def _commit_frame_relations(
    records: Sequence[Mapping[str, Any]], start_sequence: int
) -> list[dict[str, Any]]:
    commits = [item for item in records if item.get("kind") == "display_commit"]
    frames = [
        item
        for item in records
        if item.get("kind") == "camera_frame" and item.get("status") == "written"
    ]
    drops = [item for item in records if item.get("kind") == "camera_drop"]
    unmapped_camera = [
        item
        for item in (*frames, *drops)
        if item.get("host_earliest_ns") is None or item.get("host_latest_ns") is None
    ]
    unverified_camera = any(
        "camera_evidence_unverified" in item.get("limitations", ())
        for item in (*frames, *drops)
    )
    associations: list[dict[str, Any]] = []
    for commit in commits:
        dropped_overlap = not (unverified_camera or unmapped_camera) and any(
            intervals_overlap(commit, drop) for drop in drops
        )
        if commit.get("host_earliest_ns") is None or commit.get("host_latest_ns") is None:
            status, candidates, reason = "missing_evidence", [], "commit_clock_mapping_unavailable"
        elif unverified_camera:
            status, candidates, reason = "missing_evidence", [], "camera_timing_unverified"
        elif unmapped_camera:
            mapped_candidates = [frame for frame in frames if intervals_overlap(commit, frame)]
            candidates = mapped_candidates
            status = "missing_evidence"
            reason = (
                "unmapped_camera_sample_prevents_unique_frame_evidence"
                if mapped_candidates
                else "unmapped_camera_sample_prevents_frame_absence_claim"
            )
        elif not frames:
            if dropped_overlap:
                status, candidates, reason = "missing_evidence", [], "only_dropped_frame_intervals_overlap"
            elif drops:
                status, candidates, reason = "no_match", [], "only_nonoverlapping_dropped_frames_recorded"
            else:
                status, candidates, reason = (
                    "missing_evidence",
                    [],
                    "camera_evidence_absent",
                )
        else:
            candidates = [frame for frame in frames if intervals_overlap(commit, frame)]
            if dropped_overlap and candidates:
                status, reason = "missing_evidence", "overlapping_camera_drop_prevents_unique_frame_evidence"
            elif len(candidates) == 1:
                status, reason = "matched", "uncertainty_intervals_overlap"
            elif not candidates:
                status = "missing_evidence" if dropped_overlap else "no_match"
                reason = "only_dropped_frame_intervals_overlap" if dropped_overlap else "no_overlapping_frame_interval"
            else:
                status, reason = "ambiguous", "multiple_overlapping_frame_intervals"
        associations.append(
            _association_record(
                "display_commit_to_camera_frame",
                commit,
                status,
                candidates,
                reason,
                start_sequence + len(associations),
            )
        )
    return associations


def correlate_timeline(records: Sequence[Mapping[str, Any]]) -> list[dict[str, Any]]:
    """Emit explicit identity-first causal associations; never nearest-by-time."""
    associations: list[dict[str, Any]] = []
    relations = (
        (
            "replay_deadline",
            {"notification_requested"},
            "replay_deadline_to_notification_requested",
            ("stimulus_sequence", "emission_ordinal"),
        ),
        (
            "notification_requested",
            {"notification_accepted"},
            "notification_requested_to_accepted",
            ("global_tx_sequence",),
        ),
        (
            "dut_packet_parse",
            {"dut_state_publish", "dut_alert_publish"},
            "packet_parse_to_revision",
            ("clock_segment", "event_seq"),
        ),
        (
            "dut_state_publish",
            {"render_request"},
            "state_revision_to_render",
            ("clock_segment", "state_revision"),
        ),
        (
            "dut_alert_publish",
            {"render_request"},
            "alert_revision_to_render",
            ("clock_segment", "alert_revision"),
        ),
        (
            "render_request",
            {"display_commit"},
            "render_to_display_commit",
            ("clock_segment", "commit_seq"),
        ),
    )
    for source_kind, target_kinds, relation, keys in relations:
        additions = _generic_relation(
            records,
            source_kind,
            target_kinds,
            relation,
            keys,
            len(associations) + 1,
        )
        associations.extend(additions)
    associations.extend(_packet_relations(records, len(associations) + 1))
    associations.extend(_rx_parse_relations(records, len(associations) + 1))
    associations.extend(_commit_frame_relations(records, len(associations) + 1))
    return associations


def build_aligned_timeline(
    clock_alignment: Mapping[str, Any],
    *,
    bench_timeline: Path | Sequence[Mapping[str, Any]] | None = None,
    causal_trace: Path | Sequence[Mapping[str, Any]] | None = None,
    display_commits: Path | Sequence[Mapping[str, Any]] | None = None,
    camera_sidecar: Path | Sequence[Mapping[str, Any]] | None = None,
    perf_csv: Path | Sequence[Mapping[str, Any]] | None = None,
    encounter_csv: Path | Sequence[Mapping[str, Any]] | None = None,
    camera_verified: bool | None = None,
) -> list[dict[str, Any]]:
    """Align known artifacts while tolerating future additive schema columns."""
    host = _records(bench_timeline, "bench_timeline.ndjson")
    causal = _records(causal_trace, "causal_trace.csv")
    commits = _records(display_commits, "display_commits.csv")
    camera_is_path = isinstance(camera_sidecar, Path)
    camera_valid = not camera_is_path
    try:
        camera = _records(camera_sidecar, "camera/frame_timing.ndjson")
        if camera_is_path:
            validate_frame_sidecar([{key: value for key, value in row.items() if not key.startswith("__")} for row in camera])
            camera_valid = True
    except (OSError, TypeError, ValueError):
        camera = []
    perf = _records(perf_csv, "perf.csv")
    encounters = _records(encounter_csv, "encounters.csv")

    qualification_session_token = _current_qualification_session_token(host)
    if qualification_session_token is None:
        qualification_session_token = _latest_session_start_token(causal)
    if qualification_session_token is not None:
        causal = _records_for_qualification_session(causal, qualification_session_token)
        commits = _records_for_qualification_session(
            commits,
            qualification_session_token,
            retain_unscoped_tail_context=True,
        )
        encounters = _records_for_qualification_session(encounters, qualification_session_token)
        perf = _metrics_for_session(perf, causal, host, qualification_session_token)
    qualification_window = _qualification_host_window(
        host,
        causal,
        clock_alignment,
        qualification_session_token,
    )

    aligned: list[dict[str, Any]] = []
    aligned.extend(
        _host_events(
            host,
            qualification_session_token=qualification_session_token,
            qualification_window=qualification_window,
        )
    )
    aligned.extend(_causal_events(causal, clock_alignment))
    aligned.extend(_display_events(commits, clock_alignment))
    aligned.extend(
        _camera_events(
            camera,
            evidence_usable=camera_valid
            and (camera_verified is True if camera_is_path else camera_verified is not False),
        )
    )
    aligned.extend(_metric_events(perf, clock_alignment, "metric_sample"))
    aligned.extend(_metric_events(encounters, clock_alignment, "encounter_event"))
    aligned.extend(correlate_timeline(aligned))
    return sorted(
        aligned,
        key=lambda item: (
            item.get("host_estimate_ns") is None,
            item.get("host_estimate_ns") or 0,
            str(item.get("source_artifact")),
            str(item.get("source_record")),
            str(item.get("kind")),
            str(item.get("record_id")),
        ),
    )


def write_aligned_timeline(path: Path, records: Iterable[Mapping[str, Any]]) -> int:
    """Exclusively create aligned NDJSON; original and prior derived evidence wins."""
    count = 0
    with path.open("x", encoding="utf-8") as handle:
        for record in records:
            handle.write(json.dumps(record, sort_keys=True, separators=(",", ":")) + "\n")
            count += 1
    return count


def _first_existing(run_dir: Path, patterns: Sequence[str]) -> Path | None:
    for pattern in patterns:
        candidates = sorted(path for path in run_dir.rglob(pattern) if path.is_file())
        if candidates:
            return candidates[0]
    return None


def _camera_artifact_path(run_dir: Path, value: Any) -> Path | None:
    candidate = Path(str(value or ""))
    roots = (Path("/"),) if candidate.is_absolute() else (run_dir, run_dir / "camera")
    return next((root / candidate for root in roots if (root / candidate).is_file()), None)


def _camera_sidecar_from_result(run_dir: Path, camera_result: Mapping[str, Any] | None) -> Path | None:
    if camera_result is not None:
        value = _first(
            camera_result,
            (
                "frame_timing",
                "timing_sidecar",
                "frame_timing_sidecar",
                "timing_sidecar_path",
            ),
        )
        if value is not None:
            candidate = _camera_artifact_path(run_dir, value)
            if candidate is not None:
                return candidate
    return _first_existing(
        run_dir,
        ("frame_timing.ndjson", "*frame*timing*.ndjson", "*timing*sidecar*.ndjson"),
    )


def _camera_verification_acceptable(run_dir: Path, camera_result: Mapping[str, Any] | None) -> bool:
    verification = camera_result.get("video_timing_verification_result") if camera_result else None
    if not isinstance(verification, Mapping) and isinstance(camera_result, Mapping):
        path = _camera_artifact_path(run_dir, camera_result.get("video_timing_verification"))
        try:
            verification = json.loads(path.read_text(encoding="utf-8")) if path else None
        except (OSError, json.JSONDecodeError):
            pass
    return isinstance(verification, Mapping) and verification.get("status") == "verified"


def generate_alignment_artifacts(
    run_dir: Path,
    *,
    perf_csv: Path | None = None,
    encounter_csv: Path | None = None,
    camera_result: Mapping[str, Any] | None = None,
) -> dict[str, Any]:
    """Derive and exclusively publish both offline alignment artifacts.

    This is intentionally a best-evidence collector, not a gate.  Missing input
    artifacts produce fewer records and explicit association limitations.
    """
    bench_timeline = run_dir / "bench_timeline.ndjson"
    if not bench_timeline.is_file():
        raise FileNotFoundError(f"bench timeline is unavailable: {bench_timeline}")
    host_records = read_ndjson_records(bench_timeline)
    clock_alignment = fit_clock_alignment(extract_qsync_exchanges(host_records))

    causal_trace = _first_existing(run_dir, ("*causal*trace*.csv",))
    display_commits = _first_existing(run_dir, ("*display*commit*.csv",))
    camera_sidecar = _camera_sidecar_from_result(run_dir, camera_result)
    records = build_aligned_timeline(
        clock_alignment,
        bench_timeline=host_records,
        causal_trace=causal_trace,
        display_commits=display_commits,
        camera_sidecar=camera_sidecar,
        perf_csv=perf_csv,
        encounter_csv=encounter_csv,
        camera_verified=_camera_verification_acceptable(run_dir, camera_result),
    )

    clock_path = run_dir / CLOCK_ALIGNMENT_FILENAME
    timeline_path = run_dir / ALIGNED_TIMELINE_FILENAME
    with clock_path.open("x", encoding="utf-8") as handle:
        json.dump(clock_alignment, handle, sort_keys=True, separators=(",", ":"))
        handle.write("\n")
    try:
        record_count = write_aligned_timeline(timeline_path, records)
    except BaseException:
        # Do not delete the immutable clock artifact.  Its presence accurately
        # reports that publication was partial and avoids hiding a collision.
        raise

    segment_summaries = [
        {
            "mapping_id": segment.get("mapping_id"),
            "fit_type": segment.get("fit_type"),
            "maximum_uncertainty_ns": segment.get("maximum_uncertainty_ns"),
            "uncertainty_smaller_than_2_5_ms": segment.get(
                "uncertainty_smaller_than_2_5_ms", False
            ),
        }
        for segment in clock_alignment.get("segments", [])
        if isinstance(segment, Mapping)
    ]
    return {
        "schema_version": ALIGNED_TIMELINE_SCHEMA_VERSION,
        "kind": "bench_alignment_artifacts",
        "clock_alignment": {
            "path": clock_path.name,
            "exchange_count": clock_alignment.get("raw_exchange_count", 0),
            "segments": segment_summaries,
        },
        "aligned_timeline": {"path": timeline_path.name, "record_count": record_count},
        "sources": {
            "bench_timeline": bench_timeline.name,
            "causal_trace": None if causal_trace is None else causal_trace.relative_to(run_dir).as_posix(),
            "display_commits": None if display_commits is None else display_commits.relative_to(run_dir).as_posix(),
            "camera_sidecar": None if camera_sidecar is None else camera_sidecar.relative_to(run_dir).as_posix(),
            "perf_csv": None if perf_csv is None else perf_csv.name,
            "encounter_csv": None if encounter_csv is None else encounter_csv.name,
        },
    }

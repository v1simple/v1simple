#!/usr/bin/env python3
"""Expectation-blind OpenAI observation of one complete display frame.

Only a freshly encoded PNG and fixed observation instructions leave the host.
Replay input, timing, expected values, local paths and raw API responses are not
sent or retained by this module. Calls are single-attempt and require an
explicit opt-in from encounter_check.py.
"""
from __future__ import annotations

import base64
from contextlib import contextmanager
import hashlib
import json
import os
import struct
import urllib.error
import urllib.request
import zlib


API_URL = "https://api.openai.com/v1/responses"
MODEL = "gpt-6-astra"
MAX_RESPONSE_BYTES = 1024 * 1024
SUPPORTED_SIZE = (1280, 720)
FIELDS = ("counter_glyph", "primary_frequency", "active_bands", "main_arrows",
          "main_bars", "secondary", "muted_badge")
ALP_FREQUENCY_LABELS = ("PL3", "drgEYE", "truSPd", "PL2", "ULtrLt", "StLr", "LALLY", "StLtH")
PROMPT = (
    "Inspect only the attached complete V1Simple display photograph. Do not infer expected "
    "firmware behavior. The small orange seven-segment glyph near x=290 is the counter; the "
    "orange labels near x=330 are active bands (L, Ka, Ku, K, or X); the large center text is "
    "a numeric frequency, --.---, LASER, or an ALP gun abbreviation; the "
    "large right glyph is direction, where apex up means front, a horizontal bar means side, "
    "and apex down means rear; six cells can show main strength; secondary cards are the "
    "smaller alert panels; MUTED is a separate badge. Report only literal visible content. "
    "If a required region cannot be read literally, add that field to issues instead of guessing."
)

SCHEMA = {
    "type": "object",
    "additionalProperties": False,
    "required": [*FIELDS, "issues"],
    "properties": {
        "counter_glyph": {"type": "string", "enum": ["", *"0123456789", "A", "L", "l"]},
        "primary_frequency": {
            "anyOf": [
                {"type": "string", "pattern": r"^(?:|[0-9]{1,2}\.[0-9]{3}|--\.---|LASER)$"},
                {"type": "string", "enum": list(ALP_FREQUENCY_LABELS)},
            ],
        },
        "active_bands": {
            "type": "array",
            "items": {"type": "string", "enum": ["L", "Ka", "Ku", "K", "X"]},
        },
        "main_arrows": {
            "type": "array",
            "items": {"type": "string", "enum": ["front", "side", "rear"]},
        },
        "main_bars": {"type": "integer", "minimum": 0, "maximum": 6},
        "secondary": {
            "type": "array",
            "maxItems": 2,
            "items": {
                "type": "object",
                "additionalProperties": False,
                "required": ["band", "frequency", "direction", "bars"],
                "properties": {
                    "band": {"type": "string", "enum": ["X", "K", "Ka"]},
                    "frequency": {
                        "type": "string",
                        "pattern": r"^[0-9]{1,2}\.[0-9]{3}$",
                    },
                    "direction": {
                        "type": "string",
                        "enum": ["front", "side", "rear"],
                    },
                    "bars": {"type": "integer", "minimum": 0, "maximum": 6},
                },
            },
        },
        "muted_badge": {"type": "boolean"},
        "issues": {
            "type": "array",
            "items": {
                "type": "object",
                "additionalProperties": False,
                "required": ["field", "reason"],
                "properties": {
                    "field": {"type": "string", "enum": list(FIELDS)},
                    "reason": {
                        "type": "string",
                        "enum": ["unreadable", "ambiguous", "malformed"],
                    },
                },
            },
        },
    },
}

_upload_authorized = False


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate JSON object key")
        result[key] = value
    return result


def encode_png(rgb: bytes, width: int, height: int) -> bytes:
    """Encode only RGB pixels; do not transmit source-file metadata."""
    if (type(width) is not int or type(height) is not int or width < 1 or height < 1
            or width > 4096 or height > 2160 or len(rgb) != width * height * 3):
        raise ValueError("invalid complete RGB frame")

    def chunk(kind: bytes, data: bytes) -> bytes:
        checksum = zlib.crc32(kind + data)
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", checksum)

    stride = width * 3
    scanlines = b"".join(b"\0" + rgb[y * stride:(y + 1) * stride] for y in range(height))
    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(scanlines))
            + chunk(b"IEND", b""))


def build_request(png: bytes) -> dict:
    encoded = base64.b64encode(png).decode("ascii")
    return {
        "model": MODEL,
        "store": False,
        "tools": [],
        "input": [{
            "role": "user",
            "content": [
                {"type": "input_text", "text": PROMPT},
                {"type": "input_image", "image_url": f"data:image/png;base64,{encoded}",
                 "detail": "original"},
            ],
        }],
        "text": {
            "format": {
                "type": "json_schema",
                "name": "v1simple_display_observation",
                "strict": True,
                "schema": SCHEMA,
            },
        },
        "reasoning": {"effort": "high"},
        "max_output_tokens": 16384,
    }


def prepare_reader(_cache_dir=None, *, allow_upload: bool = False) -> dict:
    global _upload_authorized
    if not allow_upload:
        raise RuntimeError("OpenAI frame upload requires explicit command-line consent")
    key = os.environ.get("OPENAI_API_KEY", "")
    if len(key) < 20 or any(character.isspace() for character in key):
        raise RuntimeError("OPENAI_API_KEY is missing or malformed")
    _upload_authorized = True
    return {
        "kind": "openai_expectation_blind_frame_observer",
        "model": MODEL,
        "prompt_sha256": hashlib.sha256(PROMPT.encode()).hexdigest(),
        "schema_sha256": hashlib.sha256(
            json.dumps(SCHEMA, sort_keys=True, separators=(",", ":")).encode()
        ).hexdigest(),
        "transmitted": "canonical PNG pixels and fixed observation instructions only",
        "response_storage_requested": False,
        "retry_count": 0,
    }


@contextmanager
def analysis_session():
    global _upload_authorized
    try:
        yield
    finally:
        _upload_authorized = False


def _output_text(response: dict) -> tuple[str, str]:
    if (not isinstance(response, dict) or response.get("status") != "completed"
            or not isinstance(response.get("model"), str)
            or not isinstance(response.get("output"), list)):
        raise RuntimeError("OpenAI observation did not complete")
    texts = []
    for item in response.get("output", []):
        if not isinstance(item, dict) or item.get("type") != "message":
            continue
        content_items = item.get("content", [])
        if not isinstance(content_items, list):
            continue
        for content in content_items:
            if isinstance(content, dict) and content.get("type") == "output_text":
                texts.append(content.get("text"))
    if len(texts) != 1 or not isinstance(texts[0], str):
        raise RuntimeError("OpenAI observation returned no single structured output")
    return texts[0], response["model"]


def _validate_literal(value: dict) -> None:
    if not isinstance(value, dict) or set(value) != set(SCHEMA["required"]):
        raise ValueError("invalid OpenAI observation fields")
    if value["counter_glyph"] not in ("", *"0123456789", "A", "L", "l"):
        raise ValueError("invalid counter observation")
    frequency = value["primary_frequency"]
    if not isinstance(frequency, str) or (frequency not in ("", "--.---", "LASER", *ALP_FREQUENCY_LABELS)
            and not (len(frequency) in (5, 6) and frequency[-4] == "."
                     and frequency.replace(".", "").isdigit())):
        raise ValueError("invalid frequency observation")
    for field, allowed in (("active_bands", {"L", "Ka", "Ku", "K", "X"}),
                           ("main_arrows", {"front", "side", "rear"})):
        items = value[field]
        if (not isinstance(items, list) or len(items) != len(set(items))
                or any(item not in allowed for item in items)):
            raise ValueError(f"invalid {field} observation")
    if type(value["main_bars"]) is not int or not 0 <= value["main_bars"] <= 6:
        raise ValueError("invalid main bar observation")
    if type(value["muted_badge"]) is not bool:
        raise ValueError("invalid mute observation")
    cards = value["secondary"]
    if not isinstance(cards, list) or len(cards) > 2:
        raise ValueError("invalid secondary observation")
    for card in cards:
        if (not isinstance(card, dict) or set(card) != {"band", "frequency", "direction", "bars"}
                or card["band"] not in {"X", "K", "Ka"}
                or card["direction"] not in {"front", "side", "rear"}
                or type(card["bars"]) is not int or not 0 <= card["bars"] <= 6
                or not isinstance(card["frequency"], str)
                or not (len(card["frequency"]) in (5, 6) and card["frequency"][-4] == "."
                        and card["frequency"].replace(".", "").isdigit())):
            raise ValueError("invalid secondary card observation")
    issues = value["issues"]
    if not isinstance(issues, list) or any(
            not isinstance(issue, dict) or set(issue) != {"field", "reason"}
            or issue["field"] not in FIELDS
            or issue["reason"] not in {"unreadable", "ambiguous", "malformed"}
            for issue in issues):
        raise ValueError("invalid observation issue")
    if len({issue["field"] for issue in issues}) != len(issues):
        raise ValueError("duplicate observation issue")


def _reading(literal: dict, response_model: str, image_sha256: str) -> dict:
    _validate_literal(literal)
    issues = {issue["field"]: issue["reason"] for issue in literal["issues"]}
    fields = {}
    for name in FIELDS:
        value = literal[name]
        if name in issues:
            state = "ambiguous" if issues[name] == "ambiguous" else "unreadable"
            fields[name] = {"state": state, "value": None,
                            "reason": f"online frame observation marked {issues[name]}"}
        elif name in ("counter_glyph", "primary_frequency") and value == "":
            fields[name] = {"state": "absent", "value": None, "reason": None}
        else:
            fields[name] = {"state": "readable", "value": value, "reason": None}
    return {
        "fields": fields,
        "diagnostics": {
            "observer": "openai",
            "requested_model": MODEL,
            "response_model": response_model,
            "image_sha256": image_sha256,
            "store": False,
        },
    }


def observe(rgb: bytes, width: int, height: int, registration: dict) -> dict:
    del registration
    if not _upload_authorized:
        raise RuntimeError("OpenAI frame observer was not explicitly prepared")
    if (width, height) != SUPPORTED_SIZE:
        raise RuntimeError("OpenAI frame observer is qualified only for 1280x720 complete frames")
    api_key = os.environ.get("OPENAI_API_KEY", "")
    if len(api_key) < 20 or any(character.isspace() for character in api_key):
        raise RuntimeError("OPENAI_API_KEY disappeared or changed format before observation")
    png = encode_png(rgb, width, height)
    request = urllib.request.Request(
        API_URL,
        data=json.dumps(build_request(png), separators=(",", ":")).encode(),
        headers={"Authorization": f"Bearer {api_key}", "Content-Type": "application/json"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(request, timeout=120) as opened:
            raw = opened.read(MAX_RESPONSE_BYTES + 1)
    except urllib.error.HTTPError as exc:
        raise RuntimeError(f"OpenAI observation HTTP status {exc.code}") from None
    except urllib.error.URLError:
        raise RuntimeError("OpenAI observation transport failed") from None
    if len(raw) > MAX_RESPONSE_BYTES:
        raise RuntimeError("OpenAI observation response exceeded size limit")
    try:
        response = json.loads(raw, object_pairs_hook=_unique_object)
        text, response_model = _output_text(response)
        literal = json.loads(text, object_pairs_hook=_unique_object)
    except (UnicodeError, json.JSONDecodeError, ValueError, TypeError) as exc:
        raise RuntimeError(f"OpenAI observation response was invalid: {exc}") from None
    try:
        return _reading(literal, response_model, hashlib.sha256(png).hexdigest())
    except (ValueError, TypeError) as exc:
        raise RuntimeError(f"OpenAI observation response was invalid: {exc}") from None

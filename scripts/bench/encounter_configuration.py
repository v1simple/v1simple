"""Use recorded normal-runtime settings without inferring them from pixels.

CFG snapshots use reset-bounded device uptime and host serial-receive times,
not device setting-change times. An unchanged, nonsaturated revision must surround
the selected observations. Historical recordings remain usable without this proof.
"""
from __future__ import annotations

from bisect import bisect_left, bisect_right
import re

FIELDS = ("bootId", "uptimeMs", "revision", "activeSlot", "stealthEnabled",
          "priorityArrowOnly", "alertPersistenceSeconds")
SETTING_FIELDS = ("stealthEnabled", "priorityArrowOnly", "alertPersistenceSeconds")
UINT32_MAX = 0xFFFFFFFF
CLOCK_ALLOWANCE_NUMERATOR = 99
CLOCK_ALLOWANCE_DENOMINATOR = 100
RESET_ALLOWANCE_NS = 100_000_000


def parse_snapshot(line: str) -> dict | None:
    if not line.startswith("CFG ") and line != "CFG":
        return None
    fields = {}
    for token in line.split()[1:]:
        if token.count("=") != 1:
            raise ValueError("malformed CFG field")
        name, value = token.split("=", 1)
        if name not in FIELDS or name in fields or re.fullmatch(r"0|[1-9][0-9]*", value) is None:
            raise ValueError("unknown, duplicate or noncanonical CFG field")
        fields[name] = int(value)
    if set(fields) != set(FIELDS):
        raise ValueError("incomplete CFG snapshot")
    for key in ("bootId", "uptimeMs", "revision"):
        if not 0 <= fields[key] <= UINT32_MAX:
            raise ValueError("CFG integer exceeds uint32")
    if fields["bootId"] == 0 or fields["revision"] == UINT32_MAX:
        raise ValueError("CFG boot identity is missing or revision is saturated")
    if fields["activeSlot"] not in (0, 1, 2):
        raise ValueError("invalid CFG active slot")
    if any(fields[k] not in (0, 1) for k in ("stealthEnabled", "priorityArrowOnly")):
        raise ValueError("invalid CFG boolean")
    if not 0 <= fields["alertPersistenceSeconds"] <= 5:
        raise ValueError("invalid CFG persistence")
    return fields


def recorded_snapshots(records: list[dict], runtime_identity: dict | None) -> dict:
    snapshots = []
    try:
        if not isinstance(runtime_identity, dict) or type(runtime_identity.get("boot_id")) is not int:
            raise ValueError("recorded runtime identity is unavailable")
        requests = [r for r in records if r.get("event") == "serial_reset_requested"]
        completions = [r for r in records if r.get("event") == "serial_reset_completed"]
        boundaries = [r for r in records if r.get("event") == "serial_boundary_established"]
        if len(requests) != 1 or len(completions) != 1 or len(boundaries) != 1:
            raise ValueError("one explicit reset-to-ready serial anchor is required")
        reset_ns, completed_ns, ready_ns = [r.get("host_monotonic_ns") for r in
                                            (requests[0], completions[0], boundaries[0])]
        if any(type(t) is not int or t <= 0 for t in (reset_ns, completed_ns, ready_ns)) or not reset_ns <= completed_ns <= ready_ns:
            raise ValueError("reset-to-ready timestamps are malformed")
        if boundaries[0].get("runtime_identity") != runtime_identity or not boundaries[0].get("reset_anchored"):
            raise ValueError("fresh normal boot was not established after the explicit reset")
        for record in records:
            if record.get("event") != "serial_receive":
                continue
            line = record.get("line")
            if not isinstance(line, str):
                raise ValueError("serial timeline line is malformed")
            snapshot = parse_snapshot(line)
            if snapshot is None:
                continue
            received = record.get("host_monotonic_ns")
            if type(received) is not int or received <= 0:
                raise ValueError("CFG receive time is unavailable")
            if snapshot["bootId"] != runtime_identity["boot_id"]:
                raise ValueError("CFG identifies a different recorded boot")
            # Uptime zero follows the explicit reset request. Allow 1% clock
            # error plus 100 ms before using uptime as a lower emission bound.
            # Receive time is only an upper bound: buffered old records cannot
            # prove that settings remained stable through a later image.
            emitted_lower_ns = reset_ns + max(0, snapshot["uptimeMs"] * 1_000_000 *
                CLOCK_ALLOWANCE_NUMERATOR // CLOCK_ALLOWANCE_DENOMINATOR - RESET_ALLOWANCE_NS)
            if received < completed_ns or emitted_lower_ns > received:
                raise ValueError("CFG uptime contradicts its reset/receive bounds")
            if snapshots:
                previous = snapshots[-1]
                if received <= previous["received_ns"] or snapshot["uptimeMs"] <= previous["uptimeMs"]:
                    raise ValueError("CFG receive time or device uptime did not increase")
                if snapshot["revision"] < previous["revision"]:
                    raise ValueError("CFG revision decreased")
                if snapshot["revision"] == previous["revision"] and any(
                        snapshot[k] != previous[k] for k in ("activeSlot", *SETTING_FIELDS)):
                    raise ValueError("CFG values changed without a revision")
            snapshots.append({**snapshot, "received_ns": received, "emitted_lower_ns": emitted_lower_ns})
        if len(snapshots) < 2:
            raise ValueError("fewer than two recorded normal-runtime CFG snapshots")
    except ValueError as exc:
        return {"status": "unavailable", "reason": str(exc), "snapshot_count": len(snapshots)}
    return {"status": "available", "snapshots": snapshots, "snapshot_count": len(snapshots),
            "reset_requested_ns": reset_ns}


def configuration_for_samples(recorded: dict | None, samples: list[dict]) -> dict:
    """Bound a static observed configuration; never guess across a revision change."""
    if not recorded or recorded.get("status") != "available":
        return recorded or {"status": "unavailable", "reason": "recorded CFG evidence is absent"}
    snapshots = recorded["snapshots"]
    times = [s["capture_ns"] for s in samples if "capture_ns" in s]
    if not times:
        return {"status": "unavailable", "reason": "no selected capture timestamps"}
    received = [s["received_ns"] for s in snapshots]
    lower = [s["emitted_lower_ns"] for s in snapshots]
    first = bisect_right(received, min(times)) - 1
    last = bisect_left(lower, max(times))
    if first < 0 or last >= len(snapshots) or first == last:
        return {"status": "unavailable", "reason": "CFG observations do not surround selected camera samples",
                "snapshot_count": len(snapshots)}
    before, after = snapshots[first], snapshots[last]
    if before["revision"] != after["revision"]:
        return {"status": "unavailable", "reason": "effective display configuration changed across selected samples",
                "start_revision": before["revision"], "end_revision": after["revision"],
                "snapshot_count": len(snapshots)}
    settings = {k: bool(before[k]) if k != "alertPersistenceSeconds" else before[k] for k in SETTING_FIELDS}
    return {"status": "verified", "basis": "unchanged normal-runtime configuration revision surrounding selected samples, bounded by explicit reset, device uptime and serial receipt",
            "timing_basis": "first serial receipt is before selected images; last device emission lower bound is after them",
            "clock_allowance": {"fraction": 0.01, "reset_allowance_ms": 100},
            "reset_requested_ns": recorded["reset_requested_ns"],
            "coverage": {"start_capture_ns": before["received_ns"], "end_capture_ns": after["emitted_lower_ns"]},
            "revision": before["revision"], "active_slot": before["activeSlot"], "settings": settings,
            "snapshot_count": last - first + 1, "before": before, "after": after}

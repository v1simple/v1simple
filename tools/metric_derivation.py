#!/usr/bin/env python3
"""Shared offline metric derivation helpers."""

from __future__ import annotations

import math
from collections.abc import Mapping, Sequence
from typing import Any


def percentile(values: Sequence[float], pct: float) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    if len(ordered) == 1:
        return float(ordered[0])
    rank = (pct / 100.0) * (len(ordered) - 1)
    lo = int(math.floor(rank))
    hi = int(math.ceil(rank))
    if lo == hi:
        return float(ordered[lo])
    frac = rank - lo
    return float(ordered[lo] + (ordered[hi] - ordered[lo]) * frac)


def clamp_window_bounds(length: int, start_idx: int, end_idx: int) -> tuple[int, int]:
    start = max(0, min(start_idx, length))
    end = max(start, min(end_idx, length))
    return start, end


def window_peak(records: Sequence[Mapping[str, Any]], start_idx: int, end_idx: int, key: str) -> float | int | None:
    start, end = clamp_window_bounds(len(records), start_idx, end_idx)
    values = [record.get(key) for record in records[start:end] if record.get(key) is not None]
    return max(values) if values else None

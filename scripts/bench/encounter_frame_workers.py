"""Bounded independent pixel reads, returned in the original frame order.

Workers receive pixels and fixed camera geometry only. Input expectations,
timestamps, witness retention and report construction remain in the caller.
"""
from __future__ import annotations

from collections import deque
from concurrent.futures import ProcessPoolExecutor
from contextlib import nullcontext
import multiprocessing
from multiprocessing.util import Finalize


_geometry = None
_session = None


def _initialize(cache_dir, width, height, registration, runtime):
    import encounter_reader

    global _geometry, _session
    if encounter_reader.prepare_reader(cache_dir) != runtime:
        raise RuntimeError("frame worker reader runtime differs from parent")
    _geometry = width, height, registration
    _session = encounter_reader.analysis_session()
    _session.__enter__()
    Finalize(None, _session.__exit__, args=(None, None, None), exitpriority=10)


def _observe(pixels, width, height, registration):
    import encounter_reader

    try:
        return encounter_reader.observe(pixels, width, height, registration), None
    except Exception as error:
        return None, f"reader failed: {type(error).__name__}: {error}"


def _read(job):
    import encounter_reader

    index, pixels = job
    reading, error = _observe(pixels, *_geometry)
    failure = getattr(encounter_reader._ocr_session, "failure_reason", None)
    return index, reading, error, failure


def _ordered(executor, frames, limit):
    """Keep at most limit original images/futures pending, including results."""
    pending = deque()
    source = iter(frames)
    exhausted = False
    try:
        while pending or not exhausted:
            while not exhausted and len(pending) < limit:
                try:
                    index, pixels = next(source)
                except StopIteration:
                    exhausted = True
                    break
                pending.append((index, pixels, executor.submit(_read, (index, pixels))))
            if pending:
                index, pixels, future = pending.popleft()
                read_index, reading, error, failure = future.result()
                if read_index != index:
                    raise RuntimeError("frame worker returned a different frame index")
                if failure is not None:
                    # A healthy worker must not conceal another worker's failed
                    # OCR transport. No retry or automatic serial fallback.
                    raise RuntimeError(f"frame worker OCR failed at frame {index}: {failure}")
                yield index, pixels, reading, error
    finally:
        for _, _, future in pending:
            future.cancel()


def observe_frames(frames, width, height, registration, cache_dir, *, workers=1):
    """Read every supplied frame once; serial remains available for comparison."""
    import encounter_reader

    if type(workers) is not int or not 1 <= workers <= 8:
        raise ValueError("frame reader workers must be an integer from 1 to 8")
    try:
        if workers == 1:
            with getattr(encounter_reader, "analysis_session", nullcontext)():
                for index, pixels in frames:
                    reading, error = _observe(pixels, width, height, registration)
                    yield index, pixels, reading, error
            return
        runtime = encounter_reader.prepare_reader(cache_dir)
        with ProcessPoolExecutor(max_workers=workers, mp_context=multiprocessing.get_context("spawn"),
                                 initializer=_initialize,
                                 initargs=(cache_dir, width, height, registration, runtime)) as executor:
            yield from _ordered(executor, frames, workers * 2)
    finally:
        if hasattr(frames, "close"):
            frames.close()

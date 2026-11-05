"""External microphone capture and asset-based acoustic observations.

The microphone is independent of firmware logs and the I2S test double. Known
clips supply a waveform expectation, never a recording that is presumed correct.
No speech service, network upload, playback on the host, or DUT access is used.
"""
from __future__ import annotations

import hashlib
from fractions import Fraction
from functools import lru_cache
import json
import math
import os
from pathlib import Path
import select
import shutil
import subprocess
import time
import wave

import numpy as np

from behavior_state import Incomplete, atomic_json

SAMPLE_RATE = 22050
ANALYSIS_RATE = 11025
HOP = 110
WINDOW = 512
MIN_PHRASE_SCORE = .65
MIN_CLIP_SCORE = .58
MIN_SIGNAL_NOISE_DB = 18.0
MIN_IDENTITY_MARGIN = .04
ASSETS = Path(__file__).resolve().parents[2] / "tools/freq_audio/mulaw"
RECORDER = Path(__file__).with_name("audio_recorder.swift")
MODULE_CACHE = Path(__file__).resolve().parents[2] / "tools/v1replay/.build/audio-module-cache"
CLIPS = {
    "ka": ("band_ka", "tens_34", "digit_7", "tens_00", "dir_ahead"),
    "k": ("band_k", "tens_24", "digit_1", "tens_50", "dir_ahead"),
    "x": ("band_x", "tens_10", "digit_5", "tens_25", "dir_ahead"),
}


def file_sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def prepare_microphone_recorder():
    """Compile and exercise the recorder without opening an input or a DUT."""
    executable = shutil.which("swift")
    if not executable or not RECORDER.is_file():
        raise Incomplete("Native microphone recorder is unavailable")
    MODULE_CACHE.mkdir(parents=True, exist_ok=True)
    result = subprocess.run([executable, "-module-cache-path", str(MODULE_CACHE), str(RECORDER), "--self-test"],
                            capture_output=True, text=True, timeout=90)
    if result.returncode:
        raise Incomplete("Native microphone recorder preflight failed: " + result.stderr[-2000:])


def decode_mulaw(raw):
    """Independent G.711 formula; do not import the firmware lookup table."""
    encoded = np.frombuffer(raw, dtype=np.uint8).astype(np.int32)
    value = (~encoded) & 255
    magnitude = (((value & 15) << 3) + 132) << ((value >> 4) & 7)
    return np.where(value & 128, 132 - magnitude, magnitude - 132).astype(np.int16)


def reference_clips(root, phrase):
    if phrase not in CLIPS:
        raise ValueError("Unknown authored audio phrase")
    paths = [Path(root) / (name + ".mul") for name in CLIPS[phrase]]
    clips = [decode_mulaw(path.read_bytes()) for path in paths]
    if any(not len(clip) for clip in clips):
        raise Incomplete("Empty reference audio asset")
    # Competing words are part of the oracle too; retain their identity with the
    # expected phrase so reanalysis cannot silently change its discrimination.
    return clips, {path.name: file_sha(path) for path in sorted(Path(root).glob("*.mul"))}


def read_recording(path):
    with wave.open(str(path), "rb") as stream:
        if (stream.getnchannels(), stream.getsampwidth(), stream.getframerate(), stream.getcomptype()) != (1, 2, SAMPLE_RATE, "NONE"):
            raise Incomplete("Microphone recording must be mono 16-bit PCM at 22050 Hz")
        frames = stream.getnframes()
        if not SAMPLE_RATE <= frames <= SAMPLE_RATE * 240:
            raise Incomplete("Microphone recording duration is outside the bounded capture")
        raw = stream.readframes(frames)
    if len(raw) != frames * 2:
        raise Incomplete("Microphone recording is truncated")
    return np.frombuffer(raw, dtype="<i2").astype(np.float64) / 32768.0


def rms(samples):
    return float(np.sqrt(np.mean(np.square(samples)))) if len(samples) else 0.0


def features(samples):
    """Time-varying log spectrum; gain and fixed room coloration cancel later."""
    # The short averaging filter bounds high-frequency aliasing before 2:1 decimation.
    samples = np.convolve(samples, np.ones(4) / 4, mode="same")[::2]
    if len(samples) < WINDOW:
        raise Incomplete("Audio segment is too short to identify")
    frames = np.lib.stride_tricks.sliding_window_view(samples, WINDOW)[::HOP]
    power = np.abs(np.fft.rfft(frames * np.hanning(WINDOW), axis=1)) ** 2
    edges = np.geomspace(180, 4700, 25)
    frequency = np.fft.rfftfreq(WINDOW, 1 / ANALYSIS_RATE)
    bands = [power[:, (frequency >= a) & (frequency < b)].mean(axis=1)
             for a, b in zip(edges[:-1], edges[1:])]
    return np.log(np.maximum(np.stack(bands, axis=1), 1e-10))


def correlation(recorded, expected):
    """Normalized sliding correlation, removing each band's local static level."""
    length = len(expected)
    if len(recorded) < length or length < 3:
        return np.empty(0)
    centered = expected - expected.mean(axis=0)
    energy = float(np.square(centered).sum())
    if energy <= 1e-9:
        return np.zeros(len(recorded) - length + 1)
    count = len(recorded) - length + 1
    numerator = np.zeros(count)
    denominator = np.zeros(count)
    for band in range(recorded.shape[1]):
        column = recorded[:, band]
        numerator += np.correlate(column, centered[:, band], "valid")
        sums = np.concatenate(([0.], np.cumsum(column)))
        squares = np.concatenate(([0.], np.cumsum(column * column)))
        total = sums[length:] - sums[:-length]
        denominator += squares[length:] - squares[:-length] - total * total / length
    return numerator / np.sqrt(np.maximum(denominator, 1e-9) * energy)


@lru_cache(maxsize=128)
def reference_features(raw):
    clip = decode_mulaw(raw)
    return clip, features(clip.astype(np.float64) / 32768.0)


def clip_identity(recorded, clips, start):
    """Require each word to beat its alternatives, including similar K/Ka clips."""
    catalog = {path.stem: reference_features(path.read_bytes()) for path in sorted(ASSETS.glob("*.mul"))}
    identities, offset = [], 0.
    for clip in clips:
        names = [name for name, (reference, _) in catalog.items() if np.array_equal(clip, reference)]
        if len(names) != 1:
            raise Incomplete("Spoken reference must identify one shipped audio asset")
        name = names[0]
        scores = {}
        low = max(0, round((start + offset - .12) * ANALYSIS_RATE / HOP))
        for candidate, (_, expected) in catalog.items():
            if candidate.split("_", 1)[0] != name.split("_", 1)[0]:
                continue
            high = min(len(recorded), round((start + offset + .12) * ANALYSIS_RATE / HOP) + len(expected))
            matches = correlation(recorded[low:high], expected)
            scores[candidate] = float(matches.max()) if len(matches) else 0.
        competitor = max((key for key in scores if key != name), key=scores.get)
        identities.append({"expected": name, "score": scores[name], "competitor": competitor,
                           "competitor_score": scores[competitor], "margin": scores[name] - scores[competitor]})
        offset += len(clip) / SAMPLE_RATE
    return identities


def phrase_observation(samples, clips):
    reference = np.concatenate(clips).astype(np.float64) / 32768.0
    recorded = features(samples)
    scores = correlation(recorded, features(reference))
    if not len(scores):
        raise Incomplete("Capture does not contain a complete phrase-sized window")
    at = int(np.argmax(scores))
    start = at * HOP / ANALYSIS_RATE
    duration = len(reference) / SAMPLE_RATE
    identities = clip_identity(recorded, clips, start)
    clip_scores = [row["score"] for row in identities]
    # Two seconds were acquired before the emulator starts. A missing/dead mic
    # cannot validate either silence or a positive phrase.
    baseline = samples[:SAMPLE_RATE * 2]
    phrase = samples[round(start * SAMPLE_RATE):round((start + duration) * SAMPLE_RATE)]
    signal, noise = rms(phrase), rms(baseline)
    snr = 20 * math.log10(max(signal, 1e-12) / max(noise, 1 / 32768))
    separated = scores.copy()
    guard = math.ceil((duration + .25) * ANALYSIS_RATE / HOP)
    separated[max(0, at - guard):min(len(scores), at + guard + 1)] = -1
    duplicate = float(separated.max()) if len(separated) else -1.
    failures, incomplete = [], []
    if float(scores[at]) < MIN_PHRASE_SCORE or any(score < MIN_CLIP_SCORE for score in clip_scores):
        failures.append("expected complete spoken phrase was not identified")
    if duplicate >= MIN_PHRASE_SCORE:
        failures.append("another complete announcement was detected")
    if any(row["margin"] <= -MIN_IDENTITY_MARGIN and row["competitor_score"] >= MIN_CLIP_SCORE
           for row in identities):
        failures.append("a different spoken word was identified")
    elif any(row["margin"] < MIN_IDENTITY_MARGIN for row in identities):
        incomplete.append("spoken word identity is ambiguous")
    if start < 2.0 or snr < MIN_SIGNAL_NOISE_DB or signal < .001:
        incomplete.append("positive acoustic control is too weak or overlaps the baseline")
    if np.mean(np.abs(samples) >= .999) > .001:
        incomplete.append("microphone recording is clipped")
    outside = np.concatenate((samples[:max(0, round((start - .2) * SAMPLE_RATE))],
                              samples[min(len(samples), round((start + duration + .2) * SAMPLE_RATE)):]))
    count = len(outside) // 1102
    outside_levels = np.sqrt(np.mean(outside[:count * 1102].reshape(count, 1102) ** 2, axis=1)) if count else np.empty(0)
    quiet_limit = max(4 * noise, signal * .04, 2 / 32768)
    unexpected = int(np.sum(outside_levels > quiet_limit))
    if unexpected > 1 and duplicate < MIN_PHRASE_SCORE:
        incomplete.append("sound outside the expected announcement requires investigation")
    return {"result": "INCOMPLETE" if incomplete else "FAIL" if failures else "PASS",
            "failures": failures, "incomplete": incomplete, "phrase_score": float(scores[at]),
            "clip_scores": clip_scores, "clip_identity": identities, "start_s": start, "duration_s": duration,
            "signal_rms": signal, "baseline_rms": noise, "signal_noise_db": snr,
            "duplicate_score": duplicate, "unexpected_sound_windows": unexpected,
            "outside_quiet_rms_limit": quiet_limit}


def silence_observation(samples, positive_controls):
    if len(positive_controls) < 2 or any(row.get("result") != "PASS" for row in positive_controls):
        return {"result": "INCOMPLETE", "failures": [],
                "incomplete": ["silence requires independently identified positive controls before and after"]}
    if len(np.unique(np.round(samples * 32768).astype(np.int32))) < 2:
        return {"result": "INCOMPLETE", "failures": [],
                "incomplete": ["a digitally flat microphone recording cannot establish acoustic silence"]}
    noise = max(row["baseline_rms"] for row in positive_controls)
    signal = min(row["signal_rms"] for row in positive_controls)
    # Refuse a noisy setup instead of increasing the quiet allowance toward the
    # signal level. A mute regression emitting an ordinary announcement fails.
    limit = max(4 * noise, signal * .04, 2 / 32768)
    if limit > signal * .16:
        return {"result": "INCOMPLETE", "failures": [], "incomplete": ["ambient noise prevents a discriminating silence measurement"]}
    count = len(samples) // 1102
    levels = np.sqrt(np.mean(samples[:count * 1102].reshape(count, 1102) ** 2, axis=1))
    loud = int(np.sum(levels > limit))
    return {"result": "FAIL" if loud > 1 else "PASS", "failures": ["sound exceeded the admitted quiet threshold"] if loud > 1 else [],
            "incomplete": [], "quiet_rms_limit": limit, "maximum_window_rms": float(levels.max()),
            "windows_over_limit": loud, "window_seconds": 1102 / SAMPLE_RATE}


def verify_native_recording(directory, sample_count):
    """Cross-check actual PCM length against every timestamped input buffer."""
    directory = Path(directory)
    summary_path, timing_path = directory / "native_summary.json", directory / "audio_timing.ndjson"
    try:
        if summary_path.stat().st_size > 100_000 or timing_path.stat().st_size > 64_000_000:
            raise Incomplete("Native audio evidence exceeds its read bounds")
        summary = json.loads(summary_path.read_text())
        rows = [json.loads(line) for line in timing_path.read_text().splitlines()]
        if (summary.get("schema_version") != 1 or summary.get("result") != "CAPTURED"
                or summary.get("shutdown_reason") != "q" or summary.get("reason") != "none"
                or summary.get("source_clock") != "avcapture_session_synchronization_clock"
                or summary.get("timing_errors") != 0 or summary.get("writer_drops") != 0
                or summary.get("encoded_sample_frames") != sample_count or not rows):
            raise Incomplete("Native audio summary is incomplete or differs from the WAV")
        source_format, previous_start, source_origin = None, None, None
        source_seconds = Fraction(0)
        origins = []
        for index, row in enumerate(rows, 1):
            fmt = row["source_format"]
            rate, frames = fmt["sample_rate"], row["source_sample_frames"]
            start, end = row["host_start_ns"], row["host_end_ns"]
            callback = row["callback_host_ns"]
            if (row.get("schema_version") != 1 or row.get("event") != "audio_buffer"
                    or row.get("status") != "written" or row.get("buffer_seq") != index
                    or type(frames) is not int or frames <= 0 or type(rate) not in (int, float)
                    or not math.isfinite(rate) or rate != int(rate) or not 8000 <= rate <= 192000
                    or type(start) is not int or type(end) is not int or not 0 < start < end
                    or type(callback) is not int or start > callback
                    or (previous_start is not None and start <= previous_start)
                    or (source_format is not None and fmt != source_format)):
                raise Incomplete("Native audio buffer identity, format or order is invalid")
            times = []
            for name in ("source_pts", "source_duration", "audio_pts"):
                value, scale = row[name + "_value"], row[name + "_timescale"]
                if type(value) is not int or type(scale) is not int or scale <= 0:
                    raise Incomplete("Native audio source timestamp is invalid")
                times.append(Fraction(value, scale))
            pts, duration, relative = times
            source_origin = pts if source_origin is None else source_origin
            seconds = Fraction(frames, int(rate))
            if (duration != seconds or pts - source_origin != source_seconds or relative != source_seconds
                    or abs(Fraction(end - start, 1_000_000_000) - seconds) > Fraction(1, int(rate))):
                raise Incomplete("Native audio has missing, overlapping or mistimed samples")
            # Source sample continuity establishes losslessness. The capture clock
            # may refine its mapping to host time between callbacks; carry the
            # complete observed mapping range into cue verification instead of
            # interpreting those refinements as dropped PCM.
            origins.extend((start - source_seconds * 1_000_000_000,
                            end - (source_seconds + seconds) * 1_000_000_000))
            source_seconds += seconds
            source_format, previous_start = fmt, start
        first, last = rows[0]["host_start_ns"], rows[-1]["host_end_ns"]
        if (summary["first_host_ns"] != first or summary["last_host_ns"] != last
                or summary["source_buffers"] != len(rows) or summary["source_format"] != source_format
                or summary["source_sample_frames"] != sum(row["source_sample_frames"] for row in rows)
                or abs(source_seconds * SAMPLE_RATE - sample_count) > 1.01
                or abs(source_seconds - summary["source_duration_s"]) > 1 / SAMPLE_RATE
                or abs(summary["duration_s"] - sample_count / SAMPLE_RATE) > 1 / SAMPLE_RATE):
            raise Incomplete("Native audio source timeline and encoded sample counts disagree")
        return {**summary, "origin_min_host_ns": math.floor(min(origins)) - 1,
                "origin_max_host_ns": math.ceil(max(origins)) + 1}
    except (KeyError, TypeError, ValueError) as error:
        raise Incomplete("Native audio timing evidence is malformed") from error


class MicrophoneCapture:
    """One bounded local WAV recorder, stopped before any DUT cleanup."""
    def __init__(self, directory, device, *, executable="swift"):
        if not isinstance(device, str) or not device.strip() or ":" in device or "\n" in device:
            raise ValueError("Use an explicit microphone device name")
        self.directory, self.device = Path(directory), device
        self.directory.mkdir(mode=0o700, parents=True, exist_ok=False)
        self.executable = shutil.which(executable)
        if not self.executable:
            raise Incomplete("Native microphone capture is unavailable")
        self.process = self.log = None
        self.result = None
        self.started_ns = None
        self.ready_ns = None
        self.ready_media_s = None
        self.first_host_ns = None

    def start(self, pump=lambda: None):
        if self.process is not None:
            raise Incomplete("Microphone capture cannot be started twice")
        MODULE_CACHE.mkdir(parents=True, exist_ok=True)
        command = [self.executable, "-module-cache-path", str(MODULE_CACHE), str(RECORDER),
                   "--device-name", self.device, "--output", str(self.directory / "microphone.wav"),
                   "--timing-sidecar", str(self.directory / "audio_timing.ndjson"),
                   "--summary", str(self.directory / "native_summary.json"),
                   "--owner-pid", str(os.getpid()), "--max-seconds", "240"]
        self.started_ns = time.monotonic_ns()
        atomic_json(self.directory / "intent.json", {"device": self.device, "command": command,
                    "started_host_ns": self.started_ns, "executable_sha256": file_sha(self.executable),
                    "recorder_source_sha256": file_sha(RECORDER), "recorder_backend": "avfoundation_native"})
        self.log = (self.directory / "recorder.log").open("xb")
        try:
            self.process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                            stderr=self.log, start_new_session=True)
            deadline = time.monotonic() + 30
            pending = b""
            while time.monotonic() < deadline:
                pump()
                if self.process.poll() is not None:
                    raise Incomplete("Microphone recorder exited before admission")
                readable, _, _ = select.select([self.process.stdout], [], [], .1)
                if readable:
                    pending += os.read(self.process.stdout.fileno(), 8192)
                    lines = pending.split(b"\n")
                    pending = lines.pop()
                    for line in lines:
                        event = json.loads(line)
                        if event.get("event") != "ready":
                            continue
                        now = time.monotonic_ns()
                        first, ready = event.get("first_host_ns"), event.get("ready_host_ns")
                        if (event.get("schema_version") != 1
                                or event.get("source_clock") != "avcapture_session_synchronization_clock"
                                or type(first) is not int or type(ready) is not int
                                or not self.started_ns <= first <= ready <= now
                                or event.get("output_format") != {"sample_rate": SAMPLE_RATE, "channels": 1, "bits_per_channel": 16}):
                            raise Incomplete("Native microphone readiness identity or clock is invalid")
                        self.ready_ns, self.first_host_ns = now, first
                        self.ready_media_s = event["source_duration_s"]
                        atomic_json(self.directory / "ready.json", event)
                        return
            raise Incomplete("Microphone did not produce samples within30seconds")
        except BaseException:
            self.finish(False)
            raise

    def health_problem(self):
        if self.process is not None and self.process.stdout is not None:
            while select.select([self.process.stdout], [], [], 0)[0]:
                if not os.read(self.process.stdout.fileno(), 8192):
                    break
        return "" if self.process is not None and self.process.poll() is None else "Microphone recorder stopped unexpectedly"

    def finish(self, completed):
        if self.result is not None:
            return self.result
        errors = []
        stop_requested_ns = time.monotonic_ns()
        if self.process is not None:
            try:
                if self.process.poll() is not None:
                    errors.append("Microphone recorder stopped before the requested capture end")
                else:
                    self.process.communicate(b"q\n", timeout=5)
            except (OSError, subprocess.TimeoutExpired) as error:
                errors.append(str(error))
                self.process.kill()
                self.process.communicate(timeout=3)
        if self.log is not None:
            self.log.close()
        try:
            samples = read_recording(self.directory / "microphone.wav")
            duration = len(samples) / SAMPLE_RATE
            digest = file_sha(self.directory / "microphone.wav")
            native = verify_native_recording(self.directory, len(samples))
            if (self.first_host_ns != native["first_host_ns"]
                    or not self.started_ns <= native["first_host_ns"] < native["last_host_ns"] <= time.monotonic_ns()
                    or stop_requested_ns - native["last_host_ns"] > 250_000_000):
                raise Incomplete("Native microphone samples do not cover the requested recording interval")
        except (OSError, EOFError, wave.Error, Incomplete) as error:
            errors.append(str(error)); duration, digest, native = 0, None, {}
        code = self.process.returncode if self.process is not None else None
        self.result = {"result": "CAPTURED" if completed and code == 0 and not errors else "INCOMPLETE",
                       "device": self.device, "returncode": code, "errors": errors,
                       "sample_rate": SAMPLE_RATE, "channels": 1, "duration_s": duration,
                       "wav_sha256": digest, "started_host_ns": self.started_ns,
                       "recorder_backend": "avfoundation_native",
                       "ready_host_ns": self.ready_ns, "ready_media_s": self.ready_media_s,
                       "first_host_ns": native.get("first_host_ns"), "last_host_ns": native.get("last_host_ns"),
                       "native_summary_sha256": file_sha(self.directory / "native_summary.json") if native else None,
                       "timing_sha256": file_sha(self.directory / "audio_timing.ndjson") if native else None,
                       "stop_requested_host_ns": stop_requested_ns,
                       "finished_host_ns": time.monotonic_ns(), "collection_completed": bool(completed)}
        atomic_json(self.directory / "capture.json", self.result)
        return self.result

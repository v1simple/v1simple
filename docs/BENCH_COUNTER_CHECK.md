# Sampled count/mode check

`./bench.sh --replay --camera` runs the sampled live-counter check once after a
replay-camera window publishes qualified or collection-only evidence. It writes
new evidence under `replay/counter-check/` and prints collection, runtime
qualification, camera integrity, and the sampled counter result separately. It
does not run for a bench command without replay-camera evidence; that path prints
`sampled live counter: NOT_EVALUATED` and keeps its prior exit behavior.

The automatic check divides the validated recorded input into maximal live-alert
intervals with the same count and mute state, then fixes each interval's temporal
midpoint before decoding pixels. Of the immediate timestamped frames before and
after that midpoint, it deterministically chooses the closest one whose existing
count and mode expectations are resolved, provided the frame is inside the same
interval and within its own one-frame duration of the midpoint. If neither frame
qualifies, the interval remains unresolved without inspecting either frame's
pixels. Every selected interval remains in the denominator. Missing input
identity, an interval without a recorded end, or an unreadable selected frame
makes the result `INCONCLUSIVE`.

The same automatic mode can be run offline against a retained replay directory
containing `window_result.json`. It reads original recordings and starts no
hardware or inference service. Python 3.10+, `ffmpeg`, and `ffprobe` must be
available.

```sh
python3 scripts/bench/counter_check.py \
  --run-dir path/to/run/replay \
  --auto \
  --out path/to/new-counter-result
```

Explicit offsets remain available for bounded diagnosis:

```sh
python3 scripts/bench/counter_check.py \
  --run-dir path/to/run/replay \
  --at 3.2 9.2 24.2 \
  --out path/to/new-counter-result
```

Choose explicit sample times before inspecting their pixels. `--at` specifies seconds
after the first recorded replay request. These are sampled times, not response
deadlines. The command selects the nearest timestamped original frame, retains
its decoded RGB pixels as a PNG, and joins observations to input expectations
using that frame's identity. Every requested sample remains in the denominator;
missing frames, capture gaps and requests resolving to the same frame remain
unresolved. Existing outputs are never overwritten.

The reader recognizes orange digits and `A`, `L`, `l` in the registered counter
slot. It uses the recorded SCAN landmark geometry, never packet expectations or
a transform search. Blank, dim, partial or unsupported observations remain
unknown. A numeric glyph implies no mode glyph in that same slot, and vice versa.
Lower vertical samples avoid the font's sloping left edges. Their original
supports remain absence guards: a fragment left beside a dark interior remains
unknown, rather than disappearing into a different clean digit.
This calibration has limited development-recording coverage; it is not a
general OCR reader. Its interior and stray-ink checks cannot detect every
displacement or every defect outside the sampled regions.

Expected values come from validated display packet bytes. The command checks
scenario alert semantics against planned packets and their recorded host
acceptance, including unscoped display responses. Blink planes may permit more
than one value. The supplied scenario is hashed in the result and semantically
cross-checked; older windows do not retain its original file hash.

A canonical empty-table clear followed by an ordered idle display can establish
post-replay idle context. The original scenario count stays in the evidence;
partial, conflicting or ambiguously ordered table updates remain unresolved.

Idle interpretation requires separately established `stealthEnabled=false`.
Without that evidence, idle expectations stay unresolved. When already known,
pass `--configuration path/to/configuration-evidence.json` containing:

```json
{
  "window_result_sha256": "SHA256 of this recording's window_result.json",
  "settings": {"stealthEnabled": false},
  "precondition": "Describe the independently established configuration basis"
}
```

The hash binds this supplied configuration assertion to one window; it does not
establish the setting itself. Do not derive the setting from the frame being
tested or substitute defaults.

`report.md` lists each field check with its original frame and reason;
`result.json` retains identities, packet basis, pixel measurements, timing
verification, runtime qualification and implementation hashes. `expected.json`
and `observed.json` remain separate for review.

For the offline command, exit **0 / PASS** means every requested count/mode check matched. **1 / FAIL**
means a supported sample differs from recorded input; this alone does not locate
a firmware defect. **2 / INCONCLUSIVE** means evidence was insufficient or the
output could not be written. A supported mismatch is retained even when another
sample is unresolved. Other display fields, unsampled intervals, DUT receipt,
response latency and full-run correctness are not evaluated. The command does
not promote the original bench verdict or firmware qualification.

For `bench.sh`, a hard collection failure exits **2**. Collection-only evidence
exits **1** regardless of the separately printed counter result. With qualified
camera collection, the broader [sampled encounter check](BENCH_ENCOUNTER_CHECK.md)
controls the command outcome: its `PASS`, `INCONCLUSIVE`, and `FAIL` exit **0**,
**1**, and **2**, respectively. The narrow counter and existing image-change
timing results remain separate and cannot raise the encounter result.

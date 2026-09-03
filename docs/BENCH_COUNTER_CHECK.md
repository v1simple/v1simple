# Sampled count/mode check

Run this offline command against a retained `bench.sh --replay --camera` replay
directory containing `window_result.json`. It reads original recordings and
starts no hardware or inference service. Python 3.10+, `ffmpeg`, and `ffprobe`
must be available.

```sh
python3 scripts/bench/counter_check.py \
  --run-dir path/to/run/replay \
  --at 3.2 9.2 24.2 \
  --out path/to/new-counter-result
```

Choose sample times before inspecting their pixels. `--at` specifies seconds
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
This calibration has limited development-recording coverage; it is not a
general OCR reader. Its interior and stray-ink checks cannot detect every
displacement or every defect outside the sampled regions.

Expected values come from validated display packet bytes. The command checks
scenario alert semantics against planned packets and their recorded host
acceptance, including unscoped display responses. Blink planes may permit more
than one value. The supplied scenario is hashed in the result and semantically
cross-checked; older windows do not retain its original file hash.

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

Exit **0 / PASS** means every requested count/mode check matched. **1 / FAIL**
means a supported sample differs from recorded input; this alone does not locate
a firmware defect. **2 / INCONCLUSIVE** means evidence was insufficient or the
output could not be written. A supported mismatch is retained even when another
sample is unresolved. Other display fields, unsampled intervals, DUT receipt,
response latency and full-run correctness are not evaluated. The command does
not promote the original bench verdict or firmware qualification.

#!/usr/bin/env python3
"""Parse soak panic JSONL into key=value summary fields."""

import json
import sys


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: soak_parse_panic.py <panic_jsonl>", file=sys.stderr)
        return 2

    path = sys.argv[1]
    samples = 0
    ok_samples = 0
    was_crash_true = 0
    has_panic_file_true = 0
    first_was_crash = ""
    last_was_crash = ""
    first_has_panic_file = ""
    last_has_panic_file = ""
    first_reset_reason = ""
    last_reset_reason = ""
    state_change_count = 0
    prev_state = None

    try:
        with open(path, "r", encoding="utf-8") as f:
            for raw in f:
                line = raw.strip()
                if not line:
                    continue
                samples += 1
                try:
                    rec = json.loads(line)
                except Exception:
                    continue
                if not rec.get("ok"):
                    continue
                data = rec.get("data")
                if not isinstance(data, dict):
                    continue
                ok_samples += 1
                was_crash = data.get("wasCrash") is True
                has_panic_file = data.get("hasPanicFile") is True
                reset_reason = data.get("lastResetReason")
                if was_crash:
                    was_crash_true += 1
                if has_panic_file:
                    has_panic_file_true += 1
                state = (
                    1 if was_crash else 0,
                    1 if has_panic_file else 0,
                    reset_reason if isinstance(reset_reason, str) else "",
                )
                if prev_state is None:
                    first_was_crash = str(state[0])
                    first_has_panic_file = str(state[1])
                    first_reset_reason = state[2]
                elif state != prev_state:
                    state_change_count += 1
                prev_state = state
                last_was_crash = str(state[0])
                last_has_panic_file = str(state[1])
                last_reset_reason = state[2]
    except FileNotFoundError:
        pass

    print(f"samples={samples}")
    print(f"ok_samples={ok_samples}")
    print(f"was_crash_true={was_crash_true}")
    print(f"has_panic_file_true={has_panic_file_true}")
    print(f"first_was_crash={first_was_crash}")
    print(f"last_was_crash={last_was_crash}")
    print(f"first_has_panic_file={first_has_panic_file}")
    print(f"last_has_panic_file={last_has_panic_file}")
    print(f"first_reset_reason={first_reset_reason}")
    print(f"last_reset_reason={last_reset_reason}")
    print(f"state_change_count={state_change_count}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

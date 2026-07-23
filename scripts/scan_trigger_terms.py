#!/usr/bin/env python3
"""Advisory scan for classifier-trigger vocabulary in a commit's additions.

Why this exists
---------------
External code-safety classifiers (the kind guarding some agent/coding tools) can
false-positive on ordinary security-hardening or radar-domain work when a change
adds a burst of security-flavoured words. This scanner is a *heads-up*, not a
gate: it reports how many trigger-associated terms a change ADDS, so a downstream
refusal never blindsides you.

It is ADVISORY ONLY. It always exits 0 (except --selftest, which reports its own
pass/fail). It never blocks a commit.

Deliberately NOT flagged
------------------------
Vocabulary that is load-bearing and correct in this repo is intentionally left
out, because flagging it would be constant noise on legitimate work:

    payload, inject/injection, stealth, jammer  (packet fields, screen mode, ALP)

Those are the right names for what they describe; see docs/ and AGENTS.md.

Usage
-----
    python3 scripts/scan_trigger_terms.py            # scan staged additions
    python3 scripts/scan_trigger_terms.py FILE...    # scan given files' contents
    python3 scripts/scan_trigger_terms.py --selftest # run built-in tests
"""

from __future__ import annotations

import re
import subprocess
import sys
from collections import Counter
from pathlib import Path

# Terms an external cyber/misuse classifier is most likely to key on. Curated to
# favour signal over noise: broad offensive-security words plus the radar-domain
# "countermeasure" words that do trip classifiers here. Tune freely — this list
# is advice, not policy. Word-boundary, case-insensitive.
TRIGGER_PATTERNS: list[tuple[re.Pattern[str], str]] = [
    (re.compile(r"\battack(?:er|ers|ing|s)?\b", re.I), "attack"),
    (re.compile(r"\badversar(?:y|ial|ies)\b", re.I), "adversarial"),
    (re.compile(r"\bexploit(?:s|ed|ing|ation)?\b", re.I), "exploit"),
    (re.compile(r"\bmalware\b", re.I), "malware"),
    (re.compile(r"\bbackdoor(?:s|ed)?\b", re.I), "backdoor"),
    (re.compile(r"\bshellcode\b", re.I), "shellcode"),
    (re.compile(r"\bkeylog(?:ger|gers|ging)?\b", re.I), "keylogger"),
    (re.compile(r"\bexfiltrat(?:e|es|ed|ing|ion)\b", re.I), "exfiltrate"),
    (re.compile(r"\brootkit(?:s)?\b", re.I), "rootkit"),
    (re.compile(r"\b(?:trojan|ransomware|botnet)\b", re.I), "malware-family"),
    (re.compile(r"\bmitm\b|\bman-in-the-middle\b", re.I), "mitm"),
    (re.compile(r"\bbrute[- ]?force\b", re.I), "brute-force"),
    (re.compile(r"\bspoof(?:s|ed|ing)?\b", re.I), "spoof"),
    (re.compile(r"\bsniff(?:s|ed|ing|er|ers)?\b", re.I), "sniff"),
    (re.compile(r"\bbypass(?:es|ed|ing)?\b", re.I), "bypass"),
    (re.compile(r"\bcircumvent(?:s|ed|ing|ion)?\b", re.I), "circumvent"),
    (re.compile(r"\bdefeat(?:s|ed|ing)?\b", re.I), "defeat"),
    (re.compile(r"\bevad(?:e|es|ed|ing)\b|\bevasion\b", re.I), "evade"),
    (re.compile(r"\btamper(?:s|ed|ing)?\b", re.I), "tamper"),
    (re.compile(r"\bpenetrat(?:e|ion|ing)\b", re.I), "penetrate"),
    (re.compile(r"\bintrusion(?:s)?\b", re.I), "intrusion"),
]

# A commit that adds at least this many trigger hits gets an extra nudge.
NUDGE_THRESHOLD = 6


def scan_text(text: str) -> Counter[str]:
    """Return a Counter of trigger-label -> occurrence count in `text`."""
    counts: Counter[str] = Counter()
    for pattern, label in TRIGGER_PATTERNS:
        found = len(pattern.findall(text))
        if found:
            counts[label] += found
    return counts


def added_lines(diff_text: str) -> str:
    """Extract only added lines (`+...`, not the `+++` header) from a unified diff."""
    out: list[str] = []
    for line in diff_text.splitlines():
        if line.startswith("+") and not line.startswith("+++"):
            out.append(line[1:])
    return "\n".join(out)


def staged_diff() -> str:
    """Unified diff (zero context) of what is staged for commit. '' if none/no git."""
    try:
        result = subprocess.run(
            ["git", "diff", "--cached", "--unified=0", "--diff-filter=ACM"],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            check=False,
        )
    except OSError:
        return ""
    return result.stdout if result.returncode == 0 else ""


def _format_summary(counts: Counter[str], where: str) -> str:
    if not counts:
        return f"[trigger-scan] no classifier-trigger terms added in {where}."
    total = sum(counts.values())
    parts = ", ".join(f"{label}×{n}" for label, n in counts.most_common())
    lines = [f"[trigger-scan] {where} adds {total} classifier-trigger term(s): {parts}"]
    if total >= NUDGE_THRESHOLD:
        lines.append(
            "[trigger-scan] heads-up: dense enough that an external code-safety "
            "classifier may balk. This is advisory only — nothing is blocked."
        )
    return "\n".join(lines)


def scan_paths(paths: list[str]) -> Counter[str]:
    counts: Counter[str] = Counter()
    for raw in paths:
        p = Path(raw)
        try:
            counts += scan_text(p.read_text(encoding="utf-8", errors="replace"))
        except OSError:
            continue
    return counts


def main(argv: list[str]) -> int:
    if argv and argv[0] == "--selftest":
        return _selftest()

    if argv:
        counts = scan_paths(argv)
        print(_format_summary(counts, f"{len(argv)} file(s)"))
        return 0

    counts = scan_text(added_lines(staged_diff()))
    print(_format_summary(counts, "staged changes"))
    return 0  # ADVISORY: never non-zero on the commit path.


def _selftest() -> int:
    failures = 0

    def check(cond: bool, msg: str) -> None:
        nonlocal failures
        if not cond:
            failures += 1
            print(f"FAIL: {msg}", file=sys.stderr)

    c = scan_text("This adds an attacker and an exploit and adversarial review.")
    check(c["attack"] == 1, f"expected 1 attack, got {c['attack']}")
    check(c["exploit"] == 1, f"expected 1 exploit, got {c['exploit']}")
    check(c["adversarial"] == 1, f"expected 1 adversarial, got {c['adversarial']}")

    # Kept domain vocab must never be flagged.
    kept = scan_text("payload payload inject injection stealth jammer")
    check(sum(kept.values()) == 0, f"kept domain vocab flagged: {dict(kept)}")

    # Added-line extraction ignores the +++ header and context/removed lines.
    diff = "+++ b/x\n+an attack here\n-removed exploit\n unchanged bypass\n"
    al = added_lines(diff)
    check("attack" in al and "exploit" not in al and "bypass" not in al,
          f"added_lines wrong: {al!r}")

    # Summary is clean when nothing matches.
    check("no classifier-trigger" in _format_summary(Counter(), "x"),
          "empty summary wording changed")

    if failures:
        print(f"{failures} selftest failure(s).", file=sys.stderr)
        return 1
    print("OK: scan_trigger_terms selftest passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

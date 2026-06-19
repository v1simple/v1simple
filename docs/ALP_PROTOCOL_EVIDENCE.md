# ALP Protocol Evidence

> Status: Active
> Date: 2026-05-22

This tracked note is safe for source comments and tests to cite. It contains original project analysis, byte-pattern summaries, and capture-derived rules only; it does not quote AL Priority manuals or local private notes.

Local manual extracts may exist on a maintainer machine, but they are intentionally gitignored. Code and tests should cite this file instead of private local notes.

## Frame families used by the runtime

| Family | Shape | Runtime use |
|---|---|---|
| Heartbeat | `B0 xx 00 cc` | Tracks warm-up, PDC, DLI, LID, targeted-front, and teardown state. |
| Gun ID, operational | `CX yy 00` | Primary gun fingerprint in everyday PDC/DLI/LID operation; fingerprint is `(byte0, byte1)`. |
| Gun ID, LID deploy | `CX 00 yy` | Alternate fingerprint observed when LID is actively deploying; fingerprint is `(byte0, byte2)`. |
| Generic trigger | `C8 00 04` | Opens a speculative session but is not enough to identify a gun. |

The same gun keeps the same `byte0` across the two gun-ID frame variants. Runtime code normalizes both variants before looking up a gun type.

## Heartbeat byte1 rules

| `B0` byte1 | Derived runtime meaning | Direction rule |
|---:|---|---|
| `0x01` | Targeted-front alert. | Self-anchors `FRONT`; may override a previous rear latch. |
| `0x02` | Warm-up or PDC-at-rest state. | Not a direction sample. |
| `0x03` | DLI-active state. | May latch `REAR` only after a session-local gun-ID anchor. |
| `0x04` | LID-active state. | May latch `REAR` only after a session-local gun-ID anchor. |
| `0x06` | Transitional engaged state seen between warm-up and active modes. | Treated like `0x02` for warm-up-release edge handling. |

`0x03` and `0x04` are not trusted as display direction until a gun-ID frame has anchored the session. This prevents boot and idle chatter from producing a false rear laser display.

## Direction anchoring

Rear direction samples are gated by a session-local gun-ID frame:

1. Open or update a session from a gun-ID burst.
2. Sample the first non-`00` and non-`02` heartbeat byte after that gun ID.
3. Latch `0x03` or `0x04` as `REAR`.
4. If a later `0x01` Targeted heartbeat arrives, relatch to `FRONT`.

The `0x01` exception is deliberate because Targeted is the ALP's front-facing signal and can arrive before or after the gun-ID burst in very short front shots.

Capture-derived regressions that shaped this rule:

- `alp_5-3fe19956.csv`, session 7: repeated `0x03` heartbeat samples with no gun-ID frames; without the anchor gate this paints a false rear laser on a front-only install.
- `alp_2-f6e0ca7e.csv`: front shot initially followed a rear-looking `0x04`/`0x03` trajectory, then later reported `0x01`; runtime must relatch `FRONT` on `0x01`.
- `alp_2-64991486.csv`: boot envelope includes a `0x02`→`0x04` sequence; warm-up-release logic must not promote that boot walk to a visible laser event.
- `alp_6-61aa53a8.csv`: real sessions can use `0x02`→`0x06`→`0x03`; `0x06` must be accepted in the warm-up-release path.

## Gun-code fingerprints tracked in firmware

The runtime's lookup table currently contains these normalized `(byte0, fingerprint)` pairs:

| Bytes | Gun type |
|---|---|
| `C8 D5` | PL3 / ProLaser |
| `C8 D6` | DragonEye Compact |
| `C9 F5` | LTI TruSpeed LR |
| `CB EB` | Laser Atlanta PL2 |
| `CD D6` | Marksman / UltraLyte |
| `CD EB` | Stalker LZ1 |
| `CD D7` | Laser Ally |

Update this table only from new capture-backed evidence. Keep manual excerpts out of this tracked file.

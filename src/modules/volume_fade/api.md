# volume_fade Module API

V1 volume fade-out logic. After a configured delay during an active alert, fades the V1 volume down to a configured level; on alert clear, restores the original volume. The module is a pure decision function — the caller (quiet coordinator) sends the actual BLE commands.

Per the header at lines 4-15:

- *Does:* Track alert start, decide fade-down vs restore, track original volume, decide when to restore.
- *Does NOT:* Send BLE commands.

## Public types

### `struct VolumeFadeContext`
**Source:** `volume_fade_module.h:24-36`.

Per-decision input — `hasAlert`, `alertMuted`, `alertSuppressed`, `currentVolume`, `currentMuteVolume`, `currentFrequency` (for dedup), `now`. Default-constructs to zero/false for safe fall-through.

### `struct VolumeFadeAction`
**Source:** `volume_fade_module.h:41-58`.

Decision output. `Type` enum:
- `NONE` — no action.
- `FADE_DOWN` — reduce volume; payload: `targetVolume`, `targetMuteVolume`.
- `RESTORE` — restore original; payload: `restoreVolume`, `restoreMuteVolume`.

`hasAction()` returns true unless `NONE`.

## Class: `VolumeFadeModule`

**Header:** `src/modules/volume_fade/volume_fade_module.h:64`.

### Lifecycle

#### `VolumeFadeModule()`
Default constructor.
**Source:** `volume_fade_module.h:65`.

#### `void begin(SettingsManager* settings)`
Wires settings dependency.
**Source:** `volume_fade_module.h:67`.

### Decision

#### `VolumeFadeAction process(const VolumeFadeContext& ctx)`
Main decision method.
**Source:** `volume_fade_module.h:70`.

### Cross-module hint

#### `void setBaselineHint(uint8_t mainVol, uint8_t muteVol, uint32_t nowMs)`
Injects a one-shot baseline hint from another volume owner (typically the quiet coordinator after a SpeedVolume RESTORE). Used when a new alert arrives before V1 echoes back the true volume — without the hint, the module would capture the *faded* volume as the baseline and "restore" to that lower value.

The hint is cleared after first use or after `HINT_WINDOW_MS` (1500 ms).
**Source:** `volume_fade_module.h:76`.

## Internal state (informational)

Selected constants:

- `MAX_FADE_SEEN_FREQS = 12` — capacity for tracking distinct alert frequencies seen during an alert burst.
- `PENDING_RESTORE_WINDOW_MS = 1500` — short carry-over after RESTORE, during which a new alert won't recapture the faded volume.
- `RESTORE_RETRY_MIN_INTERVAL_MS = 75` — minimum gap between RESTORE retries (matches quiet coordinator's speed-vol retry cadence).
- `HINT_WINDOW_MS = 1500` — baseline-hint validity window.

**Source:** `volume_fade_module.h:83-99`.

The pending-restore carry-over is the load-bearing fix for a previous defect: if a new alert arrived during the BLE round-trip of a RESTORE command, the module would see the faded volume as the current state and capture that as the new "original." On the next RESTORE, V1 would be left at the faded value.

## Dependencies

| Dependency | Purpose |
|---|---|
| `SettingsManager*` | Reads `alertVolumeFadeEnabled`, `alertVolumeFadeDelaySec`, `alertVolumeFadeVolume`. |

## Notes for maintainers

The baseline-hint mechanism is co-designed with the quiet coordinator. When SpeedVolume RESTOREs the volume, it calls `setBaselineHint` to tell VolumeFade the real value, so VolumeFade doesn't pick up the still-in-flight value as the baseline. If you change SpeedVolume's RESTORE flow, double-check the hint plumbing.

The pending-restore carry-over (`PENDING_RESTORE_WINDOW_MS`) and the RESTORE retry interval (`RESTORE_RETRY_MIN_INTERVAL_MS`) are intentionally aligned with the quiet coordinator's speed-vol retry cadence (75 ms / 1500 ms). They share the same load-bearing assumption: BLE writes are best-effort, retries close the eventual-consistency gap, and the carry-over prevents re-capture during the gap. Don't tune one without reviewing the other.

Settings — `alertVolumeFadeDelaySec` is in seconds (1–10) and `alertVolumeFadeVolume` is in V1 volume units (1–9). Volume of 0 isn't allowed for fade because V1 volume 0 means "muted with the small mute icon", which is a different state and would compose oddly with explicit mute commands.

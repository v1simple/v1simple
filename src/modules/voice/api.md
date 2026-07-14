# voice Module API

Voice announcement decision engine. Decides **when** to announce (cooldowns, mute state, settings), **what** to announce (priority alert vs. secondary vs. direction-only vs. threat escalation), and tracks announcement state (last-announced ID, throttling). Returns a typed `VoiceAction`; the caller plays the audio.

Per the header at lines 6-15:

- *Does:* Decide when, decide what, track state.
- *Does NOT:* Play audio, parse BLE data.

## Public types

### `struct VoiceContext`
**Source:** `voice_module.h:32-56`.

Per-decision input — alerts array, priority alert, V1 mute state, proxy-connected flag, main volume, suppressed flag, current `nowMs`. Default constructor zeros everything for safe fall-through.

### `struct VoiceAction`
**Source:** `voice_module.h:64-93`.

Decision output. `Type` enum:
- `NONE`
- `ANNOUNCE_PRIORITY` — full announcement: band + freq + direction + bogeys.
- `ANNOUNCE_DIRECTION` — direction-only update for the current alert.
- `ANNOUNCE_SECONDARY` — full announcement for a secondary alert.
- `ANNOUNCE_ESCALATION` — threat escalation: band + freq + direction + per-direction breakdown.

Payload fields are interpreted per type. `hasAction()` returns true unless `type == NONE`.

## Class: `VoiceModule`

**Header:** `src/modules/voice/voice_module.h:104`.

### Lifecycle

#### `VoiceModule()`
Default constructor.
**Source:** `voice_module.h:100`.

#### `void begin(SettingsManager* settings, V1BLEClient* ble = nullptr)`
Wires dependencies. BLE pointer is optional — the module reads V1Settings primarily; BLE is for state queries that might be needed in future.
**Source:** `voice_module.h:103`.

### Decision

#### `VoiceAction process(const VoiceContext& ctx)`
Main decision method. Returns the action to take (if any).
**Source:** `voice_module.h:106`.

### State management

#### `void reset()` / `void clearAllState()`
Clears all tracking state (announced IDs, history, cooldowns). Called on connection cycle reset.
**Source:** `voice_module.h:109`.

### Static utilities

#### `static uint8_t getAlertBars(const AlertData& alert)`
Signal bars for an alert based on its direction.

#### `static uint32_t makeAlertId(Band band, uint16_t freq)`
Unique alert identifier for tracking purposes.

#### `static bool isBandEnabledForSecondary(Band band, const V1Settings& settings)`
Whether a band is enabled for secondary-alert announcements (per audio settings).

#### `static AlertDirection toAudioDirection(Direction dir)`
Convert V1 direction bitmask to the audio-module direction enum.

**Source:** `voice_module.h:126`.

### Speed helpers

#### `float getCurrentSpeedMph(unsigned long now)`
#### `bool getCurrentSpeedSample(unsigned long now, float& speedMphOut) const`
#### `void updateSpeedSample(float speedMph, unsigned long timestampMs)`
#### `void clearSpeedSample()`
#### `bool hasValidSpeedSource(unsigned long now) const`

Speed-aware voice features (e.g. dropping bogey-count announcements at low speed) need an arbitrated speed source. The module caches a speed sample with `SPEED_CACHE_MAX_AGE_MS` (5 s) freshness.

**Source:** `voice_module.h:133`.

## Internal tracking (informational)

The module carries substantial state for cooldowns and threat-escalation logic. Selected constants (private but useful for diagnosis):

- `MAX_ANNOUNCED_ALERTS = 10` — capacity for tracking which alert IDs we've already announced.
- `WEAK_THRESHOLD = 2`, `STRONG_THRESHOLD = 4` — bar thresholds for escalation.
- `SUSTAINED_MS = 500` — how long a strong reading must persist before escalation announces.
- `HISTORY_STALE_MS = 5000` — alert history cleanup window.
- `MAX_BOGEYS_FOR_ESCALATION = 4`
- `DIRECTION_THROTTLE_WINDOW_MS = 10000`, `DIRECTION_CHANGE_LIMIT = 3` — throttles direction-change announcements to 3 per 10 s window.
- `PRIORITY_STABILITY_MS = 1000` — priority alert must be stable for 1 s before announcing.
- `POST_PRIORITY_GAP_MS = 1500` — minimum gap before a secondary can announce after a priority.
- `VOICE_ALERT_COOLDOWN_MS = 2000` — minimum gap between repeat announcements of the same alert.
- `BOGEY_COUNT_COOLDOWN_MS = 500` — minimum gap between bogey-count announcements.

**Source:** `voice_module.h`.

## Test seams (UNIT_TEST only)

#### `AlertHistoryArray& getAlertHistories()` / `uint8_t& getAlertHistoryCount()`
Direct access to internal alert history for tests.

#### `void testUpdateAlertHistory(...)`
Exposes the private update method.

**Source:** `voice_module.h:241`.

## Dependencies

| Dependency | Purpose |
|---|---|
| `SettingsManager*` | Reads voice/audio settings. |
| `V1BLEClient*` (optional) | Reserved for future state queries. |
| `AlertData`, `Band`, `Direction` (`packet_parser.h`) | Input types. |
| `AlertBand`, `AlertDirection` (`audio_beep.h`) | Output types. |

## Notes for maintainers

The `Type::ANNOUNCE_DIRECTION` action is for *same-alert direction updates* — the alert already announced, but its direction has changed (Ahead → Beside). It's a shorter announcement than the full priority. Don't collapse it into ANNOUNCE_PRIORITY; the difference reduces voice clutter during a single encounter.

The escalation logic tracks per-alert bar history. Don't try to read it directly — use `process()` and let the module decide. The state machine for "weak → strong → sustained → escalate" is internal and the public surface is the typed action.

The 2-second voice cooldown (`VOICE_ALERT_COOLDOWN_MS`) prevents the announcer from spamming during a sustained alert. Don't reduce it below that without re-listening to the announcer in real driving — past testers found shorter intervals annoying.

Speed-aware voice features (announce bogey count only above some speed) are layered on top via `hasValidSpeedSource()` plus the audio settings. Speed source loss makes voice fall back to "always announce."

# speed Module API

Speed-source arbitration. Selects between available speed sources with **OBD as the primary source and GPS as the secondary fallback**. Provides a single `SpeedSelection` snapshot to consumers (speed-mute, stealth-mode display).

## Selection priority

OBD always wins when present. GPS is only used when OBD is unavailable (disabled, not connected, or stale). If GPS is selected and OBD subsequently comes online with a fresh sample, OBD takes over on the very next `update()` call. GPS may be the first source on boot, but a fresh OBD sample immediately preempts it.

The order each tick:
1. **OBD** — selected if `obdEnabled` AND `ObdRuntimeModule::getFreshSpeed()` returns true AND speed ≤ `MAX_VALID_SPEED_MPH`.
2. **GPS** — selected if OBD did not win AND `gpsEnabled` AND the GPS sample passes the "good signal" gate.
3. **NONE** — neither source qualifies.

### GPS "good signal" gate

GPS only takes the selection when **all** of:
- `GpsRuntimeModule::getFreshSpeed()` returns true (sample within `GpsRuntimeModule::SAMPLE_MAX_AGE_MS = 3000 ms`).
- Speed ≤ `MAX_VALID_SPEED_MPH`.
- `GpsRuntimeStatus::stableHasFix == true` (fix held for the GPS module's stability window).
- `satellites ≥ GPS_MIN_SATELLITES` (default `4`).
- `0 < hdop ≤ GPS_MAX_HDOP` (default `5.0`; NaN HDOP fails).

## Class: `SpeedSourceSelector`

**Header:** `src/modules/speed/speed_source_selector.h`.

### Public types

#### `enum class SpeedSource : uint8_t`
- `NONE = 0`
- `GPS = 1`
- `OBD = 3` (gap deliberately preserved for future sources — do not renumber).

#### `struct SpeedSelection`
Latest committed selection — `source`, `speedMph`, `timestampMs`, `ageMs`, `valid`.

#### `struct SpeedSelectorStatus`
Diagnostic status — selection plus per-source freshness and aggregate counters.

OBD fields: `obdEnabled`, `obdFresh`, `obdSpeedMph`, `obdAgeMs`.
GPS fields: `gpsEnabled`, `gpsFresh`, `gpsGoodSignal`, `gpsSpeedMph`, `gpsAgeMs`, `gpsSatellites`, `gpsHdop`.
Counters: `sourceSwitches`, `obdSelections`, `gpsSelections`, `noSourceSelections`.

### Constants

- `MAX_VALID_SPEED_MPH = 250.0f` — sanity ceiling shared by both sources.
- `GPS_MIN_SATELLITES = 4` — minimum sats for GPS "good signal".
- `GPS_MAX_HDOP = 5.0f` — maximum HDOP for GPS "good signal".

### Lifecycle

#### `void begin(ObdRuntimeModule* obd, bool obdEnabled, GpsRuntimeModule* gps = nullptr, bool gpsEnabled = false)`
Wires both sources and their initial enable flags. Either pointer may be `nullptr` to disable that source unconditionally.

#### `void syncEnabledInputs(bool obdEnabled, bool gpsEnabled = false)`
Runtime enable/disable update for both sources.

### Pump

#### `void update(uint32_t nowMs)`
Producer call — once per main-loop tick. Commits the current selection and updates counters.

### Consumer queries

#### `SpeedSelectorStatus snapshot() const`
Returns the **committed** state from the last `update()` — does not re-query sources.

#### `SpeedSelectorStatus snapshotAt(uint32_t nowMs) const`
Pure point-in-time view — re-queries sources at `nowMs` without mutating counters or state. For HTTP API and diagnostic UI.

#### `SpeedSelection selectedSpeed() const`
Inline accessor for the committed selection.

### Static helpers

#### `static const char* sourceName(SpeedSource source)`
Logging name: `"obd"`, `"gps"`, or `"none"`.

## Dependencies

| Dependency | Purpose |
|---|---|
| `ObdRuntimeModule*` | Primary speed source — calls `getFreshSpeed(...)`. |
| `GpsRuntimeModule*` | Secondary speed source — calls `getFreshSpeed(...)` and `snapshot(nowMs)` to gate on stable fix / sats / HDOP. |

## Notes for maintainers

The producer / consumer split (`update()` vs `snapshot()` vs `snapshotAt()`) matters. Producers commit state once per loop; consumers read either the committed state (cheap) or a point-in-time view (no mutation, safe to call from API handlers). Don't add side effects to the consumer paths.

`SpeedSource::OBD = 3` is deliberately not adjacent to `GPS = 1`. The gap reserves enum values for future CAN / accelerometer sources without renumbering. If you add a source, pick a fresh value rather than reusing a reserved slot.

The 250 mph ceiling filters out garbage decode (OBD adapters occasionally return wild values during discovery; GPS can briefly report bogus speeds during fix acquisition). Consumers can rely on `selectedSpeed_.valid` being false above that.

OBD always preempts GPS — even if GPS is currently selected and OBD returns a fresh sample, the very next `update()` switches the selection. This is intentional: OBD is the more accurate vehicle-bus source and GPS is fallback only. Do not add hysteresis between sources without explicit product sign-off; spurious switches are visible to consumers via `sourceSwitches`.

When neither source qualifies, the selector reports `NONE` and the speed-mute module fails open (does not mute). Don't change that — fail-open on speed source loss is in the speed-mute design rules.

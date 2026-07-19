# gps Module API

GPS receiver integration — non-blocking NMEA ingest from a serial GPS module (Adafruit Ultimate GPS v3 / MTK3339), parse RMC + GGA sentences, publish UTC time + geo snapshots, expose runtime status to the HTTP API and a ring-buffer observation log.

This module is the source of truth for GPS data across the device: ALP CSV logs and perf CSV logs both read UTC from `GpsTimePublisher`. Speed-mute can optionally use GPS speed (via `SpeedSourceSelector` — see `src/modules/speed/`).

## Files

| File | Class / role |
|---|---|
| `gps_runtime_module.{h,cpp}` | Main runtime — UART ingest, NMEA parse, status snapshot, sample injection. |
| `gps_publishers.{h,cpp}` | Single-writer/many-reader atomic snapshots: `GpsTimePublisher`, `GpsGeoPublisher`. |
| `gps_runtime_status.h` | Pure-data `GpsRuntimeStatus` struct returned by `snapshot()`. No Arduino dependency. |
| `gps_observation_log.{h,cpp}` | Bounded ring buffer (capacity 64) of recent observations. |
| `gps_api_service.{h,cpp}` | HTTP handlers for `/api/gps/config` and `/api/gps/status`. |

## Hardware wiring

From `gps_runtime_module.h:54-59`:

- GPS TX → GPIO 1 (Serial1 RX).
- GPS RX ← GPIO 5 (Serial1 TX).
- EN pin not driven (`GPS_EN_PIN = -1`); 10 kΩ internal pull-up to VBAT keeps the module on by default.
- Default baud: 9600.

## Class: `GpsRuntimeModule`

**Header:** `src/modules/gps/gps_runtime_module.h:11`.

### Lifecycle

#### `void begin(bool enabled, bool enablePinActiveHigh = true, uint32_t baud = 9600, GpsTimePublisher* timePub = nullptr, GpsGeoPublisher* geoPub = nullptr)`
Wires publishers (optional) and sets baud. `enablePinActiveHigh` is retained for compatibility but is ignored because GPS EN is not driven.
**Source:** `gps_runtime_module.h:16-17`.

#### `void setEnabled(bool enabled)` / `bool isEnabled() const`
Runtime enable/disable. Each transition increments the `enableTransitions` counter in the status snapshot.
**Source:** `gps_runtime_module.h:18`.

#### `void setBaud(uint32_t baud)`
Updates stored baud; takes effect on the next `setEnabled(true)` cycle (does not re-init the running UART).
**Source:** `gps_runtime_module.h:19`.

#### `void setEnablePinActiveHigh(bool activeHigh)`
Deprecated no-op compatibility setter. Supported wiring leaves GPS EN on its pull-up.
**Source:** `gps_runtime_module.h:20`.

### Pump

#### `void update(uint32_t nowMs)`
Non-blocking UART/NMEA ingest with bounded per-loop processing. Call once per main-loop tick.
**Source:** `gps_runtime_module.h:24`.

### Sample injection

#### `void setScaffoldSample(float speedMph, bool hasFix, uint8_t satellites, float hdop, uint32_t timestampMs, float latitudeDeg = NAN, float longitudeDeg = NAN, float courseDeg = NAN)`
Manual sample injection — used by API/tools and test scaffolding.
**Source:** `gps_runtime_module.h:27-28`.

### Queries

#### `bool getFreshSpeed(uint32_t nowMs, float& speedMphOut, uint32_t& tsMsOut) const`
Returns true with the latest speed if the most recent sample is younger than `SAMPLE_MAX_AGE_MS` (3000 ms).
**Source:** `gps_runtime_module.h:29`.

#### `GpsRuntimeStatus snapshot(uint32_t nowMs) const`
Builds a full `GpsRuntimeStatus` snapshot. Used by `/api/gps/status`.
**Source:** `gps_runtime_module.h:30`.

### Static helpers

#### `static bool parseRmcDateTime(const char* timeField, const char* dateField, int64_t& epochMsOut)`
Parses RMC UTC time + date into Unix epoch ms. Public + static for direct test coverage.
**Source:** `gps_runtime_module.h:34`.

### Test seams (UNIT_TEST only)

#### `void clearSample()` — clears all GPS sample state to simulate fix drop.
#### `bool injectNmeaSentenceForTest(const char* nmeaSentence, uint32_t nowMs)` — feeds raw NMEA into the parser without UART.

**Source:** `gps_runtime_module.h:41`.

### Constants

- `SAMPLE_MAX_AGE_MS = 3000` — speed sample freshness budget.
- `DETECTION_TIMEOUT_MS = 60000` — module-detection timeout.
- `GPS_BAUD = 9600` — default baud rate.

**Source:** `gps_runtime_module.h:13, 57`.

## Struct: `GpsRuntimeStatus`

**Header:** `gps_runtime_status.h`.

Pure-data status struct. **No Arduino dependency** — safe to include in native unit tests. Both real header and all test mocks must include this file rather than redefining the struct.

Categorized fields (see header for the full list):

- *Core fix state* — `enabled`, `sampleValid`, `hasFix`, `stableHasFix`.
- *Position / motion* — `speedMph`, `satellites`, `stableSatellites`, `hdop`, `locationValid`, `latitudeDeg`, `longitudeDeg`, `courseValid`, `courseDeg`.
- *Ages* — `courseSampleTsMs`, `courseAgeMs`, `sampleTsMs`, `sampleAgeMs`, `fixAgeMs`, `stableFixAgeMs`.
- *Parser / hardware telemetry* — `injectedSamples`, `moduleDetected`, `detectionTimedOut`, `parserActive`, `hardwareSamples`, `bytesRead`, `sentencesSeen`, `sentencesParsed`, `parseFailures`, `checksumFailures`, `sentencesUnknown`, `bufferOverruns`, `lastSentenceTsMs`, `lastSentenceAgeMs`, `firstFixMs`, `enableTransitions`.

**Source:** `gps_runtime_status.h`.

## Class: `GpsTimePublisher`

**Header:** `src/modules/gps/gps_publishers.h:56`.

Atomic UTC time snapshot. Single writer (`GpsRuntimeModule`), many readers (perf CSV logger, ALP CSV logger).

#### `static constexpr uint32_t kStaleMs = 30000`
30 s staleness threshold.
**Source:** `gps_publishers.h:59`.

#### `void publish(const GpsTimeSnapshot& s)`
Single-writer publish.
**Source:** `gps_publishers.h:60`.

#### `GpsTimeSnapshot read(uint32_t nowMs) const`
Read latest snapshot. `valid` flag respects `kStaleMs` against `nowMs`.
**Source:** `gps_publishers.h:64`.

#### `bool readUtc(uint32_t nowMs, uint64_t& utcEpochMsOut) const`
Convenience — reads UTC epoch ms if non-stale.
**Source:** `gps_publishers.h:68`.

### `struct GpsTimeSnapshot`
Fields: `valid`, `capturedMs`, `utcEpochMs`, `source` (`1` = RMC). **Source:** `gps_publishers.h:31-36`.

## Class: `GpsGeoPublisher`

**Header:** `src/modules/gps/gps_publishers.h:88`.

Atomic position/course/speed snapshot. Current header notes that no production code consumes the geo publisher yet; GPS speed used by speed selection is read directly from `GpsRuntimeModule::getFreshSpeed()`.

#### `static constexpr uint32_t kStaleMs = 5000`
5 s staleness.
**Source:** `gps_publishers.h:91`.

#### `void publish(const GpsGeoSnapshot& s)`
**Source:** `gps_publishers.h:60`.

#### `GpsGeoSnapshot read(uint32_t nowMs) const`
**Source:** `gps_publishers.h:64`.

#### `bool fresh(uint32_t nowMs) const`
True if last publish is within `kStaleMs`.
**Source:** `gps_publishers.h:98`.

### `struct GpsGeoSnapshot`
Fields: `valid`, `capturedMs`, `latitudeDeg`, `longitudeDeg`, `courseValid`, `courseDeg` (0..360 true north), `speedValid`, `speedMph`, `satellites`, `hdop`, `hasFix`. **Source:** `gps_publishers.h:38-50`.

## Class: `GpsObservationLog`

**Header:** `src/modules/gps/gps_observation_log.h:32`.

Bounded ring buffer (capacity 64) of recent observations. Used for offline analysis and native tests; no `/api/gps/observations` HTTP route is currently registered.

### Public types

#### `struct GpsObservation`
**Source:** `gps_observation_log.h:12-22`.

Per-observation snapshot — `tsMs`, `hasFix`, `speedValid`, `speedMph`, `satellites`, `hdop`, `locationValid`, `latitudeDeg`, `longitudeDeg`.

#### `struct GpsObservationLogStats`
**Source:** `gps_observation_log.h:24-28`.

`published`, `drops`, `size`.

### Methods

#### `static constexpr size_t kCapacity = 64`
**Source:** `gps_observation_log.h:34`.

#### `void reset()` — clear the ring.
#### `bool publish(const GpsObservation& observation)` — append.
#### `size_t copyRecent(GpsObservation* out, size_t maxCount) const` — copy out the most recent N observations.
#### `GpsObservationLogStats stats() const` — counters.

**Source:** `gps_observation_log.h:37`.

## Namespace: `GpsApiService`

**Header:** `src/modules/gps/gps_api_service.h`.

HTTP handlers for `/api/gps/config` (GET/POST) and `/api/gps/status` (GET).

### Public types

#### `struct Runtime`
**Source:** `gps_api_service.h:10-14`.

Callbacks/context: `void (*markUiActivity)(void* ctx)` for keep-alive signaling, `void* ctx`, and `bool maintenanceBootActive` — when set, `/api/gps/status` returns the structured maintenance 409 and config saves skip live UART re-init (persist-only).

### Handlers

- `void handleApiConfigGet(WebServer&, SettingsManager&, const Runtime&)` — `GET /api/gps/config`.
- `void handleApiConfigSave(WebServer&, SettingsManager&, GpsRuntimeModule*, const Runtime&)` — `POST /api/gps/config`; a null runtime is valid only for maintenance-mode persistence-only saves.
- `void handleApiStatus(WebServer&, GpsRuntimeModule*, const Runtime&)` — `GET /api/gps/status`; maintenance mode takes precedence over the null-runtime 503.

All three call `markUiActivity` on entry.

**Source:** `gps_api_service.h:16-26`.

## Dependencies

| External | Used by |
|---|---|
| Hardware UART1 (RX=GPIO 1, TX=GPIO 5, default 9600 baud) | `GpsRuntimeModule`. |
| `SettingsManager` | API service (config get/save). |
| `WebServer` | API service. |
| FreeRTOS spinlock primitives | Publishers, observation log (replaced with `std::atomic` under `UNIT_TEST`). |

## Notes for maintainers

The publishers exist to decouple producer (GPS module) from consumers (CSV loggers) without forcing all of them onto a shared mutex. The single-writer model is a real constraint — only `GpsRuntimeModule` calls `publish()`. If you add a new producer, the locking model needs to be revisited, not just extended.

`GpsRuntimeStatus` lives in its own header (`gps_runtime_status.h`) specifically to keep it out of the heavy include graph. Don't move it back into `gps_runtime_module.h` — native tests rely on its Arduino-free form.

Phase 1 of geo publication has no consumers per the header comment; the publisher exists so heading-aware features can be added later without re-plumbing the data flow.

The detection timeout (`DETECTION_TIMEOUT_MS = 60000`) marks the module as `detectionTimedOut` if no NMEA sentences arrive within a minute of `setEnabled(true)`. This is the right signal for "GPS hardware not connected" diagnosis from the status endpoint.

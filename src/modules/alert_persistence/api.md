# alert_persistence Module API

Manages V1 radar alert "persistence" — the brief visual fade where a recently-cleared alert stays on screen after V1 stops reporting it. The persistence duration is user-configurable (0–5 sec via `V1Settings::alpAlertPersistSec`). This module is for **V1 radar** alert persistence only; ALP laser alert persistence is handled separately in `src/modules/alp/alp_event_latch.{h,cpp}`.

## Class: `AlertPersistenceModule`

**Header:** `src/modules/alert_persistence/alert_persistence_module.h`

### Lifecycle

#### `AlertPersistenceModule()`
Default constructor. Dependencies are null until `begin()` is called.
**Source:** `alert_persistence_module.h:24`.

#### `void begin(V1BLEClient* ble, PacketParser* parser, V1Display* display, SettingsManager* settings)`
Injects dependencies. Call once from `setup()`.
**Source:** `alert_persistence_module.h:27`.

### Persistence control

#### `void setPersistedAlert(const AlertData& alert)`
Stores the alert to be persisted. Typically called with the last active alert just before V1 clears all alerts.
**Source:** `alert_persistence_module.h:30`.

#### `void startPersistence(unsigned long now)`
Begins the persistence window using `now` as the reference time. After this, `shouldShowPersisted()` returns true until `persistMs` has elapsed.
**Source:** `alert_persistence_module.h:31`.

#### `void clearPersistence()`
Ends persistence early. Used when a new alert arrives or persistence is otherwise pre-empted.
**Source:** `alert_persistence_module.h:32`.

### Persistence query

#### `bool shouldShowPersisted(unsigned long now, unsigned long persistMs) const`
Returns true if persistence is active and `now - alertClearedTime_ < persistMs`. Caller passes `persistMs` from settings; the module does not read settings itself per call.
**Source:** `alert_persistence_module.h:33`.

#### `const AlertData& getPersistedAlert() const`
Returns the stored persisted alert. Only meaningful while `isPersistenceActive()` is true.
**Source:** `alert_persistence_module.h:34`.

#### `bool isPersistenceActive() const`
Returns true between `startPersistence()` and `clearPersistence()`.
**Source:** `alert_persistence_module.h:35`.

## Dependencies

| Dependency | Purpose |
|---|---|
| `V1BLEClient*` | Connection state for sequencing decisions. |
| `PacketParser*` | Source of `AlertData`. |
| `V1Display*` | Render coordination during persistence window. |
| `SettingsManager*` | Reads `alpAlertPersistSec`. |

## Notes for maintainers

The persistence window is purely time-based: there is no acknowledgment or render-loop callback. Callers must ask `shouldShowPersisted()` each render cycle and act on the answer. There is no internal timer that fires when the window expires — expiration is detected only on the next query.

This module's responsibilities are limited to V1 alert persistence. Local voice playback has been retired.

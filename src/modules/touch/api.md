# touch Module API

Touchscreen and BOOT-button gesture handling. Two cooperating modules: `TapGestureModule` (display-area taps and maintenance long-presses) and `TouchUiModule` (BOOT button for brightness/volume adjust UI plus maintenance entry and the OBD pair gesture).

## Class: `TapGestureModule`

**Header:** `src/modules/touch/tap_gesture_module.h:14`.

Display-area taps. Triple-tap for profile change (`PROFILE_CHANGE_TAP_COUNT = 3`); 4-second long-press requests maintenance boot unless WiFi is already active, in which case it stops WiFi.

### Public types

#### `struct WifiCallbacks`
**Source:** `tap_gesture_module.h:16-22`.

Function-pointer hooks for WiFi state / maintenance entry — `isWifiActive`, `stopWifi`, `requestMaintenanceBoot`, plus context pointers.

### Lifecycle

#### `void begin(TouchHandler*, SettingsManager*, V1Display*, V1BLEClient*, PacketParser*, AutoPushModule*, AlertPersistenceModule*, DisplayMode*, QuietCoordinatorModule*, const WifiCallbacks& = {})`
Wires the wide dependency surface (taps can affect almost any subsystem).
**Source:** `tap_gesture_module.h:25-34`.

### Pump

#### `void process(unsigned long nowMs)`
Per-loop tick. Reads touch events, updates tap-counting state, fires gestures.
**Source:** `tap_gesture_module.h:36`.

### Constants

- `PROFILE_CHANGE_TAP_COUNT = 3` — triple-tap profile cycle.
- `TAP_WINDOW_MS = 600` — multi-tap window.
- `TAP_DEBOUNCE_MS = 150` — single-tap debounce.
- `LONG_PRESS_WIFI_MS = 4000` — long-press maintenance-entry / WiFi-stop threshold.

**Source:** `tap_gesture_module.h:48-58`.

## Class: `TouchUiModule`

**Header:** `src/modules/touch/touch_ui_module.h:11`.

BOOT button UI — brightness slider, volume slider, maintenance boot entry (long press), OBD pair gesture (extra-long press).

### Public types

#### `struct Callbacks`
**Source:** `touch_ui_module.h:14-30`.

Function-pointer hooks for WiFi setup, display indicator drawing, OBD scan, and gesture-safety checks.

### Lifecycle

#### `void begin(V1Display*, TouchHandler*, SettingsManager*, const Callbacks&)`
**Source:** `touch_ui_module.h:32-35`.

### Pump

#### `bool process(unsigned long nowMs, bool bootPressed)`
Returns true if the UI consumed the loop (brightness/volume adjustment was active). Caller can use this to skip downstream work.
**Source:** `touch_ui_module.h:38`.

### Constants

- `BOOT_DEBOUNCE_MS = 300` — boot-button debounce.
- `MAINTENANCE_BOOT_LONG_PRESS_MS = 4000` — maintenance reboot threshold on release.
- `OBD_PAIR_LONG_PRESS_MS = 10000` — OBD pair gesture (10 s).
- `DOUBLE_PRESS_WINDOW_MS = 600`
- `VOLUME_TEST_DEBOUNCE_MS = 1000`
- `SLIDER_REDRAW_MIN_MS = 50` — caps slider redraw at ~20 Hz.

**Source:** `touch_ui_module.h:55-60`.

## Dependencies

| Dependency | Purpose |
|---|---|
| `TouchHandler` (`src/touch_handler.{h,cpp}`) | Low-level touch event source. |
| `V1Display`, `SettingsManager`, `V1BLEClient`, `PacketParser`, `DisplayMode` | Wide gesture surface. |
| `AutoPushModule`, `AlertPersistenceModule`, `QuietCoordinatorModule` | Side effects of taps. |
| `ObdRuntimeModule` (callbacks) | OBD pair gesture. |
| WiFi / maintenance callbacks | Stop already-active WiFi, or request maintenance boot. |

## Notes for maintainers

The tap gesture and BOOT-button gesture have separate long-press thresholds (4 s and 10 s). The BOOT button uses both — short press for brightness/volume slider, release after 4 s for maintenance boot, 10 s for OBD pair. Maintenance fires on release so a user can continue holding to the OBD gesture without rebooting at 4 s.

The `TouchUiModule::process()` return value gates the rest of the loop. When the user is adjusting brightness/volume, the loop short-circuits to keep slider redraw responsive. Don't add work above the touch UI in the main loop that the UI suppression depends on running.

OBD pair gesture has its own safety check (`isObdPairGestureSafe` callback) — the gesture is not admissible during alerts, previews, or boot splash. If you add a new state where the gesture should be locked out, route it through that callback rather than adding parallel checks.

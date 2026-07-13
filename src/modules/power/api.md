# power Module API

Power-state coordination — battery polling, low/critical battery handling, graceful shutdown, auto-power-off after V1 (and ALP) signals go away. Coordinates with `BatteryManager` (low-level battery I/O) and `V1Display` (warning UI).

In car-install builds (`CAR_MODE_PWR_SHORT`), much of this module is compiled to no-ops because the device is permanently powered from the vehicle's ignition rail — see `docs/HARDWARE_NOTES.md`.

## Class: `PowerModule`

**Header:** `src/modules/power/power_module.h:9`.

### Public types

#### `using ShutdownPreparationCallback = void (*)(void*)`
Function-pointer signature for shutdown preparation hooks.
**Source:** `power_module.h:11`.

### Lifecycle

#### `void begin(BatteryManager* batteryMgr, V1Display* disp, SettingsManager* settings)`
Wires dependencies. Call once from `setup()`.
**Source:** `power_module.h:13-15`.

#### `void setShutdownPreparationCallback(ShutdownPreparationCallback callback, void* context)`
Registers a hook that runs before `performShutdown()` actually drops power. Use for last-second cleanup (flush logs, save state, etc.).
**Source:** `power_module.h:17`.

### Shutdown control

#### `void performShutdown()`
Graceful shutdown — fires the preparation callback, then drops the latch.
**Source:** `power_module.h:20`.

In `CAR_MODE_PWR_SHORT` builds, this is a no-op (`docs/HARDWARE_NOTES.md`).

### Notifications

#### `void logStartupStatus()`
Logs initial battery status. Call once after display init.
**Source:** `power_module.h:23`.

#### `void onV1DataReceived()`
Mark that real V1 data has been seen. Arms auto-power-off on subsequent V1 disconnect.
**Source:** `power_module.h:26`.

#### `void onV1ConnectionChange(bool connected)`
Notifies of V1 BLE connect/disconnect. Drives the auto-power-off timer.
**Source:** `power_module.h:29`.

#### `void onAlpSignalChange(bool active)`
Notifies of ALP heartbeat presence. ALP heartbeats also count as "device in use" for auto-power-off purposes — losing both V1 and ALP signal triggers the timer.
**Source:** `power_module.h:32`.

### Pump

#### `void process(unsigned long nowMs)`
Per-tick: battery polling, critical-shutdown check, auto-power-off timer evaluation.
**Source:** `power_module.h:35`.

### Test seams (UNIT_TEST only)

- `bool lowBatteryWarningShownForTest() const`
- `unsigned long autoPowerOffTimerStartForTest() const`
- `bool autoPowerOffArmedForTest() const`
- `void performShutdownRequestForTest()` — bypasses platform shutdown to exercise the request path.

**Source:** `power_module.h:38-42`.

## Dependencies

| Dependency | Purpose |
|---|---|
| `BatteryManager*` | Voltage polling, latch control. |
| `V1Display*` | Low-battery warning UI, shutdown screen. |
| `SettingsManager*` | Reads `autoPowerOffMinutes`, `powerOffSdLog`, etc. |

## Notes for maintainers

The "auto power-off armed" semantic is: "we have seen real V1 data at least once, so we know the user is using the device." Losing the signal then triggers the timer. Without this gate, devices that boot and never see a V1 would auto-power-off mid-search.

Both V1 and ALP presence count toward "device in use." If the user has only ALP attached (no V1 paired), `onAlpSignalChange(true)` keeps the timer disarmed. Don't simplify this to V1-only without first checking ALP-only install scenarios.

The shutdown preparation callback is the one chance for other modules to flush state. SD loggers and settings writers should hook here. Don't add long-running work — the callback is on the path to actually dropping power.

In `CAR_MODE_PWR_SHORT` builds, several methods compile to no-ops via preprocessor gates (see the .cpp). If you add a method that should also be a no-op in car mode, follow the pattern in the existing implementation file.

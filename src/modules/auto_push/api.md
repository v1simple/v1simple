# auto_push Module API

Encapsulates the V1 profile auto-push state machine. When a slot is activated (manually or by the auto-push trigger), this module sequences the multi-step BLE conversation with the V1 to upload the profile, validate the writeback, and apply mode/volume settings — coordinated with the quiet-coordinator module so other BLE traffic doesn't interleave.

## Class: `AutoPushModule`

**Header:** `src/modules/auto_push/auto_push_module.h:15`.

### Public types

#### `enum class QueueResult : uint8_t`
Result of a queue call.
**Source:** `auto_push_module.h:17-23`.

- `QUEUED` — request accepted into the executor.
- `V1_NOT_CONNECTED` — no V1 BLE link.
- `ALREADY_IN_PROGRESS` — another push is currently running.
- `NO_PROFILE_CONFIGURED` — slot has no profile.
- `PROFILE_LOAD_FAILED` — profile load from storage failed.

#### `struct PushNowRequest`
**Source:** `auto_push_module.h:25-32`.

Bundle for the `queuePushNow` entry point:
- `int slotIndex` — slot to push from (0–2).
- `bool activateSlot` — whether to mark the slot active on success.
- `bool hasProfileOverride`, `String profileName` — optional profile-name override.
- `bool hasModeOverride`, `V1Mode mode` — optional mode override.

### Lifecycle

#### `AutoPushModule()`
Default constructor; dependencies null until `begin()`.
**Source:** `auto_push_module.h:34`.

#### `void begin(SettingsManager* settings, V1ProfileManager* profileMgr, V1BLEClient* ble, V1Display* disp, QuietCoordinatorModule* quietCoordinator)`
Injects dependencies. Call once from `setup()`.
**Source:** `auto_push_module.h:36-40`.

### Queue entry points

#### `QueueResult queueSlotPush(int slotIndex, bool activateSlot = false, bool updateProfileIndicator = true)`
Queues a slot-driven push through the shared executor. Used by the auto-push trigger and by the slot-activate API endpoint.
**Source:** `auto_push_module.h:43-45`.

#### `QueueResult queuePushNow(const PushNowRequest& request)`
Queues an explicit push-now request with optional profile/mode overrides. Used by the `/api/autopush/push` endpoint.
**Source:** `auto_push_module.h:48`.

### Pump

#### `void process()`
Advances the state machine. Call once per main-loop tick.
**Source:** `auto_push_module.h:51`.

### Status

#### `String getStatusJson() const`
Returns a JSON status string consumed by `/api/autopush/status`.
**Source:** `auto_push_module.h:54`.

#### `bool isActive() const`
True when the state machine is not idle.
**Source:** `auto_push_module.h:56`.

## Internal state machine (informational)

The `Step` enum (`auto_push_module.h:62-70`) defines the push sequence: Idle → WaitReady → Profile → ProfileReadback → Display → Mode → Volume. Retry limits:

- `kMaxProfileWriteRetries = 5` — profile write attempts before failure.
- `kMaxPushNowCommandRetries = 8` — per-command retry budget for push-now requests (higher than slot-driven because push-now is user-initiated and worth more patience).

These constants are private but documented here for diagnostic context — they explain timeout shapes in serial logs.

## Dependencies

| Dependency | Purpose |
|---|---|
| `SettingsManager*` | Reads slot configurations from `V1Settings::autoPushSlots`. |
| `V1ProfileManager*` | Loads profile bytes by name. |
| `V1BLEClient*` | BLE write / readback transport to V1. |
| `V1Display*` | Updates profile indicator and status during push. |
| `QuietCoordinatorModule*` | Reserves the BLE channel during push so other traffic is suppressed. |

## Notes for maintainers

The state machine is timing-sensitive — `nextStepAtMs` schedules each step relative to `millis()`. Don't bypass `process()` to drive transitions directly.

`applySlotMuteToZero()` is private but interacts with V1 user-settings encoding; if you change V1 settings byte layout, this method needs to be reviewed.

Push-now requests carry overrides; slot-driven pushes use the slot's stored profile and mode. Both go through the same `queuePreparedSlot()` private path so retry behavior is uniform.

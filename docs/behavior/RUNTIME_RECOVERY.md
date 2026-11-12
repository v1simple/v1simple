# Runtime and recovery boundaries

## Screen ownership and live policy

`DriveLoopCoordinator::tick` delivers ALP input and heartbeat presence to
`PowerModule` before checking an auto-off expiry on every drive tick. A newly
arriving heartbeat therefore cancels the timer even at its expiry boundary.
Sliders retain their presentation ownership while the real parsed-frame quiet
policy runs through `DriveRuntime::processParsedPolicies`. Its existing parsed
edge, boot hold, phone ownership and detector-operation gates still apply.
The forced BLE tail drain remains active; queued V1 input can preempt sliders
on the following tick.

Slider paint is throttled. A pending brightness adjustment still reaches PWM
when BOOT closes the menu or an alert takes over, without another slider
frame. A power warning can suspend the menu and restore its pending brightness.
Only fields the user edited are staged for persistence. An edit made before
settings recovery completes stays in RAM and merges into recovered settings
before the next save. Recovery refreshes unedited sliders and preserves an
open preview's actual edits. Alert preemption schedules pending persistence
without opening storage on the presentation path.

`PowerModule::process` also reconciles recovered settings with current presence.
An armed, absent detector with a newly enabled interval starts a full interval
from recovery. Repeated ticks do not renew a running timer. Disabled auto-off
cancels it; car builds retain their no-auto-off contract.

Power captures its own 32-bit clock after battery/button work, instead of using
the earlier loop timestamp. An ALP loss during the pass therefore starts a full
inactivity interval. A slow ADC trigger also precedes the critical-warning
anchor, so that acquisition cannot confirm its own warning. After the full
five-second warning, a distinct successful low reading is still required;
an unavailable, zero or recovered reading cancels shutdown. A distinct reading
already acquired during the warning may be reused.

`test_drive_runtime_policies.py` exercises real touch, coordinator, ALP/power,
quiet policy and parser/queue owners. `test_settings_runtime_recovery.py`
executes actual failed NVS read, settings recovery and timer expiry. Peripheral
input and output are modeled; these tests do not qualify a physical ALP, GPS or
detector connection.

The ALP composition includes advancing phase clocks in normal/sliders views
and at rollover. `test_power_runtime_lifecycle.py` executes actual battery
acquisition/cache methods and PowerModule through the drive bridge and complete
maintenance tick, including acquisition delays, grace expiry, distinct low and
recovered readings, acquisition failure, rollover and the car shutdown gate.
Its ADC/rail adapters prove the software decision, not physical cell or cutoff
behavior.

## PWR input and unavailable supply sensing

On the official Waveshare
[V1](https://files.waveshare.com/wiki/ESP32-S3-Touch-LCD-3.49/ESP32-S3-Touch-LCD-3.49-Schematic.pdf)
and [V2](https://github.com/waveshareteam/ESP32-S3-Touch-LCD-3.49-V2/blob/main/schematic/ESP32-S3-Touch-LCD-3.49%20V2.pdf)
schematics, GPIO16/SYS_OUT is pulled high and the physical PWR switch pulls it
low. This input does not identify the active supply.
`BatteryManager::powerSource` reports `Unknown`; its legacy
`isOnBattery` accessor is a conservative policy bit, not a measurement. Unknown
retains low-cell protection for valid nonzero voltage. Zero remains unavailable
ADC data. Battery presence is still a voltage heuristic.

The button remains available independently of ADC health and supply. A held
boot button must be released before a fresh hold can request shutdown. The
hardware tail attempts the latch cut, then uses its bounded sleep fallback if
the rail remains alive. It selects only a released wake input; if neither
input is available, the existing abort/recovery behavior applies.

The status API publishes `battery.source` separately from the legacy
`on_battery` field. The dashboard renders `unknown` as unavailable sensing;
older responses without the new field retain compatibility. Source logs use
the classification rather than interpreting the policy bit as measured power.

`test_battery_input_lifecycle.py` runs the actual begin, voltage refresh,
button and shutdown methods with GPIO/ADC/I2C/rail/sleep adapters. It covers
held boot, later hold, ADC failure, clock wrap, cutoff, surviving external rail
and wake-input fallback. API and dashboard tests cover reporting separately.
Neither software simulation nor a schematic establishes physical cutoff,
sleep current, wake behavior or a future supply sensor's reliability.

## Graceful persistence and interrupted recovery

`PersistentBootstrap::run()` owns the shared Drive and Maintenance storage
startup sequence: mount, open profile/device catalogs, restore settings, migrate
profiles, resolve pending device deletion, adopt the last-device fallback, then
validate profile references. Drive initializes audio after the catalogs open;
both runtimes apply theme and saved brightness only after a successful restore.
Drive starts GPS after bootstrap returns. `test_settings_runtime_recovery.py`
executes both actual runtime adapters with the real stores, including unavailable
storage, fallback/deletion authority, interrupted restore and ordered display,
audio and GPS effects; hardware effects are host substitutes.

`V1SettingsOperationStore::begin()` reads its durable record with checked NVS
results. Only `ESP_ERR_NVS_NOT_FOUND` establishes an empty operation store.
An unavailable read blocks new Apply and factory-reset requests; invalid record
types, lengths and contents remain corrupt. Neither condition overwrites the
retained bytes. The operation API reports unavailable storage rather than no
operation. A later successful initialization recovers the original operation ID
and target; this owner does not poll or automatically repeat detector writes.

`SettingsOperationCoordinator::onStableConnection()` blocks ordinary connection
Auto-Push while the operation snapshot is invalid. Drive applies the detector's
In-the-Box policy before requesting that admission decision. An invalid record
leaves the detector connected and alert input available; a valid empty store
retains normal Auto-Push, and a known pending job retains its target admission.
While settings themselves are provisional, Drive still captures observations
for the identified detector and defers ordinary connection Auto-Push. After
recovery, it rechecks the live BLE session, device identity and durable
operation admission, then resolves the slot and policy from current settings.
A retired session drops the deferred request.
The coordinator owns transient operation deadlines, capture/session boundaries,
factory-send latches and maintenance-return retries. Its `process()` services
those decisions before each AutoPush executor tick; the durable store and wire
executor retain their separate responsibilities. Drive supplies snapshot and
controlled-restart effects.

`test_v1_settings_operation`, `test_wifi_autopush_api`, and
`test_settings_apply_lifecycle.py` cover retained bytes, rejected starts, API
status, connection admission, parsed alerts and recovery through fresh owners.
The lifecycle runner executes the real coordinator and the actual Drive snapshot,
connection and restart methods. NVS, BLE transport and physical restart effects
are host substitutes, not physical fault injection.

The maintenance Backup & Restore card reads `/api/settings/recovery`, a pure
in-memory `SettingsManager::recoveryStatus()` snapshot. It separates the last
internal-save result, queued/writing/retry state, and known recovery flags
(including `restorePending_`). Only an actual synchronous or deferred backup
write with successful completion bookkeeping creates a receipt in this boot;
preserving an older recovery copy may advance the existing completion marker
without creating that receipt. Publication happens before the existing SD lock
is released. A later change makes the receipt insufficient for current state,
and a reboot starts with no observed write. Mount flags and these observations
do not establish current card contents or media health; polling performs no
storage I/O or recovery. `test_settings_deferred_backup` and
`test_backup_api_restore_path` cover the owning boundaries, and
`SettingsBackupCard.test.js` covers pending/unknown labels and stale responses.

Drive shutdown first releases competing event/health/settings writers through
the shared preparation helper. It then flushes a pending detector snapshot and
recency through `DeviceStore`, taking the nonblocking SD lock when applicable.
Only successful preparation permits the clean marker. Failure cannot veto the
physical shutdown; dirty state survives in RAM if the hardware tail aborts.
The drive lifecycle suite covers successful SD and non-SD reload plus busy
writer, SD-lock failure and write failure. It models graceful durability, not
arbitrary flash power loss.

Station connection and retry transitions live in `WifiStationLifecycle`;
`WiFiManager` retains request validation, durable credential commits, and
AP/server orchestration. Credential edits notify the lifecycle only after
successful persistence. Enable rollback restores one lifecycle state value
without copying scan-owner state. `test_wifi_client_recovery.py` exercises
the actual station owner and the manager transaction boundaries.

Saved-network Edit, Delete and Test carry the exact SSID observed for the
selected slot. A save without a slot can only create a network whose SSID is
still absent. Reorder compares every configured slot's SSID and prior priority
before writing the new order. The maintenance routes resolve storage recovery
before checking these conditions. On a conflict, Settings reads the current
catalog and waits for a new user action before trying the operation again.
The station also checks the saved identity before starting an association for
that slot and before accepting its result. A restore that replaces the slot
invalidates the older attempt. A label-only edit preserves an untouched
password, including when a different tab secured a formerly open network.

WiFi rollback discovery in `settings_nvs.cpp` distinguishes proven absence,
single-slot rollback, forget-all rollback and unavailable authority. A failed
namespace/key read or invalid header cannot be treated as absence. Set, delete,
forget-all, legacy setters, HTTP mutation admission, backup import and SD
recovery all require successful resolution before mutation. A raw journal
writer cannot replace a ready or unavailable journal. Installed legacy journals
without a mode field remain compatible when that field is proven absent.
`test_settings_persistence` covers these paths through retry and fresh-manager
reload with bounded NVS faults, including the three-read-fault HTTP regression.

Full settings restore and profile deletion reserve their local transaction in
NVS before writing a rollback journal. They authorize external writes only
after the journal copies have been verified. The journal identifies its board
and its LittleFS or SD participants. A separate witness on the SD card allows
recovery from a healthy mirror when that card's journal payload is corrupt.
Profile startup checks the participants before reconciling catalogs, including
on deferred startup retry. A detached or different card leaves the rollback
pending and blocks conflicting mutations. LittleFS never receives an SD-only
credential snapshot. An unavailable restore credential also defers saved
network Test and automatic association.

After all participants converge, recovery records completion in NVS before
removing the journal copies. A retained copy can then be cleaned up without
replaying its payload over a later acknowledged save. Credential rollback
requires proven key absence or exact value readback, including for open
networks. Tests cover missing and wrong media, interrupted preparation and
cleanup, NVS read failure and successful retry through fresh owners. These
checks model the stores and reboot boundaries.

Version 2 and 3 rollback journals lack a board and media witness. A legacy
credential snapshot that identifies LittleFS can recover there. Other pending
legacy journals require matching SD and LittleFS copies. An unpaired legacy
journal is preserved because earlier firmware could leave a stale SD copy
after accepting newer writes. A modern pending journal whose local NVS owner
was erased is also preserved for explicit recovery. Keep the storage and
rollback evidence together when investigating either case.

Settings mutations in `settings_setters.cpp` validate every String in the
rollback snapshot before changing live values, including scalar-only updates.
`finishSettingsMutation` moves the validated snapshot back after a failed
immediate save without another allocation; an earlier deferred mutation retains
its pending state and original retry time. WiFi priority validation uses the
same snapshot protection on early rejection. `test_settings_deferred_persist`
covers all generic mutation owners and the audio HTTP save through NVS and fresh
load, with modeled String-copy and storage-write failures. Rejected saves and
later successful saves must preserve unrelated configuration.

The pinned NimBLE NVS initializer publishes a complete-load result. Normal
boot stops before bond migration, backup, proxy advertising or client creation
if any bond, subscription or peer-identity table could not be read. The existing
visible BLE boot-error path retries on reboot; partial RAM tables cannot replace
the complete SD recovery source. Empty OUR/PEER tables initialize their recency
counters to zero without reading before the array. `test_ble_bond_bootstrap.py`
compiles the pinned loader, actual `initBLE` owner and real backup writer with
modeled NVS and radio adapters. It covers first/later read failure, every table,
empty and one-sided tables, successful retry, new pairing and intentional
bond deletion. It does not inject physical NVS faults or prove radio recovery.

Normal V1 link retirement preserves the detector's saved bond, including a
controller supervision timeout and local or peer termination. Only an explicit
PIN/key-missing disconnect queues V1Simple's deferred deletion. The main loop
waits for bond recovery before deleting and scheduling a refreshed SD snapshot.
The pinned NimBLE encryption-change handler separately owns its missing-key
recovery. `test_ble_command_transport` exercises the actual callback, deferred
runtime, and new-session admission with a modeled bond table and backup queue.

Bond backups have a fixed layout: the file length must equal its header plus
exactly the declared records. A decreased count with remaining bytes cannot
masquerade as a valid empty backup and discard a valid previous generation.
`test_ble_bond_backup_writer` follows fallback restoration, new pairing,
backup replacement and a second empty-NVS boot. Valid empty/live and unavailable
read controls remain distinct. Same-size content corruption is not detected by
this format check; physical media and radio behavior are not exercised.

## Unknown maintenance settings

GPS, OBD and ALP configuration use independent instances of
`interface/src/lib/features/settings/immediateConfig.svelte.js`. That owner
controls loading, write admission and post-write readback through one phase per
instance; each card retains its payload validation and displayed field values.

The ALP page, GPS card and OBD card require complete valid configuration before
enabling edits. A failed initial read or post-save readback withdraws readiness
and offers Retry. Retry reads current settings; it never repeats a previous
write. Each explicit save remains locked through its fresh readback, including
when a response was lost after a possible commit. ALP handles its initial-load
rejection locally rather than leaving an unhandled asynchronous error.

OBD configuration and the saved-adapter catalog have independent readiness and
errors. An unknown catalog cannot authorize rename or forget, while a known
configuration remains usable. Catalog mutations also reread the authoritative
list before restoring controls. Existing rename drafts survive unrelated
configuration readback and are cleared only by their own completed operation.

`SettingsReadRecovery.test.js` exercises the real Svelte components with failed,
incomplete and malformed reads, repeated Retry, successful recovery, uncertain
writes and readback failure. The OBD page and settings-transition regressions
cover canonical values, rename drafts and concurrent interaction. HTTP is
modeled in these component tests; they do not establish device persistence.

## Long replies and presentation fallbacks

Acknowledged command writes in `ble_commands.cpp` use the same attribute-worker
lifetime gate as discovery and subscription. The main loop retains one copied
packet, caller owner and detector/phone generations. `IN_FLIGHT` means admitted;
`NOT_YET` means no write was admitted; only `SENT` publishes a completed write.
Another caller cannot consume the completion. A changed or canceled admitted
intent retires the session, and cleanup retains the client and characteristic
until the worker exits. A one-second command deadline starts quiescence before
the loop watchdog can expire. The existing bounded quiescence recovery still
owns an unresponsive disconnect.

Only no-response writes fitting the current ATT MTU stay on the main loop.
NimBLE can promote larger no-response writes to acknowledged long writes, so
raw and indexed phone commands share the worker path when required. No
ambiguous acknowledged write is automatically retried on the outgoing session.

Connected follow-up and settings Apply arm reply capture on first admission,
retain observations parsed before acknowledgment and qualify them against the
worker's actual pre-write ingress. Completion does not reset those captures.
An early canonical volume rejection remains attached to the completed write's
receipt. Quiet policies, tap mute, alert-stream recovery and detector reset
retain pending intent until acknowledgment or explicit cancellation; incoming
feedback alone cannot abandon an admitted command.

`DriveRuntime::processBleRuntime` services admitted alert-stream recovery on
every loop, including while sliders, preview or a power warning owns the
screen. `ConnectionStateModule::servicePendingAlertRecovery` polls or cancels
only that pending command; it neither renders nor starts a recovery request.
Initial requests retain the connection-display cadence, and the transport's
one-second timeout still retires blocked work. The `alert_recovery` fixture in
`test_drive_runtime_policies.py` executes the real BOOT touch handler, drive-loop
coordinator, extracted runtime adapters, command worker, queue/parser and
connection owner. It covers successful completion, slot release, suppressed
presentation, phone stop, timeout, retired sessions and live-alert preemption.
Radio, panel, clock and unrelated BLE service remain host substitutes.

Detector reset latches admission separately from acknowledgment. Losing that
job or its session yields an unverified result, including when persisting the
failure must be retried; reconnect never resends the destructive command.
An interrupted automatic In The Box mute retains a release obligation only
for the same known detector. Fresh feedback resolves it after reconnect;
another detector, phone ownership or an explicit local mute choice revokes it.
Detector-side manual changes while disconnected are indistinguishable from an
applied command whose acknowledgment was lost.

`test_ble_command_transport` executes the production sender, worker, connection
cleanup, notification queue and parser with a deliberately blocked transport.
Caller regressions in AutoPush, quiet/tap/connection suites and the settings
Apply lifecycle cover early replies, cancellation and completion ownership.
These host tests model NimBLE and FreeRTOS. The bench's optional
`--command-ack-hold-ms` mode supplies a separate physical ESP32/CoreBluetooth
command-acknowledgment check; it does not inject a subscription acknowledgment
failure or establish real-detector RF/audio behavior.

If an advertised, notifiable B4E0 channel rejects subscription,
`ble_subscription.cpp` returns failure into the existing quiesce/reconnect
path. Absence or a non-notifiable optional channel retains compatibility.
`test_ble_subscription` includes a scripted reconnect followed by actual long
notification queue/reassembly/parser delivery and complete sweep capture.
Radio reconnect scheduling itself is not simulated by that test.

The segment fallback includes G and Y, including lowercase, so font failure
does not drop letters from ALP labels. `test_display_rendering_frequency`
checks actual fallback canvas strokes for the labels without smooth-font calls.
`audio_voice.cpp` appends number/noun pairs atomically for the full 1–15 alert
count domain across voice entrypoints. `test_audio_worker` follows decoded
counts through the pipeline, voice builder and worker, preserving count-off,
single-bogey, mode/direction and laser controls. These host tests do not inspect
physical pixels or listen to the speaker.

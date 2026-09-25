# V1 alert reception and AutoPush response freshness

Behavior traced at `daf32ed`. These are two repairs sharing the BLE ingress
path. Read the owning functions below before changing that path; packet arrival,
queue processing, command completion, and verified application are separate events.

## Shared path and ownership

| Boundary | Owner and behavior |
|---|---|
| BLE callback enters | [`V1BLEClient::notifyCallback`](../../src/ble_connection.cpp) stamps ingress sequence, time, and session generation before characteristic mapping or proxy work. The sequence is nonzero and wraps past zero. |
| Main-loop handoff | [`DriveRuntime::onV1Data`](../../src/drive_runtime.cpp) forwards all stamps to [`BleQueueModule::tryOnNotify`](../../src/modules/ble/ble_queue_module.cpp). Admission and consumption both reject a different session generation. |
| Notification becomes an ESP frame | `BleQueueModule::process` retains complete notification ownership, reassembles B2CE byte streams and B4E0 indexed chunks, and preserves the frame's first ingress sequence. Gaps mark alert-table discontinuity. |
| Frame becomes an observation | [`PacketParser::parseInternal`](../../src/packet_parser.cpp) validates framing and packet-specific destination/origin/width, then records canonical observations with ingress/revision. User-byte capture has a specialized canonical path in the queue. |
| Main-loop consumers run | [`DriveLoopCoordinator::tick`](../../src/runtime_coordinator.h) drains the queue before display composition and connection dispatch. AutoPush runs at the end of `DriveRuntime::presentDisplay`; notification callbacks can execute while its synchronous BLE send waits. |

[`V1PacketFraming`](../../src/v1_packet_framing.h) admits V1 origin EAh with
checksum or E9h without checksum, using the exact data width for that origin.
An EAh checksum byte is never usable as a missing data byte.

## 1. Older Gen2 detailed alert reception

### Contract and failure

`respAlertData` (`0x43`) supplies the detailed radar table. Older Gen2 responses
target requester D6; newer responses use broadcast D8. The parser previously
required D8, so a valid D6 row never reached the table or renderable priority.
The same rejection affected count-zero table clears. This did not establish
that every alert indication disappeared: `infDisplayData` has a separate path.

The repair admits D6 or D8 without waiting for version discovery. Both still
require exactly seven data bytes, V1 origin, framing and the applicable checksum.
`infV1Busy` remains D8-only. Reception does not grant permission to write settings.

### Trace through the final consumer

1. The shared callback/queue path above supplies a complete stamped frame.
2. `PacketParser::parseInternal`, `PACKET_ID_ALERT_DATA`, checks D8 or D6 and
   calls [`parseAlertData`](../../src/packet_parser_alerts.cpp).
3. `parseAlertData` assembles a complete table, discards stale cached rows, and
   honors discontinuities before publishing `alerts_` and `alertCount_`.
   It chooses row priority from aux0 bit 7, with usable-row fallbacks.
   A count-zero row clears the radar table; display-packet mute and primary
   signal bars retain their own authority. Display laser remains separately owned.
4. The queue publishes its parsed edge. `DriveRuntime::consumeDisplayEdges`
   and `presentDisplay` dispatch
   [`DisplayPipelineModule::handleParsed`](../../src/modules/display/display_pipeline_module.cpp).
5. `buildRenderFrame` obtains `getRenderablePriorityAlert`, the complete table,
   V1 display state, and persistence/ALP state. The
   [`RenderFrameComposer`](../../src/modules/display/render_frame_composer.cpp)
   selects the live primary and cards under the display ownership contract.
6. `renderComposedFrame` calls
   [`V1Display::renderFrame`](../../src/display_update.cpp). Its live V1 path
   renders priority frequency/band, V1 display bars/arrows, and secondary cards.
   A live ALP primary can instead carry V1 radar in cards. Successful parsing
   alone does not prove these final pixels were transferred to a physical panel.

### Executable proof and limits

[`test_ble_queue_alert_integration`](../../test/test_ble_queue_alert_integration/test_ble_queue_alert_integration.cpp)
uses the production queue, framing checks, parser, table assembly and priority
selection. Fixed wire vectors have independently summed checksums.

| Test | What it establishes |
|---|---|
| `test_older_gen2_targeted_alerts_and_current_broadcasts_reach_the_alert_table` | Version 4.1030 + D6 yields one priority Ka/front/34,700 MHz row; D6 clears it; 4.1031 + D8 still yields the row. |
| `test_targeted_alerts_do_not_wait_for_firmware_version_discovery` | The D6 row is renderable before version discovery. |
| `test_long_characteristic_alert_envelopes_reach_real_parser_and_clear` | Actual B4E0 indexed envelopes for D6 priority/clear and D8 control traverse production long-RX assembly and parser before version discovery. |
| `test_alert_destination_compatibility_keeps_origin_checksum_and_shape_validation` | D5, accessory E6, bad D6/D8 checksums, and a short row cannot replace or clear a valid table. |
| `test_queue_gap_prevents_cross_cycle_alert_table_publication` | A dropped notification cannot combine old and new rows into a false complete table. |
| `test_spec_busy_broadcast_reaches_request_owner_but_targeted_busy_does_not` | D6 compatibility is specific to alert data; busy flow control retains its D8 contract. |

The original five-case suite failed two cases before the fix. The additional
B4E0 test also fails against the pre-fix parser: **six tests, three failures**;
the current parser passes all six. The fixed claim is valid D6 reception into
the detailed table and renderable priority, including startup and clear behavior.

The B4E0 test uses the documented single-chunk envelope: a 14-byte EAh alert
frame fits in the 19-byte chunk payload, prefixed with index/count `0x11`.
Other cases enter as complete ESP frames through B2CE. The separate
[`test_ble_queue_ingest`](../../test/test_ble_queue_ingest/test_ble_queue_ingest.cpp)
tests indexed-chunk reassembly, stale-session rejection and queue saturation with
a parser double. Neither suite executes the real NimBLE callback or physical
panel. Qualification on an actual older Gen2 and RF/vehicle behavior remain
separate work; passing current-version replay does not supply that evidence.

## 2. AutoPush responses arriving during a synchronous send

### Contract and failure

A response that enters after a request begins may be parsed after the send
returns. A response already queued before that request must remain stale even
when parsed later. Parsing time and observation revision alone cannot distinguish
these cases.

[`V1BLEClient::sendCommandWithResult`](../../src/ble_commands.cpp) may call
`writeValue(..., true)`, waiting for an acknowledged BLE write. The prior
`CustomRefreshSections`, `CustomRefreshMax`, `CustomRefreshDefinitions` and final
`CustomWrite` commit paths sampled ingress after that send. A valid response
entering during the wait became part of the stale baseline, causing a refresh or
commit timeout despite its later successful parse.

### Apply lifecycle and closure

The transaction owner is
[`AutoPushModule`](../../src/modules/auto_push/auto_push_module.cpp).
Its status types, deadlines and wrap-safe comparison are in
[`auto_push_module.h`](../../src/modules/auto_push/auto_push_module.h).

1. `DriveRuntime::onV1Connected` captures a session-qualified detector snapshot
   after [`connected follow-up reads`](../../src/ble_connected_followup.cpp),
   then supplies it to `setPreApplySnapshot` and `queueSlotPush`. Explicit settings
   operations use `DriveRuntime::queueSettingsApply` after a fresh capture.
2. `queueSlotPush`/`queuePushNow` load the profile; `queuePreparedSlot` stages the
   owned strings and definitions, validates slot modifiers, and persists any
   requested active-slot change before publishing the operation. A direct profile
   override does not inherit slot modifiers. The snapshot is consumed once.
3. `preflight` checks the live session, firmware capabilities, current canonical
   observations, competing owners and the complete desired plan before writes.
   A region change with custom sweeps enabled first writes the target region with
   custom filtering disabled, then verifies those user bytes through a fresh read.
4. The three `CustomRefresh*` requests collect target-region sections, maximum
   index and definitions. Each attempt now saves `latestV1NotificationIngressSequence`
   **before** sending; only `SENT` arms capture with that saved boundary.
   `NOT_YET` retries resample and have a bounded send deadline. A failed request
   does not authorize the following table write.
5. [`V1BLEClient::sessionSweepResponseEligible`](../../src/ble_client.h) requires
   ingress after that packet family's request. `BleQueueModule::process` only
   consumes its pending parser reset on an eligible reply. A stale queued reply
   cannot consume the reset; the first fresh reply starts the new collection.
   `CustomRefreshVerify` requires complete unpoisoned captures and every definition
   after its request before compiling the desired table, preserving unowned bands.
6. `CustomWrite` sends used definitions only; the final used index commits.
   Its observation revision **and ingress boundary** are sampled before that
   committing send. `CustomCommitVerify` requires a changed revision, later
   ingress, and canonical result zero. Reject/timeout never advances to readback.
7. `CustomRead` requests all definitions with its existing pre-send boundary.
   `CustomVerify` requires every index, including disabled ones, after that request.
   It checks used/unused shape and valid live sections. Preserved definitions must
   match exactly; requested frequencies may be detector-calibrated within the same
   section. Therefore “full readback” does not mean exact requested MHz equality.
8. Where required, verified custom readback enables the final user bytes and
   freshly verifies them. `finishOperation` requires all requested components
   verified and the same still-live session before reporting `succeeded` and
   publishing the generation-qualified apply edge. Failure blocks remaining work;
   it does not imply previous writes were rolled back.
9. For durable settings operations, `DriveRuntime::processSettingsOperation`
   persists the executor summary and initiates recapture. `finishSettingsRecapture`
   stores and flushes the fresh detector snapshot before final status; incomplete
   recapture can produce `partial` even after executor success. Ordinary connect
   AutoPush and this durable outer operation are distinct callers.

The changed order closes the four in-send response losses without admitting
pre-request packets. Keep the existing post-send observation boundaries for
unsolicited display/mode writes: their broadcasts have a different proof contract.

### Executable proof and limits

[`test_auto_push_module`](../../test/test_auto_push_module/test_auto_push_module.cpp)
uses the production AutoPush owner, parser, quiet coordinator and, in these eight
regressions, real queue. BLE sends, settings/profile storage, clock and display are
test doubles. The send hook enqueues a stamped response inside the mock send; the
queue parses it after return, reproducing the production ordering directly.

| Exact test names | Boundary asserted |
|---|---|
| `test_custom_refresh_sections_accepts_reply_queued_during_send`; `test_custom_refresh_max_accepts_reply_queued_during_send`; `test_custom_refresh_definition_accepts_reply_queued_during_send` | Each repaired refresh request accepts its in-send reply; complete apply/readback reaches success. The region scenario verifies initial FE (custom off) and final F6 (custom on) user byte. |
| `test_custom_refresh_sections_rejects_reply_queued_before_send`; `test_custom_refresh_max_rejects_reply_queued_before_send`; `test_custom_refresh_definition_rejects_reply_queued_before_send` | A stale queued reply leaves capture reset pending and sends no sweep write; supplying a fresh reply permits the complete transaction. |
| `test_custom_commit_accepts_reply_queued_during_send` | An in-send result zero advances to full readback; matching readback completes the actual owner. |
| `test_custom_commit_rejects_reply_queued_before_send` | A stale commit result cannot request readback and ends as `custom_commit_timeout`. |

At repair time these additions produced four failures in 102 cases before the
source change and 102 passes afterward. Existing cases also cover `NOT_YET`
retries/deadlines, request failures, incomplete/poisoned captures, per-definition
readback freshness, calibrated range validation and disconnects across phases.
Locate those with `test_region_refresh_`, `test_custom_readback_` and
`test_region_custom_on_disconnects_never_report_success_at_any_transaction_phase`.

This proves ordering and terminal executor behavior for the modeled schedules.
It does not measure how often the race occurs on hardware, execute the actual
NimBLE transport, establish physical detector persistence, or cover outer durable
recapture/NVS completion. Restoring original device settings is also a separate
qualification action, not the final custom-enable step in this transaction.

## Focused check and bounded maintenance task

From the repository root, with the documented native dependencies available:

```sh
python3 scripts/run_native_tests_serial.py --env native-sanitized \
  test_ble_queue_alert_integration test_auto_push_module
```

Use `test_ble_queue_ingest` when changing notification admission/reassembly;
its parser double limits its claim. Do not run another PlatformIO job concurrently.

For a small agent task, choose **one** repair and name the expected input and final
observable above. Read the owning path plus its complete test fixture, preserve
the rejection control, and run the focused suite. Report the before/after result,
last real consumer exercised, and first mocked boundary. If the required claim
crosses that boundary, identify the additional owner/test or physical observation
needed before describing the work as complete.

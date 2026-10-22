# V1 alert reception and AutoPush response freshness

Behavior traced at `daf32ed`. These are two repairs sharing the BLE ingress
path. Read the owning functions below before changing that path; packet arrival,
queue processing, command completion, and verified application are separate events.

## Shared path and ownership

| Boundary | Owner and behavior |
|---|---|
| BLE callback enters | [`V1BLEClient::notifyCallback`](../../src/ble_connection.cpp) stamps ingress sequence, time, and session generation before characteristic mapping or proxy work. The sequence is nonzero and wraps past zero. |
| Main-loop handoff | [`DriveRuntime::onV1Data`](../../src/drive_runtime.cpp) forwards all stamps to [`BleQueueModule::tryOnNotify`](../../src/modules/ble/ble_queue_module.cpp). Admission and consumption both reject a different session generation. |
| Notification becomes an ESP frame | `BleQueueModule::process` retains complete notification ownership, reassembles B2CE byte streams and B4E0 indexed chunks, and preserves first-arrival ingress and time. A partial B2CE frame keeps its start-byte time; an indexed B4E0 frame keeps its first received chunk's time. Later staging or parsing cannot renew its age. Gaps mark alert-table discontinuity. |
| Frame becomes an observation | [`PacketParser::parseInternal`](../../src/packet_parser.cpp) validates framing and packet-specific destination/origin/width, then records canonical observations with ingress/revision. User-byte capture has a specialized canonical path in the queue. |
| Main-loop consumers run | [`DriveLoopCoordinator::tick`](../../src/runtime_coordinator.h) drains the queue before display composition and connection dispatch. AutoPush runs from `DriveRuntime::processPeriodicMaintenance`, after durable-operation timeout handling, including while local settings own presentation; notification callbacks can execute while its synchronous BLE send waits. |

A complete B4E0 frame waits only for short frames already started when its final
chunk was staged. It then uses the same parser/callback path and processing budget
before newer B2CE frames, including when an older short tail and a newer frame
share one notification. This prevents an older clear or table from overwriting a
newer publication. Ordering does not change the frame's original receipt stamps.

If a completed B4E0 packet blocks the next long notification while B2CE still
needs a header or tail, that tail can be trapped behind the blocked notification.
The queue discards only the unfinished short candidate, marks alert discontinuity,
and counts one recovery drop before resuming complete packets. Ordinary partial
frames and parse-budget stops retain their data. An indexed assembly must contain
one complete ESP frame before it can hold up the short stream.

A known notification gap on either characteristic discards an unfinished B4E0
assembly. Recovery ignores orphan tails until a valid index-1 chunk arrives, so
the tail after a loss cannot combine with the next reply's head. Complete held
frames remain publishable, without clearing that recovery requirement. Session
reset restores ordinary tail-first assembly; without a gap, indexed reordering
and short/long interleaving retain their existing behavior.

Short-stream framing rejects any candidate containing a known ingress gap after
its start byte, even if the mixed bytes have a valid checksum. Resynchronization
begins at the first byte after the gap. This also discards an incomplete header
whose missing bytes cannot arrive, while preserving complete frames before the
gap and valid frames starting exactly at it. An intact split frame retains its
original receipt stamps across notification and uptime rollover.

[`V1PacketFraming`](../../src/v1_packet_framing.h) admits V1 origin EAh with
checksum or E9h without checksum, using the exact data width for that origin.
An EAh checksum byte is never usable as a missing data byte.

Phone commands retain both the detector-session and phone-session generations
through queued writes, deferred retries, and indexed B8D2 command ownership.
The phone disconnect callback retires its generation immediately, before the
main loop handles connection presentation or advertising. Old callbacks and
disconnect events cannot acquire a new phone's authority.
[`test_ble_proxy_phone_session`](../../test/test_ble_proxy_phone_session/test_ble_proxy_phone_session.cpp)
executes the real command-send path to verify that retired phone mute commands
cannot override a later local unmute, while connected-phone retries still work.
NimBLE transport, clocks, and locks remain host doubles.

A successfully forwarded `reqStopAlertData` retires the current detailed-stream
start, including raw and indexed long-characteristic phone writes. While the
same phone session owns that explicit stop, local requests do not undo it. A
replacement phone cannot inherit the stop, including when replacement happens
during the outgoing write. A complete forwarded start releases that ownership
on either command characteristic. After phone departure, the existing recovery
cadence sends a new start even if display packets stay fresh. The last
successful start still owns the minimum send
interval. An ordinary connected phone that did not stop the stream retains the
existing initial-start and silence recovery. `test_ble_proxy_alert_stream`
executes the production proxy, command sender, queue, parser and connection
owner; detector responses and NimBLE remain host models.

A stopped stream can leave the last detailed radar row displayed indefinitely
while the detector continues sending idle display packets. The bounded repair
qualifies **All-Bogeys A only**, on known Gen2 firmware with the existing 4.1039+
logic-mute capability. The existing compatibility policy excludes unknown
versions and unverified future major versions. This is a V1Simple presentation
rule, not a claim that StopAlertData or every resting glyph proves no detection.

The same connected phone and detector session must still own a successfully
forwarded stop. The stop owner captures the notification sequence **after** the
final successful write returns; a display frame's first receipt must be later.
Frames already queued, received during that write, or begun before it cannot
qualify by completing later. A start, phone replacement/departure, or detector
session replacement revokes that authority.

The canonical E9/EAh display must report steady raw `0x77/0x77` A images, zero
strength and both raw band/arrow images zero, scanning and display-on, non-Euro
mode 1, display-active off, and no soft, logic or Auto Mute. Both the retained
table receipt and every newer positive partial-row receipt must be more than
1500 ms old relative to that display's original receipt. Invalidated freshness
after a gap does not bypass the age check. Retirement preserves the partial
assembly and publishes the empty radar state through the existing encounter and
persistence path, using the display receipt time; queue delay cannot renew the
configured hold. A repeated idle packet does not create another clear.

Silence, Stop alone, dark/blink-off displays, active band images, laser, muted
states, logic/Euro modes and unknown firmware retain the existing behavior.
Those modes are deliberately unqualified and this repair does **not** solve
stopped-stream retention for all modes. Ordinary streaming still uses complete
count-zero radar tables. The qualified idle does not override a fresh complete
or partial positive table.

`test_ble_proxy_alert_stream` now runs the production proxy → notification queue
→ parser → persistence/composer/display pipeline. Its stopped-idle tests cover
0/3/5-second persistence (including expiry without another packet), E9/EAh,
short/indexed first-receipt ownership, final long-command transmission,
pre/during-write packets, positive partial contradictions, receipt/sequence
rollover, session replacement, and the excluded states above. The real quiet
coordinator issues no stale mute/unmute action at the clear or phone departure;
the parser's event observer receives one empty publication at the receipt time.
Transport, clocks, panel and speaker remain host substitutes; these assertions
do not establish physical detector behavior in suppressed modes or RF/audio
performance.

AutoPush retains the successful volume write's receipt through its remaining
Apply phases. A canonical rejection of that current write terminates the
component as `volume_write_failed`, releases its volume lease, and prevents
policy activation or a verified Apply edge. Fresh numeric readback cannot
override an explicit rejection of command-only feedback/disconnect policy.
A different successful volume writer blocks the older Apply; stale ingress
and other detector sessions cannot reject the new operation. A later explicit
Apply obtains a new receipt and may succeed. `test_auto_push_module` delivers
rejections and readbacks through the real queue/parser and checks terminal
component summaries, policy activation, ownership release and publication.

Fade-down delivery retries a canonical `respRequestNotProcessed(0x39)` only
while the same volume-write receipt, detector session, alert encounter and
volume owner remain current. Rejection receipt must follow the successful
write's ingress boundary. The normal busy/time-slice gate and retry cadence
still apply. Confirmed reduction, a newer volume writer or observed manual pair,
phone takeover, session change and alert replacement retire that retry; new
threat restoration runs first. A merely unconfirmed send does not cause
periodic low-volume writes. `test_volume_fade_startup` exercises the parser and
quiet/fade owners with a transport double; `test_ble_alert_request_command` and
`test_ble_proxy_alert_stream` verify the production send/queue receipt boundary.

Fade restoration authority is separate from its reduction-delivery receipt.
A fresh canonical pair outside the captured baseline and requested reduction,
or a newer successful volume writer, retires the old restore obligation even
after reduction confirmation or during restoration. The same threat cannot
immediately acquire another fade over that manual choice; a distinct next
threat uses the current pair for its new delay. Speed-volume entry reconciles
this authority before carrying a fade baseline across a clear. Ordinary
baseline/drop feedback still does not acknowledge an unsent restoration.
The frequency history holds the full 15-row table and evicts oldest identities
in longer encounters, always remembering the current priority. Revisiting an
evicted identity may sound again; an unchanged current priority never
oscillates solely because the history filled. `test_volume_fade_startup`
covers these owner transitions, all 15 priorities, a 64-priority continuous
encounter, and complete restore convergence. `test_auto_push_module` also
executes queue → parser → fade → manual clear → verified Apply → next fade
and restoration, using a transport substitute rather than physical hardware.

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
   Freshness comparisons use the newest retained receipt time when short and
   long frames finish out of order; an older copy cannot replace a newer cached row.
   Publication retains the newest contributing row's receipt time.
   A complete publication discards all pending rows, including incomplete
   candidates with other alert counts. Thus an orphan row from a two-alert
   table cannot survive a complete one-alert table and later combine with
   the first row of a new two-alert table. Until that new table is complete,
   the last complete snapshot and its original freshness timestamp remain.
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

### Completed encounters between presentation passes

The parser retains the last renderable priority of the aggregate V1 encounter,
including display-owned laser. When that encounter clears, it records the clear
packet's receipt time and a revision before later frames can replace the live
snapshot. The pipeline consumes each completed encounter at most once and uses
that source timestamp for the configured persistence window. A delayed drain
cannot restart the window. Resetting the detector session invalidates this
history; switching slots cannot revive a completed encounter from the previous
policy. A newer live encounter still owns display and voice. Completed history
is display-only and never queues a voice announcement for an obsolete alert.

The connection owner also resets display mute, soft mute, and mute-confirmation
history when a detector session opens or closes. A new alert can therefore
announce before the first new display packet; fresh mute observations still
apply immediately for soft mute and after two packets for LED mute. A normal
count-zero table clear within the same session preserves display mute authority.

[`test_voice_episode_integration`](../../test/test_voice_episode_integration/test_voice_episode_integration.cpp)
executes queue → parser → pipeline for single-pass live/clear, priority in a
two-row table, display-only laser, delayed presentation, timestamp zero/rollover,
new live encounters, real connection-session mute reset, fresh-mute controls,
and disabled/changed persistence. Existing
voice episode tests protect live rearming and pending-action deduplication.

Playback admission reserves the audio worker without consuming voice dedup.
If bundled-asset discovery is unavailable, idle service and every playback
entrypoint retry at most once per second. Unavailable discovery admits no job
and consumes no episode; recovery can announce only a still-current alert.
The worker reports when PCM is submitted, terminally unavailable assets, or a
failure before any PCM. Only the latter remains retryable, and the pipeline
checks the current episode, alert identity and suppression policy before
resubmitting it. Suppression also cancels an accepted announcement that has not
started: soft mute, phone proxy ownership, configured V1-volume-zero suppression,
disabled voice, speed voice suppression, and In-The-Box voice suppression share
the admission policy. The main loop checks this independently of presentation
edges, including after tail packet processing. Alert clear or priority change
alone does not cancel a brief accepted warning.

Cancellation competes atomically with the worker's first PCM submission commitment.
A cancellation request retains receipt/job ownership until worker cleanup is
acknowledged; it consumes no heard-announcement state. A later admission selects
only a still-current alert under current policy. Once start wins, speech may
finish and is never replayed, including after partial playback. This is a software
decision boundary, not a guarantee about RF arrival or the first acoustic sample.
The host
[`test_audio_worker`](../../test/test_audio_worker/test_audio_worker.cpp) exercises
this receipt through the parser, pipeline and real worker, including exhausted
startup attempts followed by recovery and stale-episode controls. Hardware
I2C/I2S and speaker output remain outside that host test boundary.

[`test_voice_suppression`](../../test/test_voice_suppression/test_voice_suppression.cpp)
executes canonical parser updates, quiet policy, pipeline ownership and the real
worker at controlled startup yields. It covers cancellation without a display
edge, all suppression gates, clear/new-episode cleanup, unmute and current-alert
readmission, and preservation of already-started speech. The extracted runtime
adapters in
[`test_drive_runtime_policies.py`](../../scripts/test_drive_runtime_policies.py)
also verify policy service after tail parsing and while other screens own
presentation. These tests model scheduling and hardware inputs; they do not
measure physical sound or radio timing.

Escalation speech follows the same content, direction, and bogey-count settings
as ordinary speech. Directional counts require both direction and count enabled.
Exactly two free clip slots admit a complete count/direction pair; all four pair append paths
use the full 12-slot capacity and omit only pairs that cannot fit.
The audio worker and idle service retain amplifier shutdown responsibility
until a disable succeeds; after five failed attempts, retries slow to once per
second. A new playback establishes a fresh warm period. Worker and composer
tests cover these contracts with simulated I2C results and recorded clip paths.

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
| `test_short_frame_gap_rejects_hybrid_and_recovers_after_overflow` | Production-capacity overflow cannot splice two different alert frames into a fresh hybrid, for E9/EAh origins and ingress/time rollover; a later intact alert recovers. |
| `test_short_gap_preserves_complete_frames_and_resyncs_split_headers` / `test_short_frame_split_without_gap_remains_valid_for_both_origins` | Pre-gap complete frames and frames beginning at the gap survive; incomplete headers resynchronize; intact split frames remain valid. |
| `test_queue_gap_prevents_cross_cycle_alert_table_publication` | A dropped notification cannot combine old and new rows into a false complete table. |
| `test_batched_alert_rows_keep_their_arrival_age` | Short and long notifications retain row age when drained together, including uptime wrap; fresh peer rows still assemble. |
| `test_delayed_partial_alert_frame_keeps_its_first_arrival_age` | Completing an old partial frame cannot make its row fresh enough to combine with a new peer. |
| `test_completed_long_clear_cannot_overtake_newer_short_alert` / `test_completed_long_row_cannot_overtake_newer_short_alert` | Older complete long clears and rows precede newer short alerts, including equal receipt times and uptime wrap. All-short and between-notification drains remain controls. |
| `test_held_long_waits_only_for_the_older_short_frame` | Split headers/tails survive; a newer frame cannot pass the held long frame even when bundled with the older tail. |
| `test_held_long_row_completes_older_table_before_short_refresh` / `test_held_long_row_does_not_overwrite_a_newer_short_copy` | An incomplete later table cannot renew the earlier publication's age; later complete tables retain their newer row. |
| `test_complete_table_retires_orphan_rows_from_other_counts` / `test_complete_count_transition_without_orphan_publishes_only_complete_tables` | A partial two-alert table cannot survive an intervening complete one-alert table and supply an obsolete priority to the next two-alert table. B2CE/B4E0, E9/EAh, and D6/D8 inputs preserve the old complete snapshot, observer count, and freshness until the new table completes; the no-orphan control still publishes normally. |
| `test_two_long_frames_behind_partial_short_recover_without_losing_complete_alerts` | One unfinished short frame is dropped; both complete long alerts retain their receipt stamps, including uptime wrap. Trailing discarded bytes invalidate audio freshness while preserving the latest display; a subsequent clean table restores freshness. |
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

`test_packet_parser_stream` also checks the orphan-row sequence with zero-based
indices and across uptime rollover. Both new orphan regressions fail without
the complete-publication cleanup. They establish the parser's response to the
injected incomplete-table sequence; whether a real detector begins subscription
mid-table remains unverified. This cleanup does not infer packet generations or
replace the separate known-gap recovery policy.

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
   parses requested sweep responses and consumes their pending parser reset on
   an eligible reply. A stale queued reply cannot consume the reset or mutate
   the collector after a fresh reply has started it. Responses without a request
   boundary for their own packet family retain passive parsing.
   `CustomRefreshVerify` requires complete unpoisoned captures and every definition
   after its request before compiling the desired table, preserving unowned bands.
6. `CustomWrite` sends used definitions only; the final used index commits.
   Its observation revision **and ingress boundary** are sampled before that
   committing send. `CustomCommitVerify` requires a changed revision, later
   ingress, and canonical result zero. Reject/timeout never advances to readback.
7. `CustomRead` requests all definitions with its existing pre-send boundary.
   Only `SENT` arms the queue's definitions capture with that same boundary;
   a queued older row cannot enter the new collector and poison a changed fresh
   definition. Retries resample the boundary, and in-send replies remain eligible.
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

Connection capture has the same freshness requirement. On a retried maximum-index
request followed by a new definitions request, the parser can still hold the
previous complete table until the first fresh definition arrives. A fresh maximum
response formerly marked that old table complete by its index mask alone. The
stable callback could then run with `captureTimedOut=false`, while its stricter
snapshot rejected the stale definitions and custom-frequency Apply sent no writes.
`BleQueueModule::process` now requires every definition's ingress to pass the
current definitions-request boundary in both completion paths. Fresh definitions
may still arrive before the maximum response. The real queue/parser regression
`test_fresh_max_after_retry_cannot_complete_capture_from_old_definitions` in
[`test_ble_queue_alert_integration`](../../test/test_ble_queue_alert_integration/test_ble_queue_alert_integration.cpp)
reproduces the premature completion, then requires both fresh rows before capture
completes. This closes capture admission; it does not itself prove a physical Apply.

The same suite's `test_stale_long_sweep_sections_cannot_poison_new_capture`
delays an old B4E0 tail until a fresh B2CE section has started the next capture.
The old frame must leave the collector unchanged, allowing the fresh long reply
to complete capture. Controls finish the old reply before the request or omit
it entirely. The suite also checks ingress wrap, untouched pending resets,
passive parsing before each family's request, and rejection of fresh duplicates.
These tests use the real queue/parser with a BLE-client double; they do not
execute physical detector timing or a whole settings Apply.

The same suite's `test_overflow_cannot_mix_checksummed_sweep_fragments` and
`test_overflow_cannot_mix_checksum_free_sweep_fragments` fill the default queue
and lose two actual notifications: one reply's tail and the next reply's head.
Neither may produce a complete hybrid topology. Controls cover a gap first
observed on B2CE, ingress wrap, intact and reordered replies, recovery with a
complete new reply, completed held frames, and a new session. This proves the
modeled overflow/recovery paths; the indexed wire format has no packet identity
that could distinguish every possible cross-packet reorder.

### Executable proof and limits

[`test_auto_push_module`](../../test/test_auto_push_module/test_auto_push_module.cpp)
uses the production AutoPush owner, parser, quiet coordinator and, in the queue
regressions below, real queue. BLE sends, settings/profile storage, clock and display are
test doubles. The send hook enqueues a stamped response inside the mock send; the
queue parses it after return, reproducing the production ordering directly.

| Exact test names | Boundary asserted |
|---|---|
| `test_custom_refresh_sections_accepts_reply_queued_during_send`; `test_custom_refresh_max_accepts_reply_queued_during_send`; `test_custom_refresh_definition_accepts_reply_queued_during_send` | Each repaired refresh request accepts its in-send reply; complete apply/readback reaches success. The region scenario verifies initial FE (custom off) and final F6 (custom on) user byte. |
| `test_custom_refresh_sections_rejects_reply_queued_before_send`; `test_custom_refresh_max_rejects_reply_queued_before_send`; `test_custom_refresh_definition_rejects_reply_queued_before_send` | A stale queued reply leaves capture reset pending and sends no sweep write; supplying a fresh reply permits the complete transaction. |
| `test_custom_commit_accepts_reply_queued_during_send` | An in-send result zero advances to full readback; matching readback completes the actual owner. |
| `test_custom_commit_rejects_reply_queued_before_send` | A stale commit result cannot request readback and ends as `custom_commit_timeout`. |
| `test_custom_readback_ignores_changed_reply_queued_before_request`; `test_custom_readback_retry_rejects_reply_queued_before_successful_send` | A changed old row queued before final readback cannot contaminate the collector, including after a deferred send. Fresh readback completes the transaction and final custom enable. |
| `test_custom_readback_old_reply_processed_before_request_control`; `test_custom_readback_identical_queued_reply_control` | Previously passing old-before-request and unchanged disabled-row schedules still complete successfully; the existing refresh tests retain fresh-only readback. |
| `test_custom_readback_accepts_queued_reply_during_send`; `test_custom_readback_rejects_fresh_conflicting_queue_reply` | A fresh in-send response reaches the collector; conflicting fresh data still fails verification and blocks final enable. |

The earlier eight refresh/commit regressions produced four failures in 102 cases
before their source change and 102 passes afterward. Existing cases also cover `NOT_YET`
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

`python3 scripts/test_settings_apply_lifecycle.py` additionally exercises the
production outer Apply owner, durable operation store, executor and drive-loop
ordering. It proves that local settings do not stall an Apply, that the overall
deadline cancels the executor before another write, and that a failed NVS status
write cannot leave it running. A command already sent produces a partial result;
the fixture does not prove physical detector state or the final recapture.

For a small agent task, choose **one** repair and name the expected input and final
observable above. Read the owning path plus its complete test fixture, preserve
the rejection control, and run the focused suite. Report the before/after result,
last real consumer exercised, and first mocked boundary. If the required claim
crosses that boundary, identify the additional owner/test or physical observation
needed before describing the work as complete.

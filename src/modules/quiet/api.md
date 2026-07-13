# quiet Module API

Coordinates everything that wants to **silence** or **lower** the V1: speed-mute (low-speed volume drop), volume-fade (per-alert decay), tap-gesture mute, WiFi-command mute, and auto-push push-time silencing. The quiet coordinator is the single place these intents converge so modules do not race to send conflicting `mute` / `volume` commands to the V1.

The coordinator owns the **desired** state (what owner wants what), the **committed** state (what V1 last acknowledged), and the **presentation** state (what UI shows).

## Class: `QuietCoordinatorModule`

**Header:** `src/modules/quiet/quiet_coordinator_module.h`.

### Public types

- `QuietOwner` — owner enum for `None`, `SpeedVolume`, `VolumeFade`, `TapGesture`, `WifiCommand`, and `AutoPush`.
- `QuietIntent` — internal has-flags + values for one owner request.
- `QuietDesiredState` — aggregate desired mute/volume owner and pending values.
- `QuietCommittedState` — current V1-connected display state (`muted`, `mainVolume`, `muteVolume`, etc.).
- `QuietPresentationState` — display/audio projection: `activeMuteOwner`, `activeVolumeOwner`, `speedVolZeroActive`, `effectiveMuted`, `voiceSuppressed`, and `voiceAllowVolZeroBypass`.

### Main methods

- `begin(V1BLEClient* bleClient, PacketParser* parser)` wires dependencies.
- `sendMute(QuietOwner owner, bool muted)` sends a mute command on behalf of an owner.
- `sendVolume(QuietOwner owner, uint8_t volume, uint8_t muteVolume)` sends a volume command on behalf of an owner.
- `retryPendingSpeedVolRestore(uint32_t nowMs)` retries speed-volume RESTORE writes.
- `processSpeedVolume(uint32_t nowMs, const SpeedMuteLike&, VolumeFadeLike*)` drives speed-mute DROP/RESTORE and includes the baseline guard that waits for real V1 volume data.
- `executeVolumeFade(uint32_t nowMs, VolumeFadeLike*)` runs the per-alert volume-fade decision and sends resulting volume commands.
- `getDesiredState()`, `getCommittedState()`, and `getPresentationState()` expose snapshots for status/debug surfaces.

## Templates

`quiet_coordinator_templates.h` contains `processSpeedVolume`, `executeVolumeFade`, and the private `updateSpeedVolPresentation` helper. `quiet_coordinator_voice_templates.h` contains the voice-presentation projection used by `VoiceModule`. The implementation includes a critical comment block documenting the speed-mute baseline guard fix; preserve that comment because it explains why `DisplayState::hasVolumeData` gates DROP.

## Dependencies

| Dependency | Purpose |
|---|---|
| `V1BLEClient*` | Sends mute/volume commands. |
| `PacketParser*` | Reads `DisplayState` for committed-state sync. |
| `SpeedMuteLike` / `VolumeFadeLike` | Duck-typed templates; concrete types are `SpeedMuteModule` and `VolumeFadeModule`. |

## Notes for maintainers

The owner-arbitration discipline is the load-bearing part. Do not bypass `sendMute` / `sendVolume` to talk to V1 directly from another module; you will fight whoever else is currently the active owner. If an owner does not exist for your use case, add it to the `QuietOwner` enum.

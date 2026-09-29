# In-the-Box audio controls

V1Simple stores these boxes and evaluates the V1G2's reported alerts, then sends
detector-wide mute/unmute commands. Boxes do not program detector scan ranges,
remove alert rows, or change visual priority. Saving a profile alone changes
no running policy; **Apply** activates its settings for the connected detector.

## Reference and explicit product choices

The six default inclusive boxes, in MHz, are X 10500–10550, Ku 13400–13500,
K 24050–24250, and Ka 33700–33900 / 34600–34800 / 35400–35600. These come from
[Valentine's published constants](https://github.com/ValentineResearch/AndroidESPLibrary2/blob/50fe7ba105b10adc69f29ac74053a17e3066c6e4/ESPLibrary2.0/ESPLib/src/main/java/com/esplibrary/utilities/V1FrequencyInfo.java).
The editable domains use that file's Gen2 band edges. The
[published mute implementation](https://github.com/ValentineResearch/AndroidESPLibrary2/blob/50fe7ba105b10adc69f29ac74053a17e3066c6e4/ESPLibrary2.0/ESPLib/src/main/java/com/esplibrary/client/ESPValentineClient.java#L896-L917)
sends mute/unmute requests and observes the audio-mute bit (`isSoft`).

Boxes classify frequency; they do not establish whether an alert is false.
Photo radar can be outside the default boxes. The following arbitration is
**V1Simple's behavior**, not a claim to reproduce unpublished V1connection logic.
Each band's two audio choices default **off**, including migrated profiles:

- **Outside muting:** every live radar row must be valid, outside enabled boxes,
  and on a band opted into outside muting. An inside, unknown, unselected-band,
  or laser alert prevents the mute. With all boxes for a band disabled, every
  valid alert on that band is outside; the editor explains this consequence.
- **Inside unmuting:** a new encounter entering an enabled box on an opted-in
  band can release a pre-existing detector mute. It does not raise the volume.
- A mute owned by the box policy is released when its outside condition ends,
  even if inside unmuting is off. Cleared alerts, laser, unknown data, and stale
  tables therefore release that owned mute.
- A later mute/unmute through V1Simple takes ownership away from the box policy.
  Outside muting waits for the episode to end before rearming. A newly arriving
  inside encounter may still exercise an explicitly enabled inside-unmute rule.
  An observed detector-side unmute also takes precedence for that episode.
- ESP cannot identify another physical mute-button press while already muted:
  no different state is reported. V1Simple's own mute control supplies an
  explicit ownership change. Do not claim to observe otherwise invisible intent.

Encounter matching is one-to-one by band and frequency within 5 MHz of the last
report, independently of table order, priority and direction. Frequency jitter
and leaving/reentering a box do not repeatedly unmute. A clear ends an encounter
even when clear and reappearance arrive in one queue drain. Nearby sources may
be indistinguishable; this is not physical transmitter identification.

Audio uses only complete tables at most 1500 ms old. Known stream loss invalidates
eligibility immediately. Partial rows never contribute to the decision; the
previous complete table remains usable within that age limit. Unknown rows and
gaps preserve encounter history and cannot manufacture a new inside event.
Command retries recheck current eligibility. Confirmation requires a canonical
display packet admitted after the send boundary, using `isSoft` rather than the
debounced screen icon. A confirmed mute is not repeatedly resent.

During detector settings operations, new box decisions pause; V1Simple may
release a mute it already owns. A connected phone proxy or a changed/disconnected
session cancels ownership and pending commands without sending a release. Speed-volume and volume-fade
settings keep their existing behavior; box logic never writes volume. V1Simple
voice follows its outside-mute decision, while every alert remains visible.

## Ownership and persistence

| Boundary | Owner |
|---|---|
| Defaults, strict shape/range validation | `src/v1_in_the_box.h` |
| Editor, save/copy/reset | `interface/src/lib/features/profiles/inTheBoxSettings.js`, `ProfileInTheBoxControls.svelte`, `ProfilesPage.svelte` |
| Classification and encounter history | `src/modules/in_the_box/in_the_box_module.h` |
| Mute ownership and retries | `src/modules/quiet/quiet_coordinator_module.cpp` |
| Table freshness and episode boundaries | `src/packet_parser.h`, `src/packet_parser_alerts.cpp` |
| Apply and activation | `AutoPushModule::finishOperation`, `DriveRuntime::persistInTheBoxApply` |
| Detector-specific committed copy | `V1DeviceStore::setDeviceInTheBox` / `getInTheBoxForAddressChecked` |

Apply finishes the requested detector changes, commits the app policy for that
detector's address, then activates it. A failed app commit retains the previous
policy and reports `in_the_box_persist_failed`; detector writes may already
have occurred. Reconnect/reboot uses the committed copy, independent of later
profile edits/deletion or slot selection. Applying a profile with actions off
disables automatic box audio for that detector.

Profile schema 4, USB bundle 5 and backup 23 carry the definitions. Older inputs
migrate with audio actions off. Applied copies belong to the device catalog,
separate from observed detector settings. Portable profile backups retain the
definitions, not the per-detector catalog.

## Regression boundaries

`test_in_the_box` covers ranges, mixed alerts, unknown/laser/photo rows, jitter,
order changes, duplicate frequencies and all 15 rows. `test_in_the_box_runtime`
uses the production quiet coordinator with parser/BLE substitutes to cover
transport deferral/failure, confirmation, ownership and episode transitions.
`test_packet_parser_stream` exercises real packet parsing, freshness and gaps.
`test_auto_push_module` verifies activation after detector success and app commit,
and preservation of the old policy when either fails. Profile/device/USB/backup
suites exercise production serialization and reload using filesystem/NVS
substitutes. Frontend tests cover all controls, hidden groups, copy/reset, range
validation, concurrent edits during save, and partial-result reporting.

These checks do not establish physical BLE/audio timing, button intent or flash
durability. A device check should back up first, apply one explicit policy,
exercise outside → mixed inside/outside → laser → clear/reappearance, manual
override and reconnect, then read back and restore the original settings.
Keep that observed result separate from host-test results.

## Focused DUT bench

`./bench.sh --replay --camera --profile-controls-qualification` runs a 100-second
capture containing the separate 89-second generated exercise. The exact phase
table is in [v1replay's profile-controls guide](../../tools/v1replay/README.md#profile-controls-qualification).
It tests the real V1Simple DUT against the BLE emulator. Physical V1 RF scanning
and speaker behavior require a physical detector test.

Before loading the temporary profile, power off nearby physical V1 detectors.
Retain a verified USB catalog backup and the quiet/display settings. Establish
the starting applied box policy: firmware predating this feature has inactive
defaults; a current device's last-applied policy cannot be inferred from its
selected slot or exported profile catalog. If that policy is unknown, preserve
or identify it before changing it.

The fixture builder performs no device I/O and refuses to replace an existing
file/profile name or exceed the catalog limit. Against the **fresh managed
emulator only**, its known initial user bytes are `7fffffffffff`:

```sh
./profiles.sh --stay-maintenance backup .artifacts/usb-profiles/box-original.json
python3 scripts/bench/profile_controls_bundle.py \
  .artifacts/usb-profiles/box-original.json .artifacts/usb-profiles/box-enabled.json \
  --baseline-user-bytes 7fffffffffff
python3 scripts/bench/profile_controls_bundle.py \
  .artifacts/usb-profiles/box-original.json .artifacts/usb-profiles/box-inactive.json \
  --baseline-user-bytes 7fffffffffff --inactive-box-policy
```

Update to the committed candidate firmware before loading either version-5
bundle. The enabled fixture keeps the original profiles and assigns its test
profile to all three slots, preventing the emulator's saved default slot from
selecting another profile. It uses an edited K box (24050–24150 MHz), K outside
muting and inside unmuting, and detector sweeps K 24000–24200 / Ka 33900–35000 MHz.
Other box audio actions are off. Disable speed mute and volume fade for the
exercise only if needed, retaining their original settings for restoration.

```sh
./profiles.sh --stay-maintenance load .artifacts/usb-profiles/box-enabled.json
./bench.sh --replay --camera --profile-controls-qualification
```

The run must show the custom-sweep write, successful result and fresh readback
before evaluating scan phases. Inspect `v1replay.log`, the final
`v1_emulator_state.json`, and notification payloads. The first connected-detector
snapshot is captured **before** connect AutoPush, so it is not post-apply proof.
Timestamped `dut_mute_command` events are actual requests received from the DUT.
Compare them with the named stimulus phases and camera frames. The one authored
mute ON at 79 seconds creates a pre-existing mute; only a DUT command can release
it in the subsequent inside phase. Collection COMPLETE alone is not this verdict.

For a negative control, load `box-inactive.json` and repeat the same command.
Both scan ranges must still read back and filter the authored rows. There should
be no box mute commands, and the seeded mute must remain through the new inside
alert. This also restores the exact default applied box policy when defaults
were the verified starting state. Otherwise explicitly apply the preserved
starting policy before removing the temporary profile.

Finally restore the original USB bundle, any changed quiet settings, and the
original operating mode; verify fresh readback. The isolated emulator's scan
state belongs to each retained run and is discarded for subsequent runs. A
physical detector's changed scan state would instead require explicit restoration.
Keep the recorded outcome beside the private run artifacts, including failed
attempts and original-frame witnesses, rather than marking this recipe as a pass.

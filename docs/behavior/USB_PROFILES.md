# USB profile replacement: preserving slot intent

Behavior traced at `daf32ed`. For user commands and format limits, see
[USB profiles](../USB_PROFILES.md). For maintenance work, locate the symbols
below in current source; tests and code take precedence if this map ages.

## Contract and demonstrated failure

Replacing a complete current USB bundle preserves every slot modifier, including
zero volume and an explicit dark-mode-off choice. Editing one slot's persistence
preserves other fields and other slots. This supports the
[speaker contract](../VALENTINE_PHILOSOPHY.md#the-screenspeaker-contract): user
volume is authoritative, including zero.

Before `daf32ed`, USB v3 omitted five modifier fields. Import supplied default
values while retaining the receiving device's flags. A native reproduction used
the production setter, exporter, importer, backup builder, and NVS loader:

| Slot 1 state | Main/muted | Volume override | Dark value/override | Full backup accepted |
|---|---|---|---|---|
| Before replacement | 6/2 | true | true/true | yes |
| After unchanged v3 round trip | 255/255 | true | false/true | no |
| After NVS reload | 255/255 | false | false/true | — |

Changing only slot 0 persistence reproduced the same loss in slot 1. Comparing
two exported v3 documents missed it because both omitted the damaged fields.
The fix closes this representation loss: current USB v4 transports values and
flags together, validates them, and compares them in stored readback. It does
not change the profile schema (still v3) or the NVS schema.

## Trace: caller to durable state to consumer

1. **Host intent and backup.** [profiles.sh](../../profiles.sh) launches
   [usb_profiles.py](../../scripts/usb_profiles.py). `perform()` validates and
   migrates a load/restore before touching the device. In maintenance it exports
   the current bundle and saves the exact original before replacement.
   `set-slot` deep-copies that export and changes only `slots[n].alertPersist`.
   `Device.replace()` migrates older input, validates, encodes, then sends
   `begin → write(offset, hex) → commit`. `same_bundle()` compares all fields
   after migration; catalog order is ignored, slot order is preserved.

2. **Mode and transport admission.** The main loop calls `usbProfiles.tick()`
   in [main.cpp](../../src/main.cpp). [UsbProfileRuntime](../../src/usb_profile_runtime.cpp)
   connects Serial, storage, and the two operating modes to
   [UsbProfileProtocol::dispatch()](../../src/usb_profile_protocol.cpp).
   Normal-mode bulk operations are rejected. `DriveRuntime::usbMaintenanceAllowed()`
   in [drive_runtime.cpp](../../src/drive_runtime.cpp) requires boot readiness,
   no live V1/ALP alert, no power presentation, no USB tail work, and no BLE queue
   backpressure. Maintenance admission is checked again by
   `MaintenanceRuntime::usbConfigurationAllowed()` in
   [maintenance_runtime.cpp](../../src/maintenance_runtime.cpp).
   Host `Device.mode()` requires the requested mode, a changed boot ID, and
   unchanged firmware identity. A mode ACK alone does not complete the transition.

3. **Complete message before mutation.** `UsbProfileProtocol` stages at most
   128 KiB, requires sequential offsets and the full CRC before calling the
   backend, and caches the last exact request/reply. Retrying the same commit ID
   cannot apply twice. A failed or successful apply clears the transfer.
   `UsbProfileRuntime::apply()` parses through
   [UsbProfileJson::parseAndConsume()](../../src/usb_profile_json_document.h),
   then calls [applyUsbProfileDocument()](../../src/usb_profile_document.cpp).

4. **Representation and compatibility.** In that same file,
   `toRestoreDocument()` stages exact owned strings, profiles, and all three
   slots before constructing the internal restore document. It rejects missing
   or extra fields, bad types/ranges, absent profile references, and oversized
   catalogs. USB v4 uses `volumeOverride`, `volume`, `muteVolume`,
   `darkModeOverride`, and `darkMode` on every slot:

   | Input | Modifier meaning |
   |---|---|
   | v4, volume enabled | Both volumes are 0–9; zero is a configured value. |
   | v4, volume disabled | Both volumes must be 255; use profile policy. |
   | v4, dark override enabled | Either true or false is an explicit choice. |
   | v4, dark override disabled | Dark value must be false; use profile policy. |
   | v2/v3 | These formats omitted modifiers; import explicitly disables them instead of inheriting recipient flags. |
   | v1 | Legacy slot-owned detector choices migrate into deterministic profile variants; current modifier flags are cleared. |

   An older export cannot recover values it never recorded. The current host
   sends v4 replacements; use matching updated firmware/client. Older firmware
   rejects v4, and older clients reject v4 exports.

5. **Transaction scope and rollback.** `applyUsbProfileDocument()` passes
   `SettingsBackupScope::ProfilesOnly` to
   [SettingsManager::applyBackupDocument()](../../src/settings_backup_doc.cpp).
   This replaces the entire catalog and all slot fields while preserving
   unrelated settings and credentials. `validateBackupDocumentForApply()`
   accepts current modifiers without a full backup envelope only for this
   explicit scope. Full restores still require their own version/schema rules.
   A USB internal document with enabled modifiers is rejected through `Full`.

   The transaction resolves pending recovery, validates and snapshots state,
   writes mirrored rollback intent before catalog changes, removes absent names,
   and writes incoming profiles. [V1ProfileManager::saveProfileUnlocked()](../../src/v1_profiles.cpp)
   writes a temporary file, promotes it, reloads/verifies it, and commits mirror
   data and reconciliation metadata. A failure rolls back through the enclosing
   settings transaction; unresolved rollback intent remains available for recovery.

6. **Commit and reload.** `saveDeferredBackup()` in
   [settings_backup.cpp](../../src/settings_backup.cpp) persists NVS now and
   schedules SD backup separately. In [settings_nvs.cpp](../../src/settings_nvs.cpp),
   `writeSettingsToNamespace()` writes modifier values and flags;
   `persistSettingsAtomically()` advances a complete A/B namespace generation.
   `getActiveNamespace()` chooses the committed generation; the selector is a
   cache. The restore watermark distinguishes a committed catalog replacement
   from one requiring rollback. `resolveRestoreTransaction()` removes obsolete
   journals or restores the pre-transaction catalog.

   [SettingsManager::load()](../../src/settings.cpp) loads the selected NVS
   namespace. It disables a volume override whose stored pair is invalid; this
   is why the old malformed 255/255 state became permanent loss after reload.
   Both runtime boot paths initialize storage, recover/restore, migrate supported
   legacy profiles, and validate references before ordinary use.

7. **Stored readback and later consumption.** `buildUsbProfileDocument()` first
   resolves storage transactions, requires current profile schema, snapshots the
   complete catalog, serializes all modifiers, then validates its own output.
   Host `Device.replace()` obtains this new export and compares the complete
   bundle. After returning to normal, `perform()` checks active slot, persistence,
   and Auto-Push enabled state. **That normal status does not include modifiers.**

   On detector connection, `DriveRuntime::onV1Connected()` selects a slot and,
   when Auto-Push is enabled, calls
   [AutoPushModule::queueSlotPush()](../../src/modules/auto_push/auto_push_module.cpp).
   This loads the named profile and stages an operation. `configurePlan()` reads
   the [SettingsManager slot getters](../../src/settings_setters.cpp):
   - volume modifiers replace profile values only when the profile requests
     volume (`temporary` or `saved`); the profile still owns that command policy;
   - dark-mode override selects detector display on/off; an explicit dark-mode
     value of false requests the detector display on;
   - explicit-profile `queuePushNow()` sets `applySlotModifiers=false`, so it
     applies the named profile's own values;
   - later display/volume steps send commands and require fresh observations for
     available readback fields. Stored-bundle success does not run or prove them.

## Executable evidence and its limits

Run from the repository root; the native runner serializes the selected suites:

```sh
python3 scripts/test_usb_profiles.py
python3 scripts/run_native_tests_serial.py --env native-sanitized \
  test_usb_profile_document test_usb_profile_protocol test_auto_push_module
```

| Boundary | Real code exercised | Fixture boundary and decisive cases |
|---|---|---|
| Host edit/upload/readback | CLI operation, schema migration, CRC, retries, full comparison | [test_usb_profiles.py](../../scripts/test_usb_profiles.py) uses a Python byte peer, not firmware. `test_v4_set_slot_preserves_every_modifier_and_exact_original_backup`; `test_v4_restore_and_readback_include_values_and_override_flags`; `test_stored_readback_does_not_hide_wrong_normal_consumer`. |
| Wire dispatch | Production `UsbProfileProtocol` | [test_usb_profile_protocol](../../test/test_usb_profile_protocol/test_usb_profile_protocol.cpp) fakes Serial/backend. `test_only_complete_valid_commit_dispatches_and_duplicate_never_reapplies`, CRC/offset rejection, busy/normal admission, torn output, timeout, 128-KiB final-byte checks. |
| Serialized import through committed reload | Production setters, exporter, JSON serialization, `UsbProfileJson::parseAndConsume()`, document adapter, profile manager, transaction, NVS writer/loader, backup builder | [test_usb_profile_document](../../test/test_usb_profile_document/test_usb_profile_document.cpp) uses host filesystem/heap adapters and mocked Preferences/NVS. `test_usb_round_trip_preserves_real_slot_modifiers_and_nvs` and `test_usb_set_slot_preserves_other_slots_and_full_backup_validity` serialize the bundle, parse through the same exact-JSON/PSRAM document helper as `UsbProfileRuntime::apply()`, then import. They assert 6/2+dark-on and 0/0+explicit-dark-off before and after a fresh `SettingsManager::load()`, without SD restoring the values. Actual Serial/mode dispatch is not in this fixture. |
| Compatibility and scope | Same production document/transaction path | In that suite: `test_v2_bundle_does_not_inherit_recipient_overrides`, `test_v3_bundle_does_not_inherit_recipient_overrides`, `test_v4_explicit_disabled_overrides_replace_recipient_values`, `test_usb_modifiers_require_explicit_profiles_only_scope`, and `test_exact_v1_usb_import_is_immediately_exportable_with_same_effective_commands`. Invalid input compares files, Preferences, and exported state before/after. |
| Failed/interrupted commit | Production transaction, journal, profile replacement and reload | Same suite: `test_nvs_failure_rolls_back_created_deleted_profiles_and_settings`, `test_profile_storage_failure_during_replacement_rolls_back_catalog`, `test_interrupted_replacement_recovers_both_catalog_and_slots_from_mirrored_journal`, `test_committed_replacement_survives_reboot_and_stale_journal_cleanup`. Failure/reset points and storage are simulated. |
| Effective application policy | Production `AutoPushModule` | [test_auto_push_module](../../test/test_auto_push_module/test_auto_push_module.cpp) mocks settings, profiles, and BLE. `test_slot_modifiers_win_over_profile_without_changing_direct_profile_apply` checks planned slot versus explicit-profile values; `test_volume_modifier_is_dormant_when_profile_leaves_volume_unchanged` checks no volume request/send; `test_slot_volume_modifier_uses_profile_saved_policy` checks desired values and saved-policy bit. These cases stop at plan/status, not completed physical writes. |

The document suite also measures the maximum USB fixture at 117,187 bytes,
under the 131,072-byte cap. This is a serialized-size measurement, not a device
heap or SD latency measurement.

**Closure:** preserved representation across serialization and production JSON
parsing, strict compatibility semantics, successful canonical full-backup
construction, modeled NVS reload, and the listed failure recovery boundaries are
exercised. Auto-Push's downstream modifier semantics are tested separately. The
suites do not form one host-to-real-detector execution.

**Remaining physical boundary:** these tests do not qualify USB re-enumeration,
real NVS/SD power interruption, modifier readback after a real reboot, detector
receipt/application, or resulting display/audio. `readback_verified` is a full
maintenance export comparison; `normal_consumer_verified` checks only the status
fields above. `backup_pending` explicitly allows deferred SD backup. To claim a
device repair, preserve an exact original bundle, change a represented modifier,
read it back after reboot, exercise its applicable consumer, then restore and
verify the original state. A no-op or a clean boot alone cannot close that gap.

## Bounded follow-up task

For a USB modifier change, begin with `usb_profiles.py`,
`usb_profile_document.cpp`, and the two document/host regression files. Follow
the transaction and NVS owners above only when the failing assertion crosses
those boundaries. For a correct stored value producing the wrong command, begin
at `configurePlan()` and the three named Auto-Push policy tests instead. Report
the first differing state, the test's actual input/output, and every mocked
boundary. Preserve values and flags together; never fix an invalid override by
silently discarding the user's choice.

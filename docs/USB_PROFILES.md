# USB profile backup and restore

`profiles.sh` controls V1Simple's stored detector-profile catalog and all three
Auto-Push slots over USB. It uses the existing maintenance mode for bulk work;
no Wi-Fi connection or WebUI interaction is needed. The firmware must include
the `V1USB1` API. The launcher uses the bench Python environment when available,
otherwise Python 3 with `pyserial==3.5`. It installs nothing automatically.

```sh
./profiles.sh status
./profiles.sh backup .artifacts/usb-profiles/original.json
./profiles.sh set-slot 1 --persistence 2
./profiles.sh restore .artifacts/usb-profiles/original.json
```

Slot numbers are **0, 1 and 2**, matching firmware and WebUI. Persistence is an
integer **0–5 seconds**. `set-slot` changes only that slot's persistence; it
does not select the slot or alter other settings/profiles. Its output confirms
the complete stored bundle and, when returning to normal operation, the normal
firmware's active slot, persistence and Auto-Push enabled setting.

`load FILE` and `restore FILE` both replace the complete catalog and slot bundle.
Profiles absent from the file are removed. Before either operation or `set-slot`,
the command downloads the current bundle and saves an exclusive, permission-0600
backup under ignored `.artifacts/usb-profiles/`. The saved path prints before
replacement begins. Use `--backup-before FILE` after the command for a chosen
destination. `backup FILE` also creates its destination directory as needed and
never overwrites an existing file or symlink. No profile contents are printed.

Commands enter maintenance automatically and return to the original mode by
default. Entry can be rejected as busy while normal operation owns live work.
`--stay-maintenance`, placed before the command, leaves maintenance active for
a batch. `./profiles.sh normal` explicitly returns to normal operation;
`./profiles.sh maintenance` explicitly enters maintenance. A restart ACK alone
is never success: the host verifies the requested mode, new boot and unchanged
firmware identity. Bulk operations begun in maintenance remain there by default.

Use `./profiles.sh --port DEVICE status` when more than one USB device is present.
The host opens CDC with DTR and RTS false and follows the same USB identity if
its device path changes across reboot. Close other serial monitors and bench
commands before using this interface.

Every transferred chunk has a checked offset, and the complete document has a
checked CRC32. Requests use bounded exact retransmission with the same request
ID. Normal commands allow two seconds per attempt; backup/commit allow thirty
for existing storage work. Three unsuccessful attempts fail the command. These
are host transport bounds, not firmware display-response requirements.

`readback_verified` confirms all stored bundle fields and catalog entries; only
catalog enumeration order is ignored. `normal_consumer_verified` is true only
after the default return from maintenance verifies the active stored selection
in normal firmware. It is false when the operation stays in maintenance.
`backup_pending` reports the firmware's deferred SD-backup state separately from
the confirmed storage commit. These results do **not** claim that the detector
received or applied a profile, or that display/audio behavior passed a camera
test. Importing or saving a profile never writes live detector state. A later
normal-runtime Auto-Push operation has its own per-component status and fresh
readback rules. Focused user-byte and volume reads capture the notification
boundary immediately before sending, so replies received during the send remain
eligible when parsed later; notifications received before that boundary cannot
verify the operation. An error or uncertain commit ACK is
never reported as success, and the before-backup remains available for recovery.

## Upgrading existing device data

Normal users should preserve device data when updating and should not delete or
reset valid profiles first. On each boot with profile storage available, the
firmware checks the stored schema and automatically retries any required
conversion. Migration is deterministic and atomic: the existing valid catalog,
Auto-Push enabled state and active slot are retained, as are each slot's name,
color, alert persistence and priority-arrow choice.

Legacy version-1 data stored detector behavior partly on each Auto-Push slot.
Migration moves the effective behavior into profiles. Slots that shared a source
profile and effective commands continue to share it; when their effective
commands differ, distinct deterministic variants are created. Slots without a
catalog source are grouped by their effective commands. The first distinct
configuration creates a safe `Auto-Push Slot N` profile named for that slot;
later no-source slots with identical effective commands share it. Different
configurations create separate deterministic profiles, with a deterministic
collision suffix when a generated name is already used. Their six user bytes
default to no user-byte change, and only detector choices explicitly present in
the old slots are carried forward. Existing catalog profiles remain in the
catalog.

Version-2 profiles are converted to version 3 before commit. Their existing
user bytes, mode, display and volume behavior are retained. The new volume
feedback policy is `none`, disconnect policy is `restore_saved`, and the
Bluetooth indicator follows the old display-off choice (`off` when display was
off, otherwise `unchanged`). Custom-frequency policy is `unchanged`. In version
3, `unchanged` is an explicit instruction not to invent or send a value; it is
not missing or corrupt data.

The update can recover from a valid SD or internal LittleFS profile mirror and
uses mirrored transaction records to resolve interrupted restore or migration
work. A backup with an invalid CRC, malformed profile or invalid reference is
ignored rather than applied. Before clearing a current-schema slot reference
that is absent from the available catalog, startup first attempts recovery from
valid current and previous backups. It clears the reference only after the
catalog is confirmed available and nonempty and the profile is still not found;
storage I/O failure or an unavailable/empty catalog leaves the reference intact.

This conversion changes stored ownership only. It does not itself send detector
commands or prove detector, RF, display or audio behavior. A later Auto-Push
operation has its own application and readback evidence.

The current export is schema version 3. Its root contains exactly
`format: "v1simple-profiles"`, `version: 3`, `autoPushEnabled`, `activeSlot`,
`slots` and `profiles`; network credentials and unrelated device settings are
excluded. Each slot contains `name`, `profile`, `color`, `alertPersist` and
`priorityArrowOnly`. Detector-owned settings are stored with each profile:
`name`, `description`, six `rawBytes`, and a `detector` object covering user
settings, mode, display, volume values plus feedback/disconnect policy,
Bluetooth indicator policy, and custom-frequency definitions.

The importer accepts exact version-1, version-2 and version-3 bundles. Version 1
stored detector behavior partly in slot fields; version 2 lacked the full v3
volume, Bluetooth-indicator and custom-frequency policy. Both older formats are
deterministically converted to v3 before the first catalog commit. The host and
firmware both reject an import if that conversion would exceed the supported
catalog or cannot produce valid unique UTF-8 names. Unknown/duplicate fields,
trailing data, invalid references and malformed values are rejected before any
mutation.

The supported catalog contains at most 10 profiles. Names are at most 64 UTF-8
bytes, descriptions at most 4096 UTF-8 bytes, and slot display names at most 20
UTF-8 bytes. Every complete USB, HTTP backup, SD backup and restore-journal
document is capped at 128 KiB. The measured ten-profile lexical-max shapes are
116,902 bytes for USB, 120,471 for HTTP backup, 120,640 for SD backup and 118,759
for the credential-bearing rollback journal. The largest leaves 10,432 bytes—
more than two 4 KiB allocation quanta—under the common cap. A maximal delete
journal is 11,742 bytes, leaving 4,642 bytes under its 16 KiB cap. A maximal
pretty schema-v3 profile file is 16,415 bytes; its 24 KiB cap retains 8,161 bytes
of storage/schema headroom.

Old installations could already contain more than 10 profiles. Those entries
are retained and remain available through the paginated WebUI/API list and the
transactional delete path so they can be pruned without silent loss. New saves,
complete export/import, migration and live Apply are blocked with a capacity or
migration-pending error until the catalog is reduced to 10 and deterministic
migration completes. A full-catalog rename remains count-preserving and does not
temporarily create an unjournaled eleventh profile. Enter maintenance, open the
Profiles page and delete unused profiles until the converted catalog—including
any generated slot variants—can fit within 10. Restart into normal or
maintenance mode after pruning so automatic migration retries; if it remains
pending, delete another unused profile and restart. Existing saved Auto-Push
slots remain available while migration is pending; erasing or factory-resetting
the device is not required.

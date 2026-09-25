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
does not select the slot or alter other settings or profiles.

`load FILE` and `restore FILE` both replace the complete catalog and slot bundle.
Profiles absent from the file are removed. Before either operation or `set-slot`,
the command downloads the current bundle and saves an exclusive, permission-0600
backup under ignored `.artifacts/usb-profiles/`. The saved path prints before
replacement begins. Use `--backup-before FILE` for a chosen destination. Backup
files never overwrite an existing file or symlink, and profile contents are not
printed.

Bulk commands enter maintenance automatically and return to the original mode
by default. Entry can be rejected while normal operation owns live work. Put
`--stay-maintenance` before the command to remain in maintenance for a batch;
`maintenance` and `normal` switch modes explicitly. Mode changes are successful
only after the host verifies a new boot in the requested mode with unchanged
firmware identity.

Use `./profiles.sh --port DEVICE status` when more than one USB device is
present, and close other serial monitors or bench commands first.

Transfers verify offsets, a document CRC, and a complete stored readback.
`normal_consumer_verified` additionally confirms the active slot settings after
returning to normal firmware; it remains false with `--stay-maintenance`.
`backup_pending` reports deferred SD backup separately from the confirmed
storage commit. None of these results proves that the detector received or
applied a profile, or that display or audio behavior passed a camera test.

## Upgrading existing device data

Preserve device data when updating; do not delete or reset valid profiles first.
On boot, the firmware atomically converts supported version-1 and version-2
stored profile data to the current version-3 profile schema. Existing catalog
entries, Auto-Push state, slot presentation, and explicit detector choices are retained. When
legacy slot-owned behavior differs, migration creates deterministic profile
variants instead of silently merging it.

Only valid mirrors and transaction records are used for recovery. Invalid or
malformed data is not applied, and interrupted conversion is retried or resolved
on a later boot. Conversion changes stored ownership only; it does not itself
send detector commands or prove detector, RF, display, or audio behavior.

Exports contain the profile catalog and all three Auto-Push slots. Network
credentials and unrelated device settings are excluded. Current USB bundles use
version 4 and contain version-3 detector profiles. Each slot explicitly records
its volume and dark-mode override flags and values, including zero volume and
an explicit dark-mode-off override. Backups, restore readback, and unrelated
`set-slot` edits preserve these choices.

The importer also accepts exact version-1, version-2, and version-3 bundles,
validates them before mutation, and rejects conversions that cannot fit the
supported catalog. Version-1 slot-owned detector choices migrate into profiles
as described above. Version-2 and version-3 USB bundles did not record slot
overrides; restoring them disables those overrides and uses the imported
profiles' policies. They cannot recover modifier choices omitted by an older
export. No import inherits override flags from the receiving device.

Use the updated USB client with the updated firmware. Older clients reject
version-4 exports. The updated client can read older bundles, but sends version 4
for all replacements; older firmware rejects those replacements without changing
the stored catalog or settings. Update firmware before using its mutation commands.

The supported catalog contains at most 10 profiles. Names are at most 64 UTF-8
bytes, descriptions at most 4096 UTF-8 bytes, and slot display names at most 20
UTF-8 bytes. Complete bundle documents are capped at 128 KiB.

Old installations could already contain more than 10 profiles. Those entries
are retained so they can be deleted without silent loss. New saves, complete
export/import, migration, and live Apply remain blocked until the catalog can be
reduced to 10. In maintenance, delete unused profiles and restart so migration
can retry. Existing saved Auto-Push slots remain available while migration is
pending; erasing or factory-resetting the device is not required.

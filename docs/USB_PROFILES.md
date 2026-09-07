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
after the default return from maintenance verifies active settings in the normal
firmware. It is false when the operation stays in maintenance.
`backup_pending` reports the firmware's deferred SD-backup state separately from
the confirmed storage commit. These results do **not** claim that the detector
received/applied its profile, or that display/audio behavior passed a camera test.
An error or uncertain commit ACK is never reported as success; the before-backup
remains available for recovery.

The version-1 JSON bundle contains exactly `format: "v1simple-profiles"`,
`version: 1`, `autoPushEnabled`, `activeSlot`, `slots` and `profiles`. It excludes
network credentials and unrelated device settings. Slots use the WebUI keys
`name`, `profile`, `mode`, `color`, `volumeConfigured`, `volume`, `muteVolume`,
`darkMode`, `muteToZero`, `alertPersist` and `priorityArrowOnly`. Profiles contain
`name`, `description`, `rawBytes` (six bytes), `displayOn`, `mainVolume` and
`mutedVolume`. Keep all fields when editing a backup; unknown fields, invalid
references and malformed values are rejected. The maximum document is 128 KiB.

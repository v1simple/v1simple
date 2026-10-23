# V1Simple

> Keep V1Simple simple, in code and docs.

V1Simple gives your Valentine One Gen 2 a separate touchscreen for alerts,
with saved detector profiles and adjustable display and audio settings. It
runs on the Waveshare ESP32-S3-Touch-LCD-3.49 and connects over Bluetooth.
It can also expose a BLE proxy. Configuration runs in a separate WiFi
maintenance mode.

**[Open the browser installer](https://www.v1simple.com/)** — no build tools required.

## Hardware and safety

- Waveshare ESP32-S3-Touch-LCD-3.49, board revision V1 or V2
- Valentine One Gen 2 with BLE enabled

### Board revision — read before ordering

Waveshare discontinued the V1 board and says shipments switched to V2 after
2026-06-08. The firmware supports both revisions with automatic detection and
routing at boot:

| Board | Backlight | Panel reset |
|---|---|---|
| V1 | GPIO8 | GPIO21 |
| V2 | GPIO42 | EXIO5 |

Detection samples GPIO21 and EXIO5 as complementary reset/TE candidates before
enabling the display outputs. If the revision cannot be identified safely, the
firmware leaves the swapped candidates passive and the screen dark.

Identify the revision before flashing:

| | V1 | V2 |
|---|---|---|
| PCB silkscreen | Rev1.0 | **Rev1.1** |
| QC sticker on the case | none | **V2** |

The revision numbering is confusing: silkscreen `Rev1.1` is the *newer* V2
board. Waveshare documents the revision change at
<https://docs.waveshare.com/ESP32-S3-Touch-LCD-3.49>.

### Safety

Use the board's USB connection for the documented bench, flash, and test path.
The `esp32-s3-car-install` build only changes firmware shutdown behavior; it is
not vehicle-power wiring guidance. Any vehicle installation needs a separately
verified, protected power supply and wiring.

The cased board is sold with and without a lithium cell. A lithium battery left
in a parked vehicle can exceed its rated temperature range. Choosing a
no-battery variant for a permanently mounted install avoids that; this project
does not verify thermal behavior in any configuration.

## Install and use

Follow the browser installer's preparation and flashing steps, then reset the
board.

On a normal boot, the firmware starts scanning for the detector. Hold and
release **BOOT** after about four seconds to reboot into maintenance mode. Join
the default `V1-Simple` network with `setupv1simple`, open
`http://192.168.35.5`, and change that default password during first setup.

### Upgrading existing device data

- **Update and preserve data:** updates firmware and the maintenance web
  interface together while preserving settings, WiFi networks, profiles, and
  stored audio. Restart after the transfer, then reopen maintenance mode.
- **Fresh install:** installs firmware and interface together, but erases saved
  settings, WiFi networks, and internal fallback profiles, even with
  **Erase device** off. SD-card contents remain intact; a valid SD backup can restore
  saved settings on boot. Read the installer's warnings before choosing this path.

A normal preserving upgrade does not require deleting profiles, resetting
settings, or erasing storage. Supported older profile and Auto-Push data is
converted atomically on boot. If an older catalog has more than 10 profiles,
delete unused profiles from the maintenance Profiles page and restart so
migration can finish—do not factory-reset the device.

The web interface is bundled in the application image and served directly from
firmware; updating it does not rewrite LittleFS. The preserving installer writes
only the application partition and leaves the bootloader, partition table, NVS,
LittleFS, and SD card intact. This supports existing V1Simple installations with
the current partition layout. A release that changes that layout needs explicit
upgrade instructions; this installer does not migrate partition tables.

If power or the USB connection is lost during an update, the device may not
start until the update is retried. There is one application slot and no automatic
rollback. Reconnect in bootloader mode by holding **Power + GEAR** while
connecting USB-C, then choose **Update and preserve data** again. Do not choose
**Fresh install** to recover: it erases saved device data. After the retry
completes, restart, reopen maintenance mode, and check your settings and profiles.

See [USB profile backup and restore](docs/USB_PROFILES.md#upgrading-existing-device-data)
for migration and recovery details. Storage migration does not prove that the
detector received or applied a profile.

## Build from source

Install pioarduino Core 6.2.0 and LittleFS Python support in the same Python
environment, plus a Node.js version accepted by `interface/package.json`:

```sh
python3 -m pip install 'pioarduino==6.2.0' 'littlefs-python==0.18.0'
```

From the repository root:

```sh
./build.sh          # build the web interface and default firmware
./build.sh --upload # update firmware and web interface, preserving stored data
./build.sh --all    # build, flash LittleFS and firmware, then monitor serial
```

Before building the interface, `build.sh` checks LittleFS support in the Python
interpreter reported by the selected PlatformIO executable, then prepares that
environment's platform and framework packages with `pio pkg install`. The first
build needs network access for project and npm dependencies. If using an existing
PlatformIO installation, set `PIO_CMD` to its executable; a missing LittleFS
dependency reports the installation command for that exact Python interpreter.

`--skip-web` and direct `pio run` reuse only an unchanged, verified frontend
build. If it is missing or stale, run `npm run build` in `interface/` first.

`--all` overwrites internal LittleFS data. Confirm SD-backed storage before
relying on saved profiles.

## Verify a change

Set up the repository privacy boundary once per clone, and verify it before
working or committing:

```sh
./scripts/setup-hooks.sh
./scripts/check_local_privacy_setup.py  # quick local identity, hook, index, and destination check
```

Never bypass the hooks or remote checks. The local checker requires an
owner-only private-term list at `~/.config/v1simple/privacy_terms.txt`; keep
site-specific names, addresses, networks, and device identifiers there, never
in this repository.

```sh
./scripts/ci-test.sh                   # complete pre-push/release code, test, and build gate
./scripts/run_device_tests.sh --quick  # connected-board boot and heap checks
./bench.sh --replay --camera          # raw replay/camera evidence, then sampled visual fields
```

Automated tests establish code behavior. Device tests and bench runs establish
only what happened on the connected setup. Camera evidence establishes visible
screen behavior for that recorded run. None proves every detector, power, RF,
or vehicle environment.

The bench collector preserves replay input, delivery, timing, serial, firmware
identity, settings snapshot, and camera artifacts. After `COMPLETE` confirms raw
capture, a separate report checks sampled stable display fields against the
recording and prints `VISUAL_FIELDS_PASS`, `FAIL`, or `INCONCLUSIVE`. The report
lives beside `replay/` as `visual_fields_result.json`; a mismatch or inconclusive
reading returns a nonzero status without rewriting the completed raw capture.
This is not a verdict on every video frame or profile-based hold timing. An
unavailable maintenance snapshot makes a collection incomplete, not a product
failure.

Preview old camera videos with `python3 scripts/cleanup_camera_videos.py`; add
`--apply` to remove them. The command keeps 14 days of video under
`.artifacts/bench`, preserves reports, logs, timing files and still images, and
records removals beside each affected run. It skips unfinished runs, symlinks,
and open or recently modified videos; deletion requires a working `lsof`.
If cleanup is interrupted between deletion and recording it, the record retains
that file as pending rather than reporting an unverified removal.
The command runs once; schedule it locally for recurring cleanup.

Keep changes focused, read [AGENTS.md](AGENTS.md), run checks proportionate to
the change, inspect the final diff, and state whether hardware or camera
evidence was collected. Run the complete gate before a push or release.

For maintenance, the [behavior traces](docs/behavior/README.md) connect the
repaired profile, protocol, ALP and camera paths to their owners and focused
tests, with explicit limits on what each fix proves and a bounded agent prompt.

## Releases

Every successful reviewed publication to public `main` publishes a release.
Ordinary pushes and IDE Sync use the same verified privacy gate. Patch is the
default and is derived from the newest strict semantic-version tag. To select
the next minor or major, change `FIRMWARE_VERSION` in the reviewed product
change to exactly that next version. Arbitrary version jumps are rejected.

Release builds inject the selected version, verify it in the firmware binary,
and tag the exact commit that passed CI. Automation does not create a release
commit or modify source files. [CHANGELOG.md](CHANGELOG.md) is the curated
summary; complete generated notes and history live in GitHub Releases.

## Project notices

MIT licensed; see [LICENSE](LICENSE). See [SECURITY.md](SECURITY.md) and
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). This independent project is
not affiliated with or endorsed by Valentine Research, Inc.

# V1Simple

> Keep V1Simple simple, in code and docs.

**IMPLEMENTED:** Firmware for the Valentine One Gen 2 and the Waveshare
ESP32-S3-Touch-LCD-3.49. It connects to the detector over BLE, parses its
display stream, renders alerts on the board, and can expose a BLE proxy.
Configuration runs in a separate maintenance-mode WiFi interface.

## Hardware and safety

- Waveshare ESP32-S3-Touch-LCD-3.49, **board revision V1**
- Valentine One Gen 2 with BLE enabled

### Board revision — read before ordering

This firmware is built and bench-verified against the **V1** revision only. It
has not been run on V2 hardware.

Waveshare discontinued the V1 board and switched shipments to V2 after
2026-06-08, so a board bought new today is likely V2. V2 swaps the IO for
`LCD_BL`/`EXIO_INT` and for `LCD_TE`/`LCD_RESET`. This firmware hardcodes
`LCD_BL=8` and `LCD_RST=21` in `platformio.ini`.

**Symptom on V2: the screen never lights up.** The board is not damaged and the
flash succeeded — the firmware boots, connects to the detector, and renders into
its canvas, but drives the backlight on the wrong pin. Not a dead unit.

Identify the revision before flashing:

| | V1 | V2 |
|---|---|---|
| PCB silkscreen | Rev1.0 | **Rev1.1** |
| QC sticker on the case | none | **V2** |

The revision numbering is confusing: silkscreen `Rev1.1` is the *newer* V2
board. Waveshare documents this on
<https://docs.waveshare.com/ESP32-S3-Touch-LCD-3.49> only; the older
`waveshare.com/wiki` page for the same product does not mention it.

V2 support is not yet implemented. The web installer publishes a single merged
image for all users, so revision handling cannot be a build-time option and is
an open design question.

### Safety

Use the board's USB connection for the documented bench, flash, and test path.
The `esp32-s3-car-install` build only changes firmware shutdown behavior; it is
not vehicle-power wiring guidance. Any vehicle installation needs a separately
verified, protected power supply and wiring.

The cased board is sold with and without a lithium cell. A lithium battery left
in a parked vehicle can exceed its rated temperature range. Choosing a
no-battery variant for a permanently mounted install avoids that; this project
does not verify thermal behavior in any configuration.

## Build, flash, and use

Install PlatformIO Core 6.1.19 or newer and a Node.js version accepted by
`interface/package.json`. From the repository root:

```sh
./build.sh          # build the web interface and default firmware
./build.sh --all    # build, flash LittleFS and firmware, then monitor serial
```

`--all` overwrites internal LittleFS data. Confirm SD-backed storage before
relying on saved profiles.

On a normal boot, the firmware starts scanning for the detector. Hold and
release **BOOT** after about four seconds to reboot into maintenance mode. Join
the default `V1-Simple` network with `setupv1simple`, open
`http://192.168.35.5`, and change that default password during first setup.

With an SD card mounted, completed V1 alert tables are saved under
`/encounters` for maintenance-mode download. They preserve the detector's own
alert assignments and contain no GPS or vehicle-speed data. On each normal
boot, the firmware keeps the newest 20 generated CSV files in each of `/perf`,
`/alp`, and `/encounters`. Files with other names are never removed by this
retention policy.

## Verify a change

Install and verify the fail-closed privacy boundary once per clone before making
any commit:

```sh
./scripts/setup-hooks.sh
./scripts/check_local_privacy_setup.py
```

The setup fixes this checkout's author and committer identity to the public
project identity, installs early and final privacy gates that remain active for
`git commit --no-verify`, and pins ordinary Git pushes and IDE Sync to the
verified public `origin`. Every normal push passes through the range-aware
history scanner, which checks all newly reachable blobs once by object hash as
well as commit and tag metadata. Never use `--no-verify` or override the hooks
or remote configuration. The local checker also requires an owner-only private
term list at
`~/.config/v1simple/privacy_terms.txt`; keep personal names, addresses, network
names, device identifiers, and other site-specific terms there, one per line.
Never add that file or its values to this repository.

```sh
./scripts/ci-test.sh                   # complete local code, test, and build gate
./scripts/run_device_tests.sh --quick  # connected-board boot and heap checks
./bench.sh [--all|--core|--display|--replay] [--camera] [--no-upload]  # bench evidence suites, one verdict; see ./bench.sh --help
```

Automated tests establish code behavior. Device tests and bench runs establish
only what happened on the connected setup. Camera evidence establishes visible
screen behavior for that recorded run. None proves every detector, power, RF,
or vehicle environment.

Keep changes focused, read [AGENTS.md](AGENTS.md), run the complete gate, inspect
the final diff, and say whether hardware or camera evidence was collected.

## Releases

Every successful reviewed publication to public `main` publishes a release.
Ordinary pushes and IDE Sync use the same verified privacy gate. Patch is the
default and is derived from the newest strict semantic-version tag. To select
the next minor or major, change `FIRMWARE_VERSION` in the reviewed product
change to exactly that next version. Arbitrary version jumps are rejected.

Release builds inject the selected version, verify it in the firmware binary,
and tag the exact commit that passed CI. Automation does not create a release
commit or modify source files. Release notes and history live in GitHub Releases.

## Project notices

MIT licensed; see [LICENSE](LICENSE). See [SECURITY.md](SECURITY.md) and
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). This independent project is
not affiliated with or endorsed by Valentine Research, Inc.

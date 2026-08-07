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
alert assignments and contain no GPS or vehicle-speed data.

## Verify a change

```sh
./scripts/ci-test.sh                   # complete local code, test, and build gate
./scripts/run_device_tests.sh --quick  # connected-board boot and heap checks
./bench.sh --all --camera              # core + display + replay + camera, one verdict
```

The unified bench command builds and manages `v1replay` as its V1 detector: an
idle LightBlue-compatible V1 for the core and display windows, then the fixed
multi-alert scenario for replay. The replay uses its provisional scenario blink
profile by default: the priority arrow blinks during the authored multi-alert
interval and stays steady during single-alert periods. Use
`--blink-profile steady` or `--blink-profile stress` for explicit controls. A
physical V1 is not required. After an upload,
it allows 90 unscored seconds for one-time post-flash SD activity to settle;
the scored session still retains the same SD-start and runtime limits. It then
captures calibrated camera evidence for every live window and stores the logs,
metrics, media, and `bench_result.json` under one `.artifacts/bench/.../runs/`
directory. The replay artifacts also include that boot's encounter CSV. Its
camera result confirms that evidence was captured; visual correctness still
requires review of the recorded video and bright/dim still pair.

Bench metrics have independent absolute and baseline-regression checks. The
`absolute_min` and `absolute_max` fields in
[`tools/hardware_metric_catalog.json`](tools/hardware_metric_catalog.json)
control the absolute check. When both are `null`, the scorer reports
`absolute_state=n/a`: the current value cannot fail an absolute check by itself,
but the metric can still fail a compatible baseline comparison through its
`regress_abs` or `regress_pct` threshold. For example, `disp_pipe_p95_us` is a
hard regression gate and `wifi_max_peak_us` is an advisory regression gate even
though neither has an absolute bound. The `required` field is separate again: a
missing required metric fails the run regardless of its value thresholds.

Automated tests establish code behavior. Device tests and bench runs establish
only what happened on the connected setup. Camera evidence establishes visible
screen behavior for that recorded run. None proves every detector, power, RF,
or vehicle environment.

Keep changes focused, read [AGENTS.md](AGENTS.md), run the complete gate, inspect
the final diff, and say whether hardware or camera evidence was collected.

## Choose the next release

Every successful push to public `main` publishes a release. Patch is the default.
After committing the changes to release, select a minor or major bump before
pushing:

```sh
./scripts/release-bump          # show the selection and next version
./scripts/release-bump minor    # select 1.2.0 after 1.1.x and commit the choice
./scripts/release-bump major    # select 2.0.0 after 1.x and commit the choice
./scripts/release-bump patch    # explicitly select the normal patch release
```

The selector refuses to run with other uncommitted changes and creates the
release-selection commit for you. Do not edit `FIRMWARE_VERSION`; the release
workflow owns it. After a successful minor or major release, the workflow resets
the selection to patch in its release commit, so sync `main` before continuing
development.

## Project notices

MIT licensed; see [LICENSE](LICENSE). See [SECURITY.md](SECURITY.md) and
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). This independent project is
not affiliated with or endorsed by Valentine Research, Inc.

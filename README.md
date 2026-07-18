# v1simple

A standalone display and BLE proxy for the Valentine 1 Gen 2 radar detector, running on the Waveshare ESP32-S3-Touch-LCD-3.49 board.

## What it does

Connects to a Valentine 1 Gen 2 over Bluetooth LE, parses its display data stream, and renders the bands, arrows, signal-strength bars, frequency, and bogey counter on a 3.49" LCD, with secondary alert cards for additional bogeys during multi-alert encounters. Optionally exposes a BLE proxy so a companion phone app (V1 Driver, V1connection LE) can connect through the device instead of pairing directly to the V1. When no phone app is connected, optional local voice alerts can announce alert band, frequency, direction, bogey count, and quiet-driving suppression state through the onboard audio path. Configuration is handled through a deliberate maintenance-mode WiFi web interface so normal drive runtime keeps WiFi and the web server off.

## Hardware

Required:

- Waveshare ESP32-S3-Touch-LCD-3.49 (ESP32-S3, 16 MB flash, 8 MB PSRAM, AXS15231B 640×172 QSPI display, capacitive touch)
- A Valentine 1 Gen 2 with the BLE module enabled

Optional:

- AL Priority laser jammer (for ALP integration — laser direction overlay)
- OBD-II adapter (ELM327 BLE — for vehicle data overlay)
- Serial GPS receiver (Adafruit Ultimate GPS v3 / MTK3339 — UTC + GPS speed source)
- Speaker/amp path for local voice alerts (Waveshare ES8311 output with speaker amp enable on the shared TCA9554 I/O expander)

For permanent car installation (no battery, ignition-switched 12V), see `docs/HARDWARE_NOTES.md`. That document covers the `PWR_BUTTON` solder mod and the bench-test sequence to run *before* making that mod.

## Build

Requires [PlatformIO](https://platformio.org/). From the repo root:

```sh
./build.sh                  # default firmware build (waveshare-349 env)
./build.sh --upload         # build and flash over USB
./build.sh --car            # car-install variant (CAR_MODE_PWR_SHORT)
```

Or invoke PlatformIO directly:

```sh
pio run -e waveshare-349           # default firmware
pio run -e esp32-s3-car-install    # car-install variant
pio test -e native                 # native unit tests
```

The full set of build environments is defined in `platformio.ini`. The default upload writes the application image at `0x20000` to match the custom partition table.

Authoritative filesystem upload path: `./build.sh --upload-fs` or `./build.sh --all`.

## First boot and maintenance UI

The device boots into normal drive runtime by default. Normal runtime does not start WiFi or the web server.

To reach the web UI, hold **BOOT** (or use the touchscreen 4-second maintenance gesture) for about 4 seconds. The touchscreen gesture only fires while no alert is showing, so an accidental hold during an alert can't reboot the display. The firmware records a one-shot maintenance request, reboots, skips BLE/V1/OBD/ALP/GPS/perf runtime init, and brings up the maintenance WiFi surface. In maintenance mode the AP is named `V1-Simple` by default (password `setupv1simple`; both are customizable). Connect from a phone or laptop and load **`http://192.168.35.5`** — the AP IP is fixed in firmware. Change the default AP password during first setup before using maintenance mode outside a private bench session.

The maintenance UI handles WiFi network selection, offline V1 profile authoring, display/quiet/audio settings, ALP/OBD/GPS settings, backup/restore, logging configuration, and diagnostics. Its Logs page lists and downloads a bounded allowlist of SD diagnostics, including performance/ALP files and shutdown evidence; arbitrary SD files and serial-only output remain inaccessible. Live V1 push/pull and runtime OBD/GPS/ALP status are unavailable there because those runtimes are intentionally not started.

From the WiFi Client page you can pre-stage up to four STA networks (manual SSID/password entry supported), such as a car AP, phone hotspot, garage AP, or workshop network. Future maintenance boots scan for saved SSIDs, try them in priority order, and keep the setup AP up while STA association is attempted.

## Updating

USB remains the recovery/bench update path:

```sh
./build.sh --clean --all      # build web assets, upload LittleFS, upload firmware, monitor
./build.sh --clean -f -u      # build/upload LittleFS + firmware without monitor
```

Firmware updates are performed over USB or through the ESP Web Tools installer
published to GitHub Pages with each release.

## Where to look

- `docs/API.md` — external HTTP/BLE API reference.
- `docs/TESTING.md` — CI, hardware-test, and baseline-reset workflow.
- `docs/PERF_SLOS.md` — correctness/operational SLOs and scoring contract.
- `docs/HARDWARE_NOTES.md` — install procedure, solder mods, audio/GPS wiring, car-mode bench-test sequence.
- `docs/RELEASE_CHECKLIST.md` — release procedure.
- `RELEASE_NOTES.md` — historical 1.0.1 validation scope and accepted limitations;
  later release notes are generated on GitHub from merged changes.
- `docs/ALP_PROTOCOL_EVIDENCE.md` — tracked, sanitized ALP byte-pattern evidence for source/test citations.
- `docs/V1_PROTOCOL_REFERENCES.md` — tracked V1 protocol fact summary for source/test citations. Maintainers may verify against local official PDFs, but tracked comments should cite the tracked summary by anchor.
- `CONTRIBUTORS.md` — project origin and attribution, including the upstream V1G2-T4S3 reference.
- `CHANGELOG.md` — versioned change history.
- `src/` — the firmware, organized by subsystem (display, BLE, settings, ALP, OBD, GPS, WiFi, etc.).

## License

MIT — see `LICENSE`.

## Credits

Built on Kenny Garreau's [V1G2-T4S3](https://github.com/kennygarreau/v1g2-t4s3). See `CONTRIBUTORS.md` for maintainer and AI-assisted development attribution.

## Trademarks and affiliation

This is an independent, community-built project. It is **not affiliated with, endorsed by, or sponsored by Valentine Research, Inc.** "Valentine One," "V1," and related names and logos are trademarks of Valentine Research, Inc., and are used here only nominatively, to describe interoperability with that hardware. This project is likewise not affiliated with the makers of AL Priority, ELM327/OBD adapters, or any other third-party hardware it interoperates with. See `THIRD_PARTY_NOTICES.md` for full attribution and license notices.

# v1simple 1.0.1 Release Notes

First supported release.

**1.0.0 was tagged, published, and withdrawn the same day.** It shipped without
the runtime-safety hardening in this release; its GitHub Release and assets have
been removed. Use 1.0.1.

The public `main` branch is intentionally a single root snapshot rather than a
full development history. 1.0.1 is the baseline: a Valentine 1 Gen 2 display
and BLE proxy on the Waveshare ESP32-S3-Touch-LCD-3.49, with optional voice
alerts, ALP, OBD-II, and GPS integration. See `CHANGELOG.md` for the full
capability list and `README.md` for hardware requirements and first-boot steps.

## What 1.0.1 fixes

Runtime-safety hardening, all covered by new contract tests:

- **BLE handle ownership.** Remote attribute pointers are owned solely by the
  main loop; BLE callbacks publish atomic events instead of nulling them. Closes
  a cross-task race that could null-dereference — and reboot the device — if the
  V1 disconnected mid-connection-setup.
- **Stale-callback rejection.** Connection-handle matching prevents a delayed
  `onDisconnect` from an older link tearing down the current session.
- **Storage no longer blocks the alert path.** BLE bond backup moved off the main
  loop to a dedicated Core-0 writer with a bounded, latest-value-wins queue, so a
  background SD write cannot stall BLE drain or display updates.
- **Audio task lifecycle.** Voice playback uses a persistent worker task instead
  of per-play create/delete on a reused static TCB.
- **No credentials over the network.** AP and saved STA passwords can no longer
  be exported via HTTP under any parameter; they reach only a local SD backup.
- **Supply chain.** The release action is pinned to a full commit SHA.

## Validation scope

- Release firmware and LittleFS assets are produced by the tested
  `waveshare-349` PlatformIO environment.
- Release readiness requires the full CI script, core/display bench evidence,
  merged-image flash-policy validation, and the release evidence manifest
  described in `docs/RELEASE_CHECKLIST.md`.
- Mutating maintenance API routes require the bundled UI's
  `X-V1Simple-Request: maintenance-ui` header, and LittleFS static serving is
  constrained to explicit UI/asset allowlists. Note this is a CSRF mitigation,
  not authentication: the maintenance API has no per-request auth, and its safety
  rests on the AP password. **Change the default AP password.**
- ESP Web Tools release images match PlatformIO's effective serial flash policy
  (`DIO`, 80 MHz, 16 MB) and validate both bootloader and embedded app image
  headers before publishing.

## Accepted limitations

- Physical LCD pixel-layer checks remain manual/aspirational. In
  `docs/PERF_SLOS.md`, `C-PERSIST-HOLD` and `C-BLINK-CADENCE` are
  `aspirational-lcd`, and `C-BENCH-EYES` is `manual-lcd`. Firmware contracts,
  traces, and bench scoring cover the automated layers; there is no camera or
  pixel-level LCD automation in this release.
- OBD/proxy hardware qualification is explicitly accepted as `ACCEPTED_RISK`.
  OBD, BLE proxy mode, and related arbitration paths ship covered by unit
  tests, contract checks, static checks, firmware builds, and core/display
  bench gates, but not by a representative real-hardware OBD/proxy pass.
- The maintenance web UI configures logging but does not display logs. Logs are
  written to SD (perf CSV) and the serial console.
- `notify_to_display` shows a deterministic ~37 ms outlier roughly every 78 s on
  the core bench (median 2 ms, advisory budget 50 ms). Within budget; cause not
  yet instrumented. Tracked for a post-1.0.1 fix.

## Installing

Flash over USB (`./build.sh --clean --all`) or use the ESP Web Tools installer
published to GitHub Pages with this release.

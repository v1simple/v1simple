# Changelog

All notable changes to this project are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- Added a maintenance-mode Diagnostics & Logs page with bounded, allowlisted
  downloads for shutdown, panic, performance, and ALP evidence, plus a clean
  reboot into normal runtime.

### Changed

- Maintenance pages now distinguish configuration that can be saved from live
  operations that require normal runtime, show the automatic-reboot countdown,
  and report Wi-Fi scan, disconnect, and saved-network outcomes explicitly.
- Active maintenance sessions now use a 10-minute idle window that extends with
  UI activity, while retaining a 30-minute absolute session cap.
- Release evidence is now generated from typed bench and OBD/proxy artifacts,
  bound to the clean checkout's full Git SHA, and rejected when a bench warning
  or failed hardware qualification remains.
- Automatic patch publication now waits for the exact public `main` commit's
  successful push-origin CI run and reconfirms that same run immediately before
  publishing the generated release commit and tag.

### Fixed

- **Power:** Power-off now selects only inactive wake inputs, uses battery
  latch-off or external-power deep sleep as appropriate, clears the retained
  `GOODBYE` frame, and records optional shutdown/next-boot evidence on the SD
  card. Car-install builds no longer enter battery shutdown paths, and an
  aborted shutdown restores persistence and the awake display state.
- **Battery safety:** Maintenance mode continues servicing low-battery safety,
  and battery-source classification now uses time-separated samples while
  preserving power-button operation when ADC initialization fails.
- **BLE and OBD:** V1 notifications and alerts are fenced to the active
  connection session, while proxy teardown and disconnect display cleanup run
  through their main-loop owners. OBD GATT handles and disconnects are owned by
  the transport task, and cancelled connections can no longer be adopted by a
  later state-machine cycle.
- **Wi-Fi and maintenance:** Maintenance Wi-Fi recovers after failed startup or
  an emergency memory stop, stays reachable while a saved network connects,
  and is exempt from normal-runtime idle shutdown. Scan results now remain
  valid for all consumers, and client enablement commits only after the request
  is admitted.
- **Settings and UI:** Connected persistence is deferred only for a bounded
  window, touch-driven backups avoid the interactive path, and settings restore
  feeds the watchdog across long phases. GPS settings and display previews
  remain usable in maintenance mode, Auto-Push live writes fail closed there,
  and stale Wi-Fi connection details are removed after disconnect.


## [1.0.4] - 2026-07-15

Changes are summarized in the generated GitHub release notes.

## [1.0.3] - 2026-07-14

Changes are summarized in the generated GitHub release notes.

### Changed

- The Release workflow now reuses successful authoritative CI evidence from
  the selected `main` lineage and runs a focused exact-artifact build instead
  of repeating the full native, sanitizer, mutation, frontend-test, and static
  analysis gate on a generated two-file version commit.
- Full CI and Release now share one production frontend, firmware, memory,
  package-size, and LittleFS build path, eliminating duplicate dependency and
  same-worktree artifact builds.
- A fresh release dispatched from tagged `main` now applies the selected bump;
  tag reuse is allowed only when resuming the same recorded workflow run.
- Release commits are rejected if `include/config.h` changes anything beyond
  the `FIRMWARE_VERSION` value, and CI now keeps npm installs lockfile-strict.

## [1.0.2] - 2026-07-14

Changes are summarized in the generated GitHub release notes.

### Changed

- The manual Release workflow now accepts a `patch`, `minor`, or `major` choice,
  prepares the firmware version and changelog automatically, validates the
  exact release commit, and atomically publishes that commit with its tag.
- Release notes are generated from merged changes instead of reusing the static
  1.0.1 release-notes document.

## [1.0.1] - 2026-07-13

First supported release. v1simple is a standalone display and BLE proxy for the
Valentine 1 Gen 2 radar detector, running on the Waveshare
ESP32-S3-Touch-LCD-3.49.

1.0.0 was tagged, published, and withdrawn the same day: it shipped without the
runtime-safety hardening below. Its release and assets have been removed. Use
1.0.1. See the 1.0.0 entry for the full capability list, which 1.0.1 inherits
unchanged.

### Fixed

**BLE (Tier 1/2 — connectivity and ingest)**

- Remote attribute handles (`pClient_`, `pRemoteService_`, `pDisplayDataChar_`,
  `pCommandChar_`, `pCommandCharLong_`) are now owned solely by the main loop.
  BLE callbacks publish atomic events and no longer null these pointers, closing
  a cross-task data race whose worst case was a null dereference — and a reboot —
  if the V1 disconnected during discovery, subscribe, or command send.
- Connection-handle matching (`activeConnectionHandle_` /
  `quiescingConnectionHandle_`) rejects a delayed `onDisconnect` from a previous
  link, so a stale callback can no longer tear down the current session.
- The discovery task and reconnect are now explicitly interlocked: reconnect is
  forbidden until the state machine leaves `DISCOVERING`.
- Removed a redundant 272-byte packet frame from the queue-full path in the
  NimBLE notify callback, recovering stack headroom on the 5 KB host task.

**Storage (Tier 6 must never block Tier 1–3)**

- BLE bond backup no longer takes the blocking SD lock on the main loop. Runtime
  callers enqueue a snapshot to a dedicated Core-0 writer through a bounded,
  latest-value-wins queue (`ble_bond_backup_writer`). A background SD write can
  no longer stall BLE drain or display updates. Enforced by a source contract
  test (`test_runtime_callers_contain_no_sd_lock_or_filesystem_write`).

**Audio**

- Voice playback runs on a persistent worker task signalled by task
  notification, replacing per-play task create/delete on a reused static TCB
  that could be re-initialized while the previous task was still live.
- Tasks created with `xTaskCreatePinnedToCoreWithCaps` now use the matching
  `vTaskDeleteWithCaps`, enforced by contract test.
- Added stack high-water instrumentation for the audio and writer tasks.

**Maintenance web UI**

- Credential material is removed from the network backup path entirely. The
  `includePasswords` query parameter is ignored; a `BackupTransport` type makes
  the payload builder structurally aware of HTTP vs SD, so AP and saved STA
  passwords can only ever reach a local SD backup.
- Removed a dead `Content-Length` pre-check that could never fire (the header was
  not in the `collectHeaders` allowlist). The body-size cap is now documented
  honestly as a semantic cap, not a pre-allocation transport cap.

**Release engineering**

- `softprops/action-gh-release` pinned to a full commit SHA rather than a mutable
  major tag, so a repointed tag cannot execute with `contents: write`.
- Added missing third-party license notices: FreeType (bundled via
  OpenFontRender), Arduino GFX, NimBLE-Arduino, ArduinoJson, OpenFontRender,
  Svelte, SvelteKit, Tailwind, daisyUI.
- Corrected the task-watchdog comment in `sdkconfig.defaults`, which overstated
  its coverage (it watches the Core-0 idle task, not all tasks).

### Known limitations

- Unchanged from 1.0.0 (see below).
- `notify_to_display` shows a deterministic ~37 ms outlier roughly every 78 s on
  the core bench (median 2 ms; budget 50 ms advisory). It is within budget and
  its cause is not yet instrumented. Tracked for a post-1.0.1 fix.

## [1.0.0] - 2026-07-13 [WITHDRAWN]

**Withdrawn.** Published and removed the same day; superseded by 1.0.1, which
adds the runtime-safety hardening listed above. Do not use. The capability list
below is accurate and is inherited by 1.0.1.

First public release. v1simple is a standalone display and BLE proxy for the
Valentine 1 Gen 2 radar detector, running on the Waveshare
ESP32-S3-Touch-LCD-3.49.

### Added

**V1 display**

- BLE connection to a Valentine 1 Gen 2, with display-data stream parsing and
  rendering of bands, directional arrows, signal-strength bars, frequency, and
  the bogey counter on the 640×172 LCD.
- An 8-bar signal meter mirroring the V1 Gen2's 8-segment scale.
- Secondary alert cards for additional bogeys during multi-alert encounters.
- Persisted-alert and preview render paths with partial-region flushing, so
  steady-state redraws touch only the regions that changed.

**BLE proxy**

- Optional proxy GATT server so a companion phone app (V1 Driver,
  V1connection LE) can connect through the device rather than pairing directly
  to the V1.
- Scan-ownership arbitration and a SCANNING watchdog to keep dual-role NimBLE
  operation stable.
- Bond backup/restore, with a fresh-flash policy that resets bonds when the
  firmware version changes.

**Local voice alerts**

- Optional spoken alerts over the onboard ES8311 audio path announcing band,
  frequency, direction, bogey count, and quiet-driving suppression state.
  Active only when no phone app is connected.

**Maintenance web UI**

- Deliberate maintenance mode (4-second BOOT hold, or a touchscreen long-press
  that fires only while no alert is showing). Normal drive runtime keeps WiFi
  and the web server off entirely.
- Setup AP (`V1-Simple` by default) at a fixed `192.168.35.5`, plus pre-staging
  of up to four STA networks tried in priority order.
- Offline V1 profile authoring with auto-push slot activation, display/quiet/
  audio settings, ALP/OBD/GPS settings, backup/restore, logging configuration,
  and diagnostics. Logs are written to SD (perf CSV) and the serial console;
  the web UI does not display them.
- Mutating API routes require the bundled UI's `X-V1Simple-Request` header, and
  LittleFS static serving is constrained to explicit allowlists.

**Optional peripherals**

- AL Priority laser jammer integration (laser direction overlay).
- OBD-II (ELM327 BLE) vehicle data overlay.
- Serial GPS (MTK3339) for UTC and GPS speed, with speed-source selection and
  speed-based volume control.

**Power and car install**

- Car-install variant (`CAR_MODE_PWR_SHORT`) for ignition-switched 12V with no
  battery, including auto-power arming, critical-voltage warning and shutdown.

**Storage**

- Crash-safe JSON persistence: temp-file promotion with rollback, and an
  `nvsValid` written-last sentinel that rejects a truncated persist rather than
  booting a half-written namespace as defaults.

**Build and release**

- PlatformIO build environments for the default board, the car-install variant,
  and native unit tests.
- Manual (`workflow_dispatch`) release pipeline that builds firmware and
  filesystem images, merges binaries for ESP Web Tools, publishes a GitHub
  Release, and deploys the web installer to GitHub Pages.

### Known limitations

- Physical LCD pixel-layer checks remain manual. `C-PERSIST-HOLD` and
  `C-BLINK-CADENCE` are `aspirational-lcd` and `C-BENCH-EYES` is `manual-lcd`
  in `docs/PERF_SLOS.md`. Firmware contracts, traces, and bench scoring cover
  the automated layers; there is no camera or pixel-level LCD automation.
- OBD and BLE proxy hardware qualification is accepted risk. Those paths ship
  covered by unit tests, contract checks, static checks, firmware builds, and
  core/display bench gates — but not by a representative real-hardware
  OBD/proxy pass.

[Unreleased]: https://github.com/v1simple/v1simple/compare/v1.0.4...HEAD
[1.0.4]: https://github.com/v1simple/v1simple/releases/tag/v1.0.4
[1.0.3]: https://github.com/v1simple/v1simple/releases/tag/v1.0.3
[1.0.2]: https://github.com/v1simple/v1simple/releases/tag/v1.0.2
[1.0.1]: https://github.com/v1simple/v1simple/releases/tag/v1.0.1
[1.0.0]: https://github.com/v1simple/v1simple/releases/tag/v1.0.0

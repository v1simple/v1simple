# V1 Simple 4.2.14 Terminal Release Notes

This is the terminal release snapshot for this project. The public `main`
branch is intentionally a single root snapshot rather than the full development
history.

## Validation scope

- Release firmware and LittleFS assets are produced by the tested
  `waveshare-349` PlatformIO environment.
- Local release readiness requires the full CI script, core/display bench
  evidence, merged-image flash-policy validation, and the release evidence
  manifest described in `docs/RELEASE_CHECKLIST.md`.
- The release workflow pins current GitHub Actions major tags, including
  `actions/checkout@v6` and `actions/setup-node@v6`; those tags were verified
  as existing on 2026-05-27.

## Accepted limitations

- Physical LCD pixel-layer checks remain manual/aspirational. In
  `docs/PERF_SLOS.md`, `C-PERSIST-HOLD` and `C-BLINK-CADENCE` are
  `aspirational-lcd`, and `C-BENCH-EYES` is `manual-lcd`. Firmware contracts,
  traces, and bench scoring cover the automated layers; there is no camera or
  pixel-level LCD automation in this terminal release.
- OBD/proxy hardware qualification is explicitly accepted as
  `ACCEPTED_RISK`. OBD, BLE proxy mode, and related arbitration paths ship
  covered by unit tests, contract checks, static checks, firmware builds, and
  core/display bench gates, but not by a representative real-hardware
  OBD/proxy pass.
- The changelog intentionally starts at 4.2.14 in this archive snapshot. Prior
  development lineage is not represented in the single-commit public history.

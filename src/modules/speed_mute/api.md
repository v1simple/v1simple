# speed_mute Module API

Lowers V1 alert volume below a configurable speed threshold. Inspired by JBV1, Highway Radar, and Spectre Nav low-speed muting. This module is a pure decision function with a thin wrapper for main-loop wiring; BLE volume commands are sent by the quiet coordinator.

## Design rules

1. **Fail-open** — if the speed source is lost, never mute. Safety first.
2. **Hysteresis** — unmute threshold = `threshold + hysteresis` to prevent cycling.
3. **Band overrides** — Laser and Ka always bypass low-speed mute.
4. **Best-effort** — never blocks BLE / display / connectivity.
5. **Pure decision function** — caller owns BLE commands.

## Public types

### `SpeedMuteSettings`

- `enabled`
- `thresholdMph` — mute below this speed, range 5-60.
- `hysteresisMph` — unmute at threshold + this.
- `v1Volume` — V1 volume to push when speed-muted, range 0-9.
- `voice` — whether local voice announcements should be allowed while speed-muted.

### `SpeedMuteContext`

Per-loop input from `SpeedSourceSelector`: `speedMph`, `speedValid`, and `nowMs`.

### `SpeedMuteDecision`

`shouldMute` indicates whether the quiet coordinator should apply the speed-muted V1 volume.

### `SpeedMuteState`

Persistent state: `muteActive` and `lastTransitionMs`.

## Functions and methods

- `evaluateSpeedMute(const SpeedMuteSettings&, const SpeedMuteContext&, SpeedMuteState&)` is the testable pure function.
- `SpeedMuteModule::begin(enabled, thresholdMph, hysteresisMph, v1Volume, voice)` initializes settings and state.
- `SpeedMuteModule::syncSettings(enabled, thresholdMph, hysteresisMph, v1Volume, voice)` updates runtime settings.
- `SpeedMuteModule::update(speedMph, speedValid, nowMs)` evaluates once per loop.
- `SpeedMuteModule::isBandOverridden(band)` returns true for Laser and Ka.

## Notes for maintainers

The fail-open rule is structural. `evaluateSpeedMute` checks `ctx.speedValid` before any threshold logic; if speed is invalid, `shouldMute` is forced false. Losing GPS / OBD should not silence radar alerts.

The hysteresis is asymmetric: mute below threshold, unmute above `threshold + hysteresis`. If hysteresis is 0, speed wobble near the threshold can cycle mute/unmute rapidly.

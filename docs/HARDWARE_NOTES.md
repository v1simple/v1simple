# Hardware Notes — v1simple

> Physical installation details, solder modifications, and bench-test procedures.
> Consult this before cutting wires or flashing a car-install build.

---

## Portable Shutdown and Wake

The portable build has two distinct shutdown outcomes because firmware can cut
the battery latch but cannot remove an attached USB/external supply:

- On battery, firmware drops the TCA9554 power latch. If the rail remains alive,
  it waits up to 1.5 seconds for PWR/GPIO16 to return HIGH before using it as an
  active-low deep-sleep wake source. If PWR is still LOW, wake falls back to
  active-low BOOT/GPIO0.
- On USB/external power, firmware isolates the battery latch and enters deep
  sleep using active-low BOOT/GPIO0. PWR/GPIO16 is deliberately excluded because
  this board reads it LOW while external power is attached, which would make an
  active-low wake condition already asserted.

The selected wake pin receives an RTC pull-up and is checked both before EXT1 is
armed and immediately before sleep entry. If it is LOW at either check, shutdown
is aborted, the backlight sleep hold is released, and the disconnected screen is
restored. The runtime also rewrites its clean-shutdown marker to unclean and
reopens the BLE-bond and deferred-settings backup writers, preserving their
existing queues and retry state. `/poweroff.log` records `wake=BOOT_GPIO0` or
`wake=PWR_GPIO16` with the terminal shutdown outcome when power-off SD logging is
enabled.

---

## Car Install (Permanent Vehicle Mount)

### Overview

The standard Waveshare ESP32-S3-Touch-LCD-3.49 uses an onboard 18650 cell as the
retention supply for the TCA9554 power latch circuit. In a permanent car install,
the 18650 is unnecessary — the car's ignition-switched 12V rail does the same job.

The `CAR_MODE_PWR_SHORT` compile-time flag (`./build.sh --car`) disables every
software-driven shutdown path because ignition power controls the device lifetime:

- Shutdown requests return before preparation, display, or battery handoff work
- `powerOff()` remains a low-level no-op as a defense-in-depth latch safeguard
- `processPowerButton()` is never called in `process()` — no shutdown from long-press
- The critical-battery grace-period and low-battery warning are disabled
- V1/ALP auto-power arming, timer creation, and timeout shutdown are disabled

### PWR_BUTTON Solder Mod (GPIO 16)

> **Flash before soldering.** Once GPIO 16 is bridged LOW, the USB-serial bootloader
> may not enumerate reliably and `pio run --target upload` will likely time out.
> Flash the `--car` firmware and verify boot on the bench *before* making this mod.

At power-on, GPIO 16 is sampled to decide whether an 18650 is installed (HIGH = battery
present, LOW = USB or button pressed). In a car install with no 18650, GPIO 16 floats
at startup and the battery-presence detection is unreliable.

**Solder mod:** Bridge GPIO 16 (PWR_BUTTON) to GND on the Waveshare board.

- GPIO 16 is active-LOW (button-pressed = LOW)
- Bridging it LOW permanently at the PCB level makes firmware behave as if the power
  button is always held
- With `CAR_MODE_PWR_SHORT`, the button-check code path is compiled away entirely,
  so this has no effect on behavior — it only makes the GPIO deterministic at startup

**Pad location:** GPIO 16 / SPICS0 test pad on the Waveshare ESP32-S3-Touch-LCD-3.49.
Use a 10kΩ pulldown resistor to GND rather than a direct short if you want to preserve
the option to remove the mod later.

> **Reversibility:** Desolder the resistor/bridge to restore normal portable operation.
> Reflash the standard env (`./build.sh -f -u`, or `./build.sh --clean -f -u` for a clean rebuild) to re-enable battery logic without opening the monitor.

### 18650 Removal

The onboard 18650 holder can be left unpopulated in a car install. The TCA9554 power
latch is not exercised with `CAR_MODE_PWR_SHORT`, so the cell is never needed.

Leaving the holder empty eliminates:
- Risk of cell over-discharge if the latch ever fails
- Weight and bulk
- The `hasBattery()` boot-time uncertainty (with no cell, GPIO 16 samples LOW
  consistently after the solder mod, confirming no-battery to the power code)

### ALP RX Pin (GPIO 2)

The ALP uses UART2 RX on GPIO 2 (RJ-45 pin 2 from the ALP CPU TX line). No solder
modification is needed for car install — the ALP connection is unchanged.

GPIO 2 is not currently used as a production wake source. The deep-sleep helper
can configure EXT1 wake masks, but `CAR_MODE_PWR_SHORT` rejects shutdown requests
and retains a low-level `powerOff()` no-op, so it does not enter deep sleep. Do
not rely on ALP wake from sleep unless a future change explicitly wires
`enterDeepSleep()` with the GPIO 2 mask and validates the polarity on hardware.

### Build and Flash

```bash
# Build and upload the car-install firmware
./build.sh --car --clean -f -u

# Run the focused native car-install suite serially
python3 scripts/run_native_tests_serial.py --env native_car
```

### Bench Test Sequence

**Order matters:** flash first, solder second.

1. **Flash `--car` firmware** before any solder work: `./build.sh --car --clean -f -u`
2. **Connect to 5V USB** (no 18650 installed).
3. Confirm display shows the idle screen (V1 locator, no alert).
4. Hold the PWR button (or momentarily short GPIO 16 to GND if solder-modded):
   - Normal build: long-press triggers shutdown sequence.
   - Car-install build: **no shutdown** — display remains on.
5. Confirm the car-install build shows no `GOODBYE` screen, performs no shutdown
   preparation, and keeps BLE, WiFi, and logging operational.
6. Connect V1 BLE. Confirm alerts render normally.
7. Disconnect V1. Wait for `autoPowerOffMinutes` timeout.
   - Car-install: no auto-power timer or shutdown runs; the display and runtime
     services remain active while the ignition rail is up.
8. Verify ALP UART still receives packets and can render alerts after the wait.

### Pin Map Summary

| Signal | GPIO | Notes |
|---|---|---|
| PWR_BUTTON | 16 | Active LOW; sampled at boot for battery detection; solder to GND for car install |
| TCA9554 PWR_LATCH (I2C) | Pin 6 on TCA9554 | Latch is never exercised with `CAR_MODE_PWR_SHORT` |
| ALP RX (UART2) | 2 | RJ-45 pin 2 from ALP CPU TX |
| GPS RX (Serial1) | 1 | From GPS TX |
| GPS TX (Serial1) | 5 | To GPS RX |
| GPS EN | not driven | Internal 10 kΩ pull-up to VBAT keeps module ON; firmware does not assert EN |

---

## Audio / Voice Alerts

Local voice alerts use the Waveshare ES8311 audio path. The speaker amplifier is
enabled through pin 7 on the shared TCA9554 I/O expander at address `0x20`; pin 6
on the same expander remains the battery power-latch output in non-car builds.

Voice announcements are configured from the maintenance UI Audio page (`/audio`)
and are intended for standalone use when no companion phone app is connected
through the BLE proxy.

---

## GPS Module — Adafruit Ultimate GPS Breakout v3 (PID 3133)

**Chipset:** MediaTek MTK3339 / PA6H  
**Interface:** Serial1 (UART), RX=GPIO1, TX=GPIO5. EN pin not connected to firmware.

### EN pin

The MTK3339 EN line has an internal 10 kΩ pull-up to VBAT — the module is **ON
by default** when powered. The firmware does not drive EN; `gpsEnabled=false`
shuts down the UART (`Serial1.end()`) but leaves the module powered. This is
acceptable — the module draws ~25 mA idle, and car-install power is always on.

### Bench-test checklist

1. Wire VIN→3.3 V, GND, GPS-TX→GPIO1, GPS-RX←GPIO5.
2. In the web UI GPS page (`/gps`), enable GPS.
3. Confirm `moduleDetected = true` in `GET /api/gps/status` within ~5 s indoors.
4. After outdoor exposure: `hasFix = true`, `stableHasFix = true`, `satellites ≥ 4`.
5. UTC appears in the perf CSV `utc` column and in the ALP CSV.

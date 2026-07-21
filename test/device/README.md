# Device Test Suite

Hardware-specific tests that run on the ESP32-S3 to catch issues that native
(host) tests cannot reproduce: real heap fragmentation, PSRAM integrity,
FreeRTOS contention, BLE/WiFi radio coexistence, NVS persistence, and I2C
hardware interaction.

## Running

```bash
# Run ALL device tests (requires connected ESP32-S3 via USB)
pio test -e device --filter "test_device_*"

# Run a single suite
pio test -e device --filter test_device_heap

# Run the device functional gate (core + dependent suites)
./scripts/run_device_tests.sh

# Repeat the device test gate and collect failure-rate metrics
./scripts/run_device_soak.sh --cycles 20 --cooldown-seconds 6

# Production-image bench evidence
./bench.sh
```

Production-image runtime evidence comes from the bench wrapper. Live HTTP debug
metrics are no longer part of the production maintenance surface; use SD perf
CSV and serial logs for evidence.

Bench evidence is separate from device-unit suites: `./bench.sh` measures core
and display runtime windows, while `./scripts/run_device_tests.sh` validates
hardware-specific unit behavior.

## Local board inventory

Hardware runners must address a board by a stable local alias, never by the
first serial path returned by the operating system. The tracked
`test/device/board_inventory.json` file is an empty, versioned template. Copy it
to the ignored local overlay and add the capabilities and connection identity
available on each physical board:

```bash
cp test/device/board_inventory.json test/device/board_inventory.local.json
```

```json
{
  "schema_version": 1,
  "boards": [
    {
      "alias": "bench-a",
      "capabilities": ["device-tests", "display", "lan", "serial"],
      "usb_serial": "A1B2C3D4",
      "lan_base_url": "http://192.0.2.10"
    }
  ]
}
```

Replace the example values with local values. Aliases and capabilities are
lowercase slugs. Capability names are intentionally extensible; a pinned HIL
profile selects which ones a job requires. A board advertising `serial` must
have a unique USB serial. A board may advertise `lan` without storing an
address when firmware will report it over serial; that dynamic mode also
requires `serial` and its USB identity so the resolver can collect from the
exact selected board itself.

Resolve only the capabilities a job needs:

```bash
python3 scripts/resolve_hil_board.py bench-a \
  --capability device-tests \
  --capability serial
```

The resolver obtains PlatformIO's port inventory and exact-matches the
configured USB serial. It does not fall back to a sole or first-enumerated
device path. For deterministic runner input, pass a saved PlatformIO JSON list
with `--ports-json`; `test/device/serial_ports.local.json` is ignored for that
purpose.

For LAN jobs, `lan_base_url` is preferred. If the local inventory omits it,
request both `lan` and `serial`. The resolver opens only the port selected by
the configured USB serial and collects the most recent exact firmware line
`[WiFiClient] Connected! IP: <IPv4 address>` within the bounded
`--lan-collection-timeout-seconds` window. Caller-supplied serial captures are
not accepted, so a capture from a different port cannot be substituted.

Successful stdout is a sanitized resolver attestation only. It contains no USB
serial, device path, LAN endpoint, or unrelated board metadata. A runner that
needs resolved endpoints must explicitly retain the raw resolution under the
ignored `.artifacts/` tree and retain the sanitized attestation separately:

```bash
python3 scripts/resolve_hil_board.py dut-bench-a \
  --capability device-tests \
  --capability serial \
  --local-resolution-output .artifacts/bug-squash-hil/<run-id>/local/dut-resolution.json \
  --attestation-output .artifacts/bug-squash-hil/<run-id>/resolver-attestation.json
```

The attestation contains only the resolver schema, alias, requested
capabilities, UTC observation time, and a stable SHA-256 of the complete local
resolution. The qualification validator recomputes that digest from the unique
ignored local resolution for every DUT and rig. This binds evidence to the
actual resolver result without copying the USB identity, serial path, or LAN
endpoint into the qualification manifest.

## Suites

| Suite | Category | What it catches |
|-------|----------|-----------------|
| `test_device_heap` | **Core / Memory** | Internal SRAM leaks, fragmentation, OOM on allocation paths |
| `test_device_psram` | **Core / Memory** | PSRAM detection, large-alloc integrity, pattern-verify corruption |
| `test_device_freertos` | **Core / RTOS** | Queue overflow, semaphore contention, critical-section correctness |
| `test_device_event_bus` | **Core / Concurrency** | `SystemEventBus` under real `portMUX` locking across tasks |
| `test_device_nvs` | **Dependent / Persistence** | NVS write/read round-trip, namespace A/B toggle, quota |
| `test_device_coexistence` | **Dependent / Radio** | WiFi AP start/stop heap impact, DMA gate, BLE+WiFi contention |
| `test_device_battery` | **Dependent / Hardware** | ADC sampling, TCA9554 I2C, power-latch, button GPIO |
| `test_device_boot` | **Integration / System** | Post-setup heap baseline, millis advancing, core IDs, stack marks |
| `test_device_heap_stress` | **Stress** | Fragmentation churn, alloc/free leak checks, near-OOM (manual run) |

## Writing Device Tests

Device tests follow the same Unity pattern as native tests but with two
differences:

1. **No `#ifndef ARDUINO` path** – they only compile for the device env.
2. **Use real ESP-IDF APIs** – `heap_caps_*`, `xQueue*`, `WiFi`, `Preferences`, etc.

```cpp
#include <unity.h>
#include <Arduino.h>

void setUp() {}
void tearDown() {}

void test_something_hardware_specific() {
    TEST_ASSERT_TRUE(psramFound());
}

void setup() {
    delay(2000);  // USB CDC settle
    UNITY_BEGIN();
    RUN_TEST(test_something_hardware_specific);
    UNITY_END();
}

void loop() {}
```

## Relationship to Native Tests

- **Native tests** (`pio test -e native`): Mock hardware, test logic. Fast, runs on host.
- **Device tests** (`pio test -e device --filter "test_device_*"`): Real hardware, test
  integration. Slower, requires board connected.

The `[env:native]` config ignores `test_device_*` suites (they won't compile
without ESP32 headers). The `[env:device]` config runs both the shared native
tests *and* the device-only tests.

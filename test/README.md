# V1 Simple Unit Tests

Test suites for repo semantics, integration scenarios, and device validation.

## Running Tests

```bash
# Run all native tests
pio test -e native

# Run focused functional scenarios (integration + behavior checks)
./scripts/run_functional_tests.sh

# Run the same functional scenarios on hardware, too
./scripts/run_functional_tests.sh --with-device

# Run with verbose output
pio test -e native -v

# Run specific test suites
pio test -e native --filter test_display
pio test -e native --filter test_packet_parser
pio test -e native --filter test_wifi_boot_policy

# Compile the allow-listed AlpEventLatch production source separately
python3 scripts/run_native_tests_serial.py --linked-pilot test_alp_event_latch
python3 scripts/run_native_tests_serial.py --env native-sanitized --linked-pilot test_alp_event_latch
```

## Device Test Suite

Hardware-specific tests that find memory, coexistence, and concurrency issues
before production. Requires ESP32-S3 connected via USB.

```bash
# All device-only suites (boot → heap → PSRAM → RTOS → NVS → battery → radio)
./scripts/run_device_tests.sh

# Quick sanity (boot + heap only)
./scripts/run_device_tests.sh --quick

# Full run (all validated device-only suites; shared native suites are not yet enabled on hardware)
./scripts/run_device_tests.sh --full

# Repeat device test firmware cycles and collect flake metrics (CSV + summary)
./scripts/run_device_soak.sh --cycles 20 --cooldown-seconds 6

# Production-image bench evidence (core + display SD/serial perf evidence)
./bench.sh

# Individual suite
pio test -e device --filter test_device_heap
```

Production-image runtime evidence comes from the top-level hardware wrapper.
Live HTTP debug metrics are no longer part of the production maintenance surface; use SD perf CSV and serial logs for evidence.

### Device Suites

| Suite | Category | What it catches |
|-------|----------|-----------------|
| `test_device_boot` | Core / System | Post-boot baseline, CPU/PSRAM detection, flash/partition |
| `test_device_heap` | Core / Memory | Internal SRAM leaks, fragmentation, OOM resilience |
| `test_device_psram` | Core / Memory | PSRAM detection, 4 MB pattern-verify, write-speed sanity |
| `test_device_freertos` | Core / RTOS | Queue overflow, semaphore, cross-task communication |
| `test_device_event_bus` | Core / Concurrency | SystemEventBus under real portMUX across cores |
| `test_device_nvs` | Dependent / Persistence | NVS write/read round-trip, namespace A/B, XOR obfuscation |
| `test_device_battery` | Dependent / Hardware | ADC sampling, TCA9554 I2C, power latch, button GPIO |
| `test_device_coexistence` | Dependent / Radio | WiFi AP heap cost, DMA gate, BLE+WiFi simultaneous |
| `test_device_heap_stress` | Stress | Fragmentation churn, alloc/free leak checks, near-OOM (manual run) |

See [device/README.md](device/README.md) for detailed documentation.

## Functional Test Gate

Use `./scripts/run_functional_tests.sh` when you want behavior-level coverage
in addition to broad `native` unit coverage. The functional gate runs:

- `test_wifi_boot_policy` (WiFi startup gating)
- `test_wifi_manager` (WiFi state behavior)

Each run writes machine-readable reports to:

- `.artifacts/test_reports/functional_<timestamp>/native.json`
- `.artifacts/test_reports/functional_<timestamp>/native.xml`
- `.artifacts/test_reports/functional_<timestamp>/native.log`

## Test Structure

```
test/
├── device/              # Device test documentation
│   └── README.md
├── fixtures/            # Real-world logs and packet captures
├── mocks/               # Mock headers for ESP32/Arduino types
│   ├── Arduino.h        # Basic Arduino types
│   ├── display_driver.h # Display/graphics mocks
│   ├── settings.h       # Settings manager mock
│   ├── battery_manager.h # Battery manager mock
│   ├── ble_client.h     # BLE client mock
│   └── freertos/        # FreeRTOS stubs
├── test_device_boot/    # [DEVICE] System baseline + chip detection
├── test_device_heap/    # [DEVICE] Heap fragmentation + leak detection
├── test_device_psram/   # [DEVICE] PSRAM integrity + write speed
├── test_device_freertos/ # [DEVICE] Queue/semaphore/cross-task
├── test_device_event_bus/ # [DEVICE] Event bus concurrency
├── test_device_nvs/     # [DEVICE] NVS persistence round-trip
├── test_device_battery/ # [DEVICE] ADC + I2C hardware
├── test_device_coexistence/ # [DEVICE] BLE/WiFi radio coexistence
├── test_alert_persistence/
├── test_display/        # Display system torture tests
├── test_packet_parser/  # V1 protocol parsing tests
├── test_wifi_manager/
├── ...                  # Additional test_* suites
└── README.md
```

## Current Baseline

Suite counts change frequently. Treat the commands below as current entry
points, not historical totals. Observability/testing authority lives in
[`docs/TESTING.md`](../docs/TESTING.md) and [`docs/PERF_SLOS.md`](../docs/PERF_SLOS.md).

| Metric | Native | Device |
|--------|--------|--------|
| Command | `pio test -e native` | `./scripts/run_device_tests.sh` |
| Authoritative gate | `./scripts/ci-test.sh` | `./bench.sh` |

For hardware evidence, `./bench.sh` records production-image `core` and
`display` SD/serial windows. Device-unit suites remain separate under
`./scripts/run_device_tests.sh`.

## Display Torture Test Categories

The `test_display` suite comprehensively tests the display system:

### Band/Direction Decoding (11 tests)
- Band priority (Laser > Ka > K > X)
- Direction bitmap parsing
- Multi-direction support

### Frequency Tolerance (4 tests)
- ±5 MHz tolerance prevents jitter redraws
- Force flag overrides tolerance
- Zero-to-nonzero detection

### Cache Invalidation (4 tests)
- `drawBaseFrame()` sets all force flags
- No redraw when state unchanged
- State changes trigger redraw
- Force flags clear after draw

### Component Caching
- Band indicator caching
- Arrow direction caching
- Signal bars caching
- Mute state transitions

### State Transitions (2 tests)
- Resting to alert
- Alert to muted

### Multi-Alert Scenarios
- Priority selection (alert-row priority bit with first-usable fallback)
- Secondary alert context for pipeline composition
- Single alert (no secondary context)

### Display State (2 tests)
- Default values
- Volume support detection

### Boundary Conditions (4 tests)
- Frequency min/max ranges
- Signal strength clamping (0-6 V1 Gen2 LED bars)
- Brightness range (0-255)
- Volume range (0-9)

### Stress Tests (6 tests)
- Rapid frequency changes within tolerance
- Rapid frequency changes beyond tolerance
- Rapid direction changes
- Rapid band changes
- Alternating mute state
- Full screen clear cycles

### Bogey Counter (4 tests)
- All digits 0-9
- Special chars (J, L, P, A, #)
- Decimal point detection
- Unknown patterns

### Alert Data (5 tests)
- Equality comparison
- Different band/direction/frequency/strength

### Color Helpers (3 tests)
- Band color mapping
- Muted state overrides
- BAND_NONE handling

### Layout (2 tests)
- Screen dimensions (640×172)
- Primary/secondary zone fit

### Test Mode State Machine (6 tests)
Tests display restore behavior after web UI tests (color preview) end.
**These tests catch the "stuck screen" bug where display didn't return to SCANNING when V1 was disconnected.**

| Test | Scenario | Expected Behavior |
|------|----------|-------------------|
| Color preview ends, V1 disconnected | Test ends while scanning | Show SCANNING (not RESTING!) |
| Color preview ends, V1 connected | Test ends with V1 idle | Show RESTING |
| Color preview ends, V1 has alerts | Test ends with active alert | Show ALERT with data |
| Ended flags clear | After processing | Flags reset (no infinite loop) |
| V1 disconnects during test | State change mid-test | Uses current state at end |
| V1 connects during test | State change mid-test | Uses current state at end |

**Key Invariant:**
```
When test mode ends:
  if (v1Connected) → showResting() or update()
  else → showScanning()  // NEVER showResting() when disconnected!
```

## Writing Tests

Tests use the Unity framework. Example:

```cpp
#include <unity.h>

void test_example() {
    TEST_ASSERT_EQUAL(42, someFunction());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_example);
    return UNITY_END();
}
```

## Known Bug Patterns to Test Against

### Display Flashing Bug Pattern

The most common display bug is calling a display function every frame without change detection:

```cpp
// ❌ BUG PATTERN - causes flashing
void loop() {
    if (!hasGpsFix) {
        display.drawGpsIndicator();  // Called EVERY FRAME!
    }
}
```

**Prevention**: All display functions must have early-exit when state unchanged:

```cpp
void V1Display::drawGpsIndicator() {
    static bool lastHasFix = false;
    bool hasFix = gpsHasFix();
    
    if (hasFix == lastHasFix) return;  // Early exit
    
    // ... actual icon draw logic ...
    lastHasFix = hasFix;
}
```

### Frequency Jitter Pattern

V1 frequency can jitter ±1-5 MHz between packets. Never use exact equality:

```cpp
// ❌ BUG PATTERN - causes constant redraws
if (priority.frequency != lastFrequency) {
    needsRedraw = true;
}

// ✅ CORRECT - use tolerance
if (abs(priority.frequency - lastFrequency) > 5) {
    needsRedraw = true;
}
```

See `test/test_display/test_display.cpp` for comprehensive frequency tolerance tests.

## Test Philosophy

1. **Test behavior, not implementation** - Tests verify what the display *should do*, not *how* it does it
2. **Torture test edge cases** - Every boundary condition, every state transition
3. **Prevent regression** - Each fixed bug gets a test to ensure it never returns
4. **Document invariants** - Tests serve as executable documentation

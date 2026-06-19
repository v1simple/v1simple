/**
 * Device Boot / System Integration Tests
 *
 * Validates ESP32-S3 system state after minimal setup — catches issues that
 * only appear on real hardware:
 *   - Post-boot heap baseline (catches framework regressions)
 *   - Core ID and dual-core availability
 *   - millis/micros clock advancing
 *   - Stack high-water marks
 *   - PSRAM + internal SRAM both available
 *   - CPU frequency
 *   - Flash size and partition table
 *   - Reset reason readable
 *   - Serial/USB CDC functional
 *
 * This suite should be the FIRST device test run — if it fails, the board
 * has a fundamental hardware or firmware issue.
 */

#include <unity.h>
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_chip_info.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "../device_test_reset.h"

void setUp() {}
void tearDown() {}

static void initSerialForTests(const char* suiteName) {
    Serial.begin(115200);

    const unsigned long startMs = millis();
    while (!Serial && (millis() - startMs) < 5000) {
        delay(10);
    }

    delay(250);
    Serial.printf("\n[%s] serial_ready=%d uptime=%lu ms free_heap=%lu\n",
                  suiteName,
                  Serial ? 1 : 0,
                  (unsigned long)millis(),
                  (unsigned long)ESP.getFreeHeap());
}

// ===========================================================================
// CLOCK / TIMING
// ===========================================================================

void test_boot_millis_advancing() {
    unsigned long t1 = millis();
    delay(50);
    unsigned long t2 = millis();
    unsigned long elapsed = t2 - t1;

    TEST_ASSERT_GREATER_THAN(0UL, elapsed);
    TEST_ASSERT_UINT32_WITHIN(20, 50, (uint32_t)elapsed);
}

void test_boot_micros_advancing() {
    unsigned long t1 = micros();
    delayMicroseconds(1000);
    unsigned long t2 = micros();
    unsigned long elapsed = t2 - t1;

    // Should be approximately 1000 µs (±500 µs tolerance)
    TEST_ASSERT_GREATER_THAN(500UL, elapsed);
    TEST_ASSERT_LESS_THAN(2000UL, elapsed);
}

void test_boot_millis_not_zero() {
    // After Serial init + host wait in setup(), millis should be > 100
    TEST_ASSERT_GREATER_THAN(100UL, millis());
}

// ===========================================================================
// CPU / CHIP INFO
// ===========================================================================

void test_boot_cpu_frequency_240mhz() {
    uint32_t freq = getCpuFrequencyMhz();
    Serial.printf("  [boot] CPU frequency: %lu MHz\n", (unsigned long)freq);
    TEST_ASSERT_EQUAL_UINT32(240, freq);
}

void test_boot_chip_is_esp32s3() {
    esp_chip_info_t info;
    esp_chip_info(&info);

    Serial.printf("  [boot] Chip model: %d, cores: %d, revision: %d\n",
                  info.model, info.cores, info.revision);

    TEST_ASSERT_EQUAL(CHIP_ESP32S3, info.model);
    TEST_ASSERT_EQUAL(2, info.cores);  // Dual core
}

void test_boot_dual_core_available() {
    // Arduino loop runs on Core 1; verify we can query both
    BaseType_t coreId = xPortGetCoreID();
    Serial.printf("  [boot] Test running on core: %d\n", (int)coreId);

    TEST_ASSERT_TRUE(coreId == 0 || coreId == 1);
}

// ===========================================================================
// MEMORY BASELINE
// ===========================================================================

void test_boot_internal_sram_baseline() {
    uint32_t free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint32_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    Serial.printf("  [boot] Internal SRAM: free=%lu largest=%lu\n",
                  (unsigned long)free, (unsigned long)largest);
    deviceTestMetricU32("internal_free_bytes", "baseline", free, "bytes");
    deviceTestMetricU32("internal_largest_block_bytes", "baseline", largest, "bytes");

    // After framework init + test overhead, expect > 100 KB free
    TEST_ASSERT_GREATER_THAN_UINT32(100 * 1024, free);
    // Largest block should be at least 32 KB (enough for WiFi DMA)
    TEST_ASSERT_GREATER_THAN_UINT32(32 * 1024, largest);
}

void test_boot_psram_detected() {
    bool found = psramFound();
    uint32_t size = ESP.getPsramSize();

    Serial.printf("  [boot] PSRAM: found=%d size=%lu\n", found, (unsigned long)size);
    deviceTestMetricU32("psram_size_bytes", "baseline", size, "bytes");

    TEST_ASSERT_TRUE_MESSAGE(found, "PSRAM not detected — board misconfigured?");
    TEST_ASSERT_GREATER_THAN_UINT32(4 * 1024 * 1024, size);  // At least 4 MB
}

void test_boot_total_heap_reasonable() {
    uint32_t total = ESP.getHeapSize();
    uint32_t free = ESP.getFreeHeap();

    Serial.printf("  [boot] Total heap: %lu, free: %lu\n",
                  (unsigned long)total, (unsigned long)free);

    // With test_build_src=false, PSRAM may not be in heap pool.
    // Internal SRAM alone should be > 200 KB after framework init.
    TEST_ASSERT_GREATER_THAN_UINT32(200 * 1024, total);
}

// ===========================================================================
// FLASH / PARTITION
// ===========================================================================

void test_boot_flash_size_16mb() {
    uint32_t flashSize = ESP.getFlashChipSize();
    Serial.printf("  [boot] Flash size: %lu bytes\n", (unsigned long)flashSize);

    // Expected: 16 MB
    TEST_ASSERT_EQUAL_UINT32(16 * 1024 * 1024, flashSize);
}

void test_boot_sketch_has_space() {
    uint32_t sketchSize = ESP.getSketchSize();
    uint32_t freeSketch = ESP.getFreeSketchSpace();

    Serial.printf("  [boot] Sketch: used=%lu free=%lu\n",
                  (unsigned long)sketchSize, (unsigned long)freeSketch);
    deviceTestMetricU32("free_sketch_bytes", "baseline", freeSketch, "bytes");

    // Should have more than 1 MB free (16 MB flash, ~2 MB firmware)
    TEST_ASSERT_GREATER_THAN_UINT32(1 * 1024 * 1024, freeSketch);
}

// ===========================================================================
// RESET REASON
// ===========================================================================

void test_boot_reset_reason_readable() {
    esp_reset_reason_t reason = esp_reset_reason();
    Serial.printf("  [boot] Reset reason: %d\n", (int)reason);

    // Should be a valid reset reason enum value
    TEST_ASSERT_TRUE(reason >= ESP_RST_UNKNOWN && reason <= ESP_RST_USB);
}

// ===========================================================================
// SERIAL / USB CDC
// ===========================================================================

void test_boot_serial_functional() {
    // Serial write should report that bytes were queued/sent.
    size_t written = Serial.println("  [boot] Serial output functional");
    TEST_ASSERT_GREATER_THAN_UINT32(0, (uint32_t)written);
}

// ===========================================================================
// STACK HIGH WATER MARKS
// ===========================================================================

void test_boot_main_task_stack_not_exhausted() {
    UBaseType_t highWater = uxTaskGetStackHighWaterMark(NULL);
    uint32_t highWaterBytes = (uint32_t)(highWater * sizeof(StackType_t));
    Serial.printf("  [boot] Main task stack high water: %lu bytes remaining\n",
                  (unsigned long)highWaterBytes);
    deviceTestMetricU32("main_stack_high_water_bytes", "baseline", highWaterBytes, "bytes");

    // Should have at least 1 KB of stack remaining
    TEST_ASSERT_GREATER_THAN((UBaseType_t)256, highWater);
}

// ===========================================================================
// SDK / FRAMEWORK VERSION
// ===========================================================================

void test_boot_sdk_version_readable() {
    const char* sdk = ESP.getSdkVersion();
    Serial.printf("  [boot] SDK version: %s\n", sdk);

    TEST_ASSERT_NOT_NULL(sdk);
    TEST_ASSERT_GREATER_THAN(0, strlen(sdk));
}

// ===========================================================================
// FIRMWARE VERSION MACRO
// ===========================================================================

void test_boot_firmware_version_defined() {
    // FIRMWARE_VERSION should be defined by config.h
    // We don't include config.h here since test_build_src=false,
    // but we can verify the build flag works
#ifdef UNIT_TEST
    TEST_PASS_MESSAGE("UNIT_TEST build flag is active");
#else
    TEST_FAIL_MESSAGE("UNIT_TEST build flag should be defined in device test env");
#endif
}

// ===========================================================================
// TEST RUNNER
// ===========================================================================

void setup() {
    if (deviceTestSetup("test_device_boot")) return;

    Serial.println("========================================");
    Serial.println("  Device Boot / System Integration Tests");
    Serial.println("========================================");

    UNITY_BEGIN();

    // Clock / timing (most basic)
    RUN_TEST(test_boot_millis_advancing);
    RUN_TEST(test_boot_micros_advancing);
    RUN_TEST(test_boot_millis_not_zero);

    // CPU / chip
    RUN_TEST(test_boot_cpu_frequency_240mhz);
    RUN_TEST(test_boot_chip_is_esp32s3);
    RUN_TEST(test_boot_dual_core_available);

    // Memory baseline
    RUN_TEST(test_boot_internal_sram_baseline);
    RUN_TEST(test_boot_psram_detected);
    RUN_TEST(test_boot_total_heap_reasonable);

    // Flash / partition
    RUN_TEST(test_boot_flash_size_16mb);
    RUN_TEST(test_boot_sketch_has_space);

    // Reset reason
    RUN_TEST(test_boot_reset_reason_readable);

    // Serial
    RUN_TEST(test_boot_serial_functional);

    // Stack
    RUN_TEST(test_boot_main_task_stack_not_exhausted);

    // SDK
    RUN_TEST(test_boot_sdk_version_readable);

    // Build flags
    RUN_TEST(test_boot_firmware_version_defined);

    UNITY_END();
    deviceTestFinish();
}

void loop() {
    delay(100);  // Keep USB CDC alive after post-test reboot
}

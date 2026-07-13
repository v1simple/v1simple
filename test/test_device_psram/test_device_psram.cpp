/**
 * Device PSRAM Tests
 *
 * Validates PSRAM (8 MB OPI on Waveshare ESP32-S3-Touch-LCD-3.49):
 *   - Detection and reported size
 *   - Large allocation integrity (read-back pattern verify)
 *   - Multi-block allocation concurrent with internal SRAM
 *   - Fragmentation recovery
 *   - Write-speed sanity (detect misconfigured bus clock)
 *
 * PSRAM problems are insidious — they often manifest as silent data corruption
 * or random crashes under load.  These tests catch configuration regressions
 * (clock speed, OPI mode, memory mapping) early.
 */

#include <unity.h>
#include <Arduino.h>
#include <esp_heap_caps.h>
#include "../device_test_reset.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static uint32_t psramFree() {
    return heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
}

static uint32_t psramLargest() {
    return heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
}

void setUp() {}
void tearDown() {}

// ===========================================================================
// PSRAM DETECTION
// ===========================================================================

void test_psram_detected() {
    TEST_ASSERT_TRUE_MESSAGE(psramFound(), "PSRAM not detected — check board config");
}

void test_psram_size_is_8mb() {
    if (!psramFound()) { TEST_IGNORE_MESSAGE("No PSRAM"); return; }
    uint32_t size = ESP.getPsramSize();
    // Expected: ~8 MB (8,388,608 bytes). Allow ±512 KB for overhead.
    TEST_ASSERT_UINT32_WITHIN(512 * 1024, 8 * 1024 * 1024, size);
    Serial.printf("  [psram] total size: %lu bytes\n", (unsigned long)size);
}

void test_psram_free_reasonable_at_boot() {
    if (!psramFound()) { TEST_IGNORE_MESSAGE("No PSRAM"); return; }
    uint32_t free = psramFree();
    // After framework init, at least 6 MB should be free
    TEST_ASSERT_GREATER_THAN_UINT32(6 * 1024 * 1024, free);
    Serial.printf("  [psram] free: %lu bytes\n", (unsigned long)free);
}

// ===========================================================================
// ALLOCATION / INTEGRITY
// ===========================================================================

void test_psram_1mb_alloc_pattern_verify() {
    if (!psramFound()) { TEST_IGNORE_MESSAGE("No PSRAM"); return; }

    static constexpr size_t SIZE = 1024 * 1024;  // 1 MB
    uint8_t* buf = (uint8_t*)heap_caps_malloc(SIZE, MALLOC_CAP_SPIRAM);
    TEST_ASSERT_NOT_NULL_MESSAGE(buf, "Failed to allocate 1 MB from PSRAM");

    // Write ascending byte pattern
    for (size_t i = 0; i < SIZE; i++) {
        buf[i] = (uint8_t)(i & 0xFF);
    }

    // Verify
    bool ok = true;
    size_t firstMismatch = 0;
    for (size_t i = 0; i < SIZE; i++) {
        if (buf[i] != (uint8_t)(i & 0xFF)) {
            ok = false;
            firstMismatch = i;
            break;
        }
    }

    heap_caps_free(buf);
    if (!ok) {
        char msg[80];
        snprintf(msg, sizeof(msg), "PSRAM data corruption at offset %u", (unsigned)firstMismatch);
        TEST_FAIL_MESSAGE(msg);
    }
}

void test_psram_4mb_alloc_pattern_verify() {
    if (!psramFound()) { TEST_IGNORE_MESSAGE("No PSRAM"); return; }

    static constexpr size_t SIZE = 4 * 1024 * 1024;  // 4 MB
    uint32_t* buf = (uint32_t*)heap_caps_malloc(SIZE, MALLOC_CAP_SPIRAM);
    TEST_ASSERT_NOT_NULL_MESSAGE(buf, "Failed to allocate 4 MB from PSRAM");

    size_t count = SIZE / sizeof(uint32_t);

    // Write 32-bit pattern (faster than byte-wise)
    for (size_t i = 0; i < count; i++) {
        buf[i] = (uint32_t)(i ^ 0xDEADBEEF);
    }

    // Verify
    bool ok = true;
    size_t firstMismatch = 0;
    for (size_t i = 0; i < count; i++) {
        if (buf[i] != (uint32_t)(i ^ 0xDEADBEEF)) {
            ok = false;
            firstMismatch = i;
            break;
        }
    }

    heap_caps_free(buf);
    if (!ok) {
        char msg[80];
        snprintf(msg, sizeof(msg), "PSRAM 4 MB corruption at word %u", (unsigned)firstMismatch);
        TEST_FAIL_MESSAGE(msg);
    }
}

// ===========================================================================
// MULTIPLE CONCURRENT ALLOCATIONS
// ===========================================================================

void test_psram_multiple_blocks_independent() {
    if (!psramFound()) { TEST_IGNORE_MESSAGE("No PSRAM"); return; }

    static constexpr size_t BLOCK_SIZE = 256 * 1024;  // 256 KB
    static constexpr int NUM_BLOCKS = 8;
    uint8_t* blocks[NUM_BLOCKS] = {};

    for (int i = 0; i < NUM_BLOCKS; i++) {
        blocks[i] = (uint8_t*)heap_caps_malloc(BLOCK_SIZE, MALLOC_CAP_SPIRAM);
        TEST_ASSERT_NOT_NULL(blocks[i]);
        memset(blocks[i], (uint8_t)(i + 1), BLOCK_SIZE);
    }

    // Verify each block still holds its pattern
    for (int i = 0; i < NUM_BLOCKS; i++) {
        uint8_t expected = (uint8_t)(i + 1);
        for (size_t j = 0; j < BLOCK_SIZE; j += 1024) {  // Sample every 1 KB
            TEST_ASSERT_EQUAL_UINT8(expected, blocks[i][j]);
        }
    }

    for (int i = 0; i < NUM_BLOCKS; i++) {
        heap_caps_free(blocks[i]);
    }
}

// ===========================================================================
// PSRAM + INTERNAL SRAM COEXISTENCE
// ===========================================================================

void test_psram_alloc_does_not_consume_internal() {
    if (!psramFound()) { TEST_IGNORE_MESSAGE("No PSRAM"); return; }

    uint32_t internalBefore = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    void* psramBlock = heap_caps_malloc(512 * 1024, MALLOC_CAP_SPIRAM);
    TEST_ASSERT_NOT_NULL(psramBlock);

    uint32_t internalAfter = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    heap_caps_free(psramBlock);

    // Internal SRAM should not have decreased by more than 256 bytes (metadata)
    TEST_ASSERT_UINT32_WITHIN(256, internalBefore, internalAfter);
}

// ===========================================================================
// FRAGMENTATION RECOVERY
// ===========================================================================

void test_psram_fragmentation_recovery() {
    if (!psramFound()) { TEST_IGNORE_MESSAGE("No PSRAM"); return; }

    uint32_t baselineLargest = psramLargest();

    static constexpr int N = 32;
    static constexpr size_t BLOCK = 64 * 1024;
    void* blocks[N] = {};

    for (int i = 0; i < N; i++) {
        blocks[i] = heap_caps_malloc(BLOCK, MALLOC_CAP_SPIRAM);
        TEST_ASSERT_NOT_NULL(blocks[i]);
    }

    // Free odd-indexed blocks → hole pattern
    for (int i = 1; i < N; i += 2) {
        heap_caps_free(blocks[i]);
        blocks[i] = nullptr;
    }

    uint32_t fragmentedLargest = psramLargest();
    Serial.printf("  [psram] fragmented largest: %lu (baseline: %lu)\n",
                  (unsigned long)fragmentedLargest, (unsigned long)baselineLargest);

    // Free remaining
    for (int i = 0; i < N; i++) {
        if (blocks[i]) heap_caps_free(blocks[i]);
    }

    uint32_t recoveredLargest = psramLargest();
    // After full free, should recover to within 10% of baseline
    TEST_ASSERT_GREATER_THAN_UINT32(baselineLargest * 90 / 100, recoveredLargest);
}

// ===========================================================================
// WRITE THROUGHPUT SANITY
// ===========================================================================

void test_psram_write_speed_sanity() {
    if (!psramFound()) { TEST_IGNORE_MESSAGE("No PSRAM"); return; }

    static constexpr size_t SIZE = 1024 * 1024;  // 1 MB
    uint32_t* buf = (uint32_t*)heap_caps_malloc(SIZE, MALLOC_CAP_SPIRAM);
    TEST_ASSERT_NOT_NULL(buf);

    unsigned long startUs = micros();
    size_t words = SIZE / sizeof(uint32_t);
    for (size_t i = 0; i < words; i++) {
        buf[i] = i;
    }
    unsigned long elapsedUs = micros() - startUs;

    heap_caps_free(buf);

    // At OPI 80 MHz we expect ~40+ MB/s write; set floor at 10 MB/s to catch
    // misconfigured SPI mode.
    float mbPerSec = (float)SIZE / (float)elapsedUs;  // bytes/us = MB/s
    Serial.printf("  [psram] write speed: %.1f MB/s (%lu us for 1 MB)\n",
                  mbPerSec, (unsigned long)elapsedUs);
    TEST_ASSERT_GREATER_THAN(10.0f, mbPerSec);
}

// ===========================================================================
// TEST RUNNER
// ===========================================================================

void setup() {
    if (deviceTestSetup("test_device_psram")) return;
    UNITY_BEGIN();

    RUN_TEST(test_psram_detected);
    RUN_TEST(test_psram_size_is_8mb);
    RUN_TEST(test_psram_free_reasonable_at_boot);

    RUN_TEST(test_psram_1mb_alloc_pattern_verify);
    RUN_TEST(test_psram_4mb_alloc_pattern_verify);

    RUN_TEST(test_psram_multiple_blocks_independent);
    RUN_TEST(test_psram_alloc_does_not_consume_internal);

    RUN_TEST(test_psram_fragmentation_recovery);
    RUN_TEST(test_psram_write_speed_sanity);

    UNITY_END();
    deviceTestFinish();
}

void loop() {
    delay(100);  // Keep USB CDC alive after post-test reboot
}

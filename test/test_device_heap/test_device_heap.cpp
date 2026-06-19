/**
 * Device Heap / Memory Tests
 *
 * Validates real ESP32-S3 heap behavior that native mocks cannot reproduce:
 *   - Internal SRAM availability and allocation
 *   - Heap fragmentation under churn
 *   - Leak detection across alloc/free cycles
 *   - heap_caps API consistency
 *   - OOM resilience (large alloc that should fail gracefully)
 *
 * These tests are the first line of defense against memory regressions that
 * only manifest on hardware after extended runtime.
 */

#include <unity.h>
#include <Arduino.h>
#include <esp_heap_caps.h>
#include "../device_test_reset.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

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

static uint32_t internalFree() {
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static uint32_t internalLargest() {
    return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static uint32_t totalFree() {
    return heap_caps_get_free_size(MALLOC_CAP_8BIT);
}

// ---------------------------------------------------------------------------
// setUp / tearDown
// ---------------------------------------------------------------------------

void setUp() {}
void tearDown() {}

// ===========================================================================
// INTERNAL SRAM SANITY
// ===========================================================================

void test_heap_internal_free_is_sane() {
    uint32_t free = internalFree();
    // ESP32-S3 has ~380 KB internal SRAM total; after framework init expect > 80 KB
    TEST_ASSERT_GREATER_THAN_UINT32(80 * 1024, free);
    Serial.printf("  [heap] internal free: %lu bytes\n", (unsigned long)free);
    deviceTestMetricU32("baseline_internal_free_bytes", "baseline", free, "bytes");
}

void test_heap_internal_largest_block_positive() {
    uint32_t largest = internalLargest();
    // Largest contiguous block should be at least 16 KB for WiFi DMA buffers
    TEST_ASSERT_GREATER_THAN_UINT32(16 * 1024, largest);
    Serial.printf("  [heap] internal largest block: %lu bytes\n", (unsigned long)largest);
    deviceTestMetricU32("baseline_internal_largest_block_bytes", "baseline", largest, "bytes");
}

void test_heap_total_free_includes_psram() {
    uint32_t total = totalFree();
    uint32_t internal = internalFree();
    // If PSRAM is present, total should be significantly larger than internal
    if (psramFound()) {
        TEST_ASSERT_GREATER_THAN_UINT32(internal, total);
    } else {
        // Without PSRAM they should be roughly equal
        TEST_ASSERT_UINT32_WITHIN(internal / 4, internal, total);
    }
}

// ===========================================================================
// ALLOCATION / FREE ROUND-TRIP
// ===========================================================================

void test_heap_internal_alloc_free_no_leak() {
    uint32_t before = internalFree();

    // Allocate 4 KB from internal SRAM
    void* ptr = heap_caps_malloc(4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    TEST_ASSERT_NOT_NULL(ptr);

    uint32_t during = internalFree();
    TEST_ASSERT_LESS_THAN_UINT32(before, during);  // during < before

    heap_caps_free(ptr);

    uint32_t after = internalFree();
    uint32_t delta = (before > after) ? (before - after) : (after - before);
    deviceTestMetricU32("internal_alloc_recovery_delta_bytes", "recovery", delta, "bytes");
    // Allow 256-byte tolerance for heap metadata overhead
    TEST_ASSERT_UINT32_WITHIN(256, before, after);
}

void test_heap_spiram_alloc_free_no_leak() {
    if (!psramFound()) {
        TEST_IGNORE_MESSAGE("PSRAM not present - skipping");
        return;
    }

    uint32_t before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    void* ptr = heap_caps_malloc(64 * 1024, MALLOC_CAP_SPIRAM);
    TEST_ASSERT_NOT_NULL(ptr);

    heap_caps_free(ptr);

    uint32_t after = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    uint32_t delta = (before > after) ? (before - after) : (after - before);
    deviceTestMetricU32("spiram_alloc_recovery_delta_bytes", "recovery", delta, "bytes");
    TEST_ASSERT_UINT32_WITHIN(512, before, after);
}

// ===========================================================================
// FRAGMENTATION STRESS
// ===========================================================================

void test_heap_fragmentation_under_churn() {
    // Simulate the pattern that causes fragmentation in long drives:
    // many small allocations, free alternate ones, then try a larger alloc.
    static constexpr int N = 64;
    static constexpr size_t SMALL_SIZE = 256;
    void* blocks[N] = {};

    uint32_t baselineLargest = internalLargest();

    // Allocate N small blocks
    for (int i = 0; i < N; i++) {
        blocks[i] = heap_caps_malloc(SMALL_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        TEST_ASSERT_NOT_NULL_MESSAGE(blocks[i], "Small alloc failed during churn setup");
    }

    // Free every other block → fragmentation
    for (int i = 0; i < N; i += 2) {
        heap_caps_free(blocks[i]);
        blocks[i] = nullptr;
    }

    uint32_t fragLargest = internalLargest();
    Serial.printf("  [heap] post-fragment largest block: %lu (baseline: %lu)\n",
                  (unsigned long)fragLargest, (unsigned long)baselineLargest);

    // Largest block should still be usable (> 8 KB) even after fragmentation
    TEST_ASSERT_GREATER_THAN_UINT32(8 * 1024, fragLargest);

    // Clean up remaining blocks
    for (int i = 0; i < N; i++) {
        if (blocks[i]) {
            heap_caps_free(blocks[i]);
        }
    }

    // After full cleanup, largest block should recover close to baseline
    uint32_t recoveredLargest = internalLargest();
    // Allow 20% degradation tolerance
    TEST_ASSERT_GREATER_THAN_UINT32(baselineLargest * 80 / 100, recoveredLargest);
}

// ===========================================================================
// REPEATED ALLOC/FREE CYCLE LEAK CHECK
// ===========================================================================

void test_heap_repeated_alloc_free_no_cumulative_leak() {
    uint32_t baseline = internalFree();

    for (int cycle = 0; cycle < 100; cycle++) {
        void* p = heap_caps_malloc(1024, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        TEST_ASSERT_NOT_NULL(p);
        memset(p, 0xAA, 1024);  // Touch memory
        heap_caps_free(p);
    }

    uint32_t afterCycles = internalFree();
    // No cumulative leak: should be within 512 bytes of baseline
    TEST_ASSERT_UINT32_WITHIN(512, baseline, afterCycles);
}

// ===========================================================================
// OOM RESILIENCE
// ===========================================================================

void test_heap_oom_returns_null_not_crash() {
    // Try to allocate more internal SRAM than available — must not crash
    uint32_t available = internalFree();
    void* ptr = heap_caps_malloc(available + (1024 * 1024), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    TEST_ASSERT_NULL_MESSAGE(ptr, "OOM should return NULL, not succeed");
}

void test_heap_near_oom_alloc_and_recover() {
    // Allocate most of internal SRAM, then free and verify recovery
    uint32_t available = internalFree();
    size_t allocSize = available * 80 / 100;  // 80% of free

    void* big = heap_caps_malloc(allocSize, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (big == nullptr) {
        // If even 80% fails, try 50%
        allocSize = available / 2;
        big = heap_caps_malloc(allocSize, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    TEST_ASSERT_NOT_NULL_MESSAGE(big, "Should be able to allocate at least 50% of free heap");

    uint32_t remaining = internalFree();
    Serial.printf("  [heap] allocated %lu, remaining: %lu\n",
                  (unsigned long)allocSize, (unsigned long)remaining);

    heap_caps_free(big);

    uint32_t recovered = internalFree();
    TEST_ASSERT_UINT32_WITHIN(1024, available, recovered);
}

// ===========================================================================
// HEAP CAPS API CONSISTENCY
// ===========================================================================

void test_heap_free_size_monotonic_with_alloc() {
    uint32_t a = internalFree();
    void* p1 = heap_caps_malloc(2048, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    TEST_ASSERT_NOT_NULL(p1);
    uint32_t b = internalFree();
    TEST_ASSERT_LESS_THAN_UINT32(a, b);  // b < a

    void* p2 = heap_caps_malloc(2048, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    TEST_ASSERT_NOT_NULL(p2);
    uint32_t c = internalFree();
    TEST_ASSERT_LESS_THAN_UINT32(b, c);  // c < b

    heap_caps_free(p2);
    heap_caps_free(p1);
}

void test_heap_largest_block_lte_free_size() {
    uint32_t free = internalFree();
    uint32_t largest = internalLargest();
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(free, largest);
}

// ===========================================================================
// TEST RUNNER
// ===========================================================================

void setup() {
    if (deviceTestSetup("test_device_heap")) return;
    Serial.println("  Device Heap / Memory Tests");
    UNITY_BEGIN();

    // Internal SRAM sanity
    RUN_TEST(test_heap_internal_free_is_sane);
    RUN_TEST(test_heap_internal_largest_block_positive);
    RUN_TEST(test_heap_total_free_includes_psram);

    // Alloc/free round-trip
    RUN_TEST(test_heap_internal_alloc_free_no_leak);
    RUN_TEST(test_heap_spiram_alloc_free_no_leak);

    // API consistency
    RUN_TEST(test_heap_free_size_monotonic_with_alloc);
    RUN_TEST(test_heap_largest_block_lte_free_size);

    // Stress cases moved to test_device_heap_stress so quick/device runs stay
    // connectivity-safe on fragile hardware.
    Serial.println("  [heap] stress tests skipped (use test_device_heap_stress)");

    UNITY_END();
    deviceTestFinish();
}

void loop() {
    delay(100);  // Keep USB CDC alive after post-test reboot
}

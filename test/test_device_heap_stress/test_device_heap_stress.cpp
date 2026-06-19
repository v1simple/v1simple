/**
 * Device Heap Stress Tests
 *
 * Runs aggressive heap scenarios that may destabilize fragile hardware:
 *   - Fragmentation churn
 *   - Repeated alloc/free leak checks
 *   - Near-OOM behavior
 *
 * Keep this suite out of default quick/device runs. Run manually when
 * connectivity is known-good.
 */

#include <unity.h>
#include <Arduino.h>
#include <esp_heap_caps.h>
#include "../device_test_reset.h"

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

void setUp() {}
void tearDown() {}

void test_heap_fragmentation_under_churn() {
    static constexpr int N = 64;
    static constexpr size_t SMALL_SIZE = 256;
    void* blocks[N] = {};

    uint32_t baselineLargest = internalLargest();

    for (int i = 0; i < N; i++) {
        blocks[i] = heap_caps_malloc(SMALL_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        TEST_ASSERT_NOT_NULL_MESSAGE(blocks[i], "Small alloc failed during churn setup");
    }

    for (int i = 0; i < N; i += 2) {
        heap_caps_free(blocks[i]);
        blocks[i] = nullptr;
    }

    uint32_t fragLargest = internalLargest();
    Serial.printf("  [heap_stress] post-fragment largest block: %lu (baseline: %lu)\n",
                  (unsigned long)fragLargest, (unsigned long)baselineLargest);

    TEST_ASSERT_GREATER_THAN_UINT32(8 * 1024, fragLargest);

    for (int i = 0; i < N; i++) {
        if (blocks[i]) {
            heap_caps_free(blocks[i]);
        }
    }

    uint32_t recoveredLargest = internalLargest();
    TEST_ASSERT_GREATER_THAN_UINT32(baselineLargest * 80 / 100, recoveredLargest);
}

void test_heap_repeated_alloc_free_no_cumulative_leak() {
    uint32_t baseline = internalFree();

    for (int cycle = 0; cycle < 100; cycle++) {
        void* p = heap_caps_malloc(1024, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        TEST_ASSERT_NOT_NULL(p);
        memset(p, 0xAA, 1024);
        heap_caps_free(p);
    }

    uint32_t afterCycles = internalFree();
    TEST_ASSERT_UINT32_WITHIN(512, baseline, afterCycles);
}

void test_heap_oom_returns_null_not_crash() {
    uint32_t available = internalFree();
    void* ptr = heap_caps_malloc(available + (1024 * 1024), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    TEST_ASSERT_NULL_MESSAGE(ptr, "OOM should return NULL, not succeed");
}

void test_heap_near_oom_alloc_and_recover() {
    uint32_t available = internalFree();
    size_t allocSize = available * 80 / 100;

    void* big = heap_caps_malloc(allocSize, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (big == nullptr) {
        allocSize = available / 2;
        big = heap_caps_malloc(allocSize, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    TEST_ASSERT_NOT_NULL_MESSAGE(big, "Should be able to allocate at least 50% of free heap");

    uint32_t remaining = internalFree();
    Serial.printf("  [heap_stress] allocated=%lu remaining=%lu\n",
                  (unsigned long)allocSize, (unsigned long)remaining);

    heap_caps_free(big);

    uint32_t recovered = internalFree();
    TEST_ASSERT_UINT32_WITHIN(1024, available, recovered);
}

void setup() {
    if (deviceTestSetup("test_device_heap_stress")) return;
    Serial.println("  Device Heap Stress Tests");
    UNITY_BEGIN();

    RUN_TEST(test_heap_fragmentation_under_churn);
    RUN_TEST(test_heap_repeated_alloc_free_no_cumulative_leak);
    RUN_TEST(test_heap_oom_returns_null_not_crash);
    RUN_TEST(test_heap_near_oom_alloc_and_recover);

    UNITY_END();
    deviceTestFinish();
}

void loop() {
    delay(100);  // Keep USB CDC alive after post-test reboot
}

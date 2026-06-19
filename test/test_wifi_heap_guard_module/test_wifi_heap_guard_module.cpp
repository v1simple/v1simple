#include <unity.h>

#include "../../src/modules/wifi/wifi_heap_guard_module.cpp"

void test_ap_mode_reports_low_heap_without_jitter() {
    WifiHeapGuardModule module;
    WifiHeapGuardInput input;
    input.staRadioOn = false;
    input.dualRadioMode = false;
    input.staOnlyMode = false;
    input.freeInternal = 16000;
    input.largestInternal = 9000;
    input.criticalFree = 16384;
    input.criticalBlock = 8192;
    input.apStaFreeJitterTolerance = 256;
    input.staOnlyBlockJitterTolerance = 128;

    const WifiHeapGuardResult result = module.evaluate(input);
    TEST_ASSERT_EQUAL_STRING("AP", result.modeLabel);
    TEST_ASSERT_TRUE(result.freeLow);
    TEST_ASSERT_FALSE(result.blockLow);
    TEST_ASSERT_TRUE(result.lowHeap);
}

void test_ap_sta_mode_ignores_small_free_deficit() {
    WifiHeapGuardModule module;
    WifiHeapGuardInput input;
    input.staRadioOn = true;
    input.dualRadioMode = true;
    input.staOnlyMode = false;
    input.freeInternal = 20280;  // 200 below floor
    input.largestInternal = 9000;
    input.criticalFree = 20480;
    input.criticalBlock = 8192;
    input.apStaFreeJitterTolerance = 256;
    input.staOnlyBlockJitterTolerance = 128;

    const WifiHeapGuardResult result = module.evaluate(input);
    TEST_ASSERT_EQUAL_STRING("AP+STA", result.modeLabel);
    TEST_ASSERT_FALSE(result.freeLow);
    TEST_ASSERT_FALSE(result.blockLow);
    TEST_ASSERT_FALSE(result.lowHeap);
}

void test_ap_sta_mode_marks_large_free_deficit_low() {
    WifiHeapGuardModule module;
    WifiHeapGuardInput input;
    input.staRadioOn = true;
    input.dualRadioMode = true;
    input.staOnlyMode = false;
    input.freeInternal = 20100;  // 380 below floor
    input.largestInternal = 9000;
    input.criticalFree = 20480;
    input.criticalBlock = 8192;
    input.apStaFreeJitterTolerance = 256;
    input.staOnlyBlockJitterTolerance = 128;

    const WifiHeapGuardResult result = module.evaluate(input);
    TEST_ASSERT_TRUE(result.freeLow);
    TEST_ASSERT_FALSE(result.blockLow);
    TEST_ASSERT_TRUE(result.lowHeap);
}

void test_sta_mode_ignores_small_block_deficit() {
    WifiHeapGuardModule module;
    WifiHeapGuardInput input;
    input.staRadioOn = true;
    input.dualRadioMode = false;
    input.staOnlyMode = true;
    input.freeInternal = 20000;
    input.largestInternal = 7090;  // 78 below floor
    input.criticalFree = 16384;
    input.criticalBlock = 7168;
    input.apStaFreeJitterTolerance = 256;
    input.staOnlyBlockJitterTolerance = 128;

    const WifiHeapGuardResult result = module.evaluate(input);
    TEST_ASSERT_EQUAL_STRING("STA", result.modeLabel);
    TEST_ASSERT_FALSE(result.freeLow);
    TEST_ASSERT_FALSE(result.blockLow);
    TEST_ASSERT_FALSE(result.lowHeap);
}

void test_sta_mode_marks_large_block_deficit_low() {
    WifiHeapGuardModule module;
    WifiHeapGuardInput input;
    input.staRadioOn = true;
    input.dualRadioMode = false;
    input.staOnlyMode = true;
    input.freeInternal = 20000;
    input.largestInternal = 7000;  // 168 below floor
    input.criticalFree = 16384;
    input.criticalBlock = 7168;
    input.apStaFreeJitterTolerance = 256;
    input.staOnlyBlockJitterTolerance = 128;

    const WifiHeapGuardResult result = module.evaluate(input);
    TEST_ASSERT_FALSE(result.freeLow);
    TEST_ASSERT_TRUE(result.blockLow);
    TEST_ASSERT_TRUE(result.lowHeap);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_ap_mode_reports_low_heap_without_jitter);
    RUN_TEST(test_ap_sta_mode_ignores_small_free_deficit);
    RUN_TEST(test_ap_sta_mode_marks_large_free_deficit_low);
    RUN_TEST(test_sta_mode_ignores_small_block_deficit);
    RUN_TEST(test_sta_mode_marks_large_block_deficit_low);
    return UNITY_END();
}

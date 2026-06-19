#include <unity.h>

#include "../mocks/Arduino.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../src/modules/system/loop_settings_prep_module.cpp"

static LoopSettingsPrepModule module;

enum CallId {
    CALL_TAP = 1,
    CALL_SETTINGS = 2,
};

static int callLog[16];
static size_t callLogCount = 0;

static uint32_t tapNowMs = 0;
static int providerTapCalls = 0;
static int providerSettingsCalls = 0;

static LoopSettingsPrepValues providerValues;

static void noteCall(int id) {
    if (callLogCount < (sizeof(callLog) / sizeof(callLog[0]))) {
        callLog[callLogCount++] = id;
    }
}

static void resetState() {
    callLogCount = 0;
    tapNowMs = 0;
    providerTapCalls = 0;
    providerSettingsCalls = 0;
    providerValues = LoopSettingsPrepValues{};
}

static void runProviderTap(void*, uint32_t nowMs) {
    providerTapCalls++;
    tapNowMs = nowMs;
    noteCall(CALL_TAP);
}

static LoopSettingsPrepValues readProviderSettings(void*) {
    providerSettingsCalls++;
    noteCall(CALL_SETTINGS);
    return providerValues;
}

void setUp() {
    resetState();
}

void tearDown() {}

void test_provider_path_runs_tap_then_reads_settings() {
    LoopSettingsPrepModule::Providers providers;
    providers.runTapGesture = runProviderTap;
    providers.readSettingsValues = readProviderSettings;
    module.begin(providers);

    providerValues.enableWifi = false;

    LoopSettingsPrepContext ctx;
    ctx.nowMs = 456;

    const LoopSettingsPrepValues result = module.process(ctx);

    TEST_ASSERT_EQUAL(1, providerTapCalls);
    TEST_ASSERT_EQUAL(1, providerSettingsCalls);
    TEST_ASSERT_EQUAL(456u, tapNowMs);

    TEST_ASSERT_FALSE(result.enableWifi);

    TEST_ASSERT_EQUAL(2, callLogCount);
    TEST_ASSERT_EQUAL(CALL_TAP, callLog[0]);
    TEST_ASSERT_EQUAL(CALL_SETTINGS, callLog[1]);
}

void test_missing_tap_provider_still_reads_settings_snapshot() {
    LoopSettingsPrepModule::Providers providers;
    providers.readSettingsValues = readProviderSettings;
    module.begin(providers);

    providerValues.enableWifi = false;

    const LoopSettingsPrepValues result = module.process(LoopSettingsPrepContext{});

    TEST_ASSERT_EQUAL(0, providerTapCalls);
    TEST_ASSERT_EQUAL(1, providerSettingsCalls);
    TEST_ASSERT_FALSE(result.enableWifi);
    TEST_ASSERT_EQUAL(1, callLogCount);
    TEST_ASSERT_EQUAL(CALL_SETTINGS, callLog[0]);
}

void test_empty_providers_and_context_returns_defaults() {
    LoopSettingsPrepModule::Providers providers;
    module.begin(providers);

    const LoopSettingsPrepValues result = module.process(LoopSettingsPrepContext{});

    TEST_ASSERT_TRUE(result.enableWifi);
    TEST_ASSERT_EQUAL(0, providerTapCalls);
    TEST_ASSERT_EQUAL(0, providerSettingsCalls);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_provider_path_runs_tap_then_reads_settings);
    RUN_TEST(test_missing_tap_provider_still_reads_settings_snapshot);
    RUN_TEST(test_empty_providers_and_context_returns_defaults);
    return UNITY_END();
}

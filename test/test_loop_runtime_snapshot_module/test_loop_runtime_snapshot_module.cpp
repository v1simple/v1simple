#include <unity.h>

#include "../mocks/Arduino.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../src/modules/system/loop_runtime_snapshot_module.cpp"

static LoopRuntimeSnapshotModule module;

static int providerBleCalls = 0;
static int providerDmaCalls = 0;
static int providerPreviewCalls = 0;

static bool providerBleValue = false;
static bool providerDmaValue = false;
static bool providerPreviewValue = false;

static void resetState() {
    providerBleCalls = 0;
    providerDmaCalls = 0;
    providerPreviewCalls = 0;

    providerBleValue = false;
    providerDmaValue = false;
    providerPreviewValue = false;
}

static bool readProviderBle(void*) {
    providerBleCalls++;
    return providerBleValue;
}

static bool readProviderDma(void*) {
    providerDmaCalls++;
    return providerDmaValue;
}

static bool readProviderPreview(void*) {
    providerPreviewCalls++;
    return providerPreviewValue;
}

void setUp() {
    resetState();
}

void tearDown() {}

void test_provider_snapshot_reads_all_values_once() {
    LoopRuntimeSnapshotModule::Providers providers;
    providers.readBleConnected = readProviderBle;
    providers.readCanStartDma = readProviderDma;
    providers.readDisplayPreviewRunning = readProviderPreview;
    module.begin(providers);

    providerBleValue = true;
    providerDmaValue = false;
    providerPreviewValue = true;

    const LoopRuntimeSnapshotValues result = module.process(LoopRuntimeSnapshotContext{});

    TEST_ASSERT_TRUE(result.bleConnected);
    TEST_ASSERT_FALSE(result.canStartDma);
    TEST_ASSERT_TRUE(result.displayPreviewRunning);
    TEST_ASSERT_EQUAL(1, providerBleCalls);
    TEST_ASSERT_EQUAL(1, providerDmaCalls);
    TEST_ASSERT_EQUAL(1, providerPreviewCalls);
}

void test_partial_snapshot_providers_leave_safe_defaults() {
    LoopRuntimeSnapshotModule::Providers providers;
    providers.readCanStartDma = readProviderDma;
    module.begin(providers);

    providerDmaValue = true;

    const LoopRuntimeSnapshotValues result = module.process(LoopRuntimeSnapshotContext{});

    TEST_ASSERT_FALSE(result.bleConnected);
    TEST_ASSERT_TRUE(result.canStartDma);
    TEST_ASSERT_FALSE(result.displayPreviewRunning);
    TEST_ASSERT_EQUAL(0, providerBleCalls);
    TEST_ASSERT_EQUAL(1, providerDmaCalls);
    TEST_ASSERT_EQUAL(0, providerPreviewCalls);
}

void test_empty_providers_return_safe_defaults() {
    LoopRuntimeSnapshotModule::Providers providers;
    module.begin(providers);

    const LoopRuntimeSnapshotValues result = module.process(LoopRuntimeSnapshotContext{});

    TEST_ASSERT_FALSE(result.bleConnected);
    TEST_ASSERT_FALSE(result.canStartDma);
    TEST_ASSERT_FALSE(result.displayPreviewRunning);
    TEST_ASSERT_EQUAL(0, providerBleCalls);
    TEST_ASSERT_EQUAL(0, providerDmaCalls);
    TEST_ASSERT_EQUAL(0, providerPreviewCalls);
}

void test_dma_probe_can_be_skipped_and_reuses_cached_value() {
    LoopRuntimeSnapshotModule::Providers providers;
    providers.readCanStartDma = readProviderDma;
    module.begin(providers);

    providerDmaValue = true;
    LoopRuntimeSnapshotContext ctx;
    ctx.canStartDmaProbeAllowed = true;
    const LoopRuntimeSnapshotValues first = module.process(ctx);

    TEST_ASSERT_TRUE(first.canStartDma);
    TEST_ASSERT_EQUAL(1, providerDmaCalls);

    providerDmaValue = false;
    ctx.canStartDmaProbeAllowed = false;
    const LoopRuntimeSnapshotValues skipped = module.process(ctx);

    TEST_ASSERT_TRUE(skipped.canStartDma);
    TEST_ASSERT_EQUAL(1, providerDmaCalls);
}

void test_dma_probe_skipped_without_cache_returns_safe_default() {
    LoopRuntimeSnapshotModule::Providers providers;
    providers.readCanStartDma = readProviderDma;
    module.begin(providers);

    LoopRuntimeSnapshotContext ctx;
    ctx.canStartDmaProbeAllowed = false;
    const LoopRuntimeSnapshotValues result = module.process(ctx);

    TEST_ASSERT_FALSE(result.canStartDma);
    TEST_ASSERT_EQUAL(0, providerDmaCalls);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_provider_snapshot_reads_all_values_once);
    RUN_TEST(test_partial_snapshot_providers_leave_safe_defaults);
    RUN_TEST(test_empty_providers_return_safe_defaults);
    RUN_TEST(test_dma_probe_can_be_skipped_and_reuses_cached_value);
    RUN_TEST(test_dma_probe_skipped_without_cache_returns_safe_default);
    return UNITY_END();
}

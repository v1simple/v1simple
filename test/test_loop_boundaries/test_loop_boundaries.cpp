#include <unity.h>

#include <cstddef>
#include <cstdint>

#include "../../src/modules/system/loop_ingest_module.cpp"
#include "../../src/modules/system/loop_tail_module.cpp"

namespace {

struct Probe {
    int calls[16] = {};
    size_t callCount = 0;
};

enum Call {
    BLE_PROCESS = 1,
    BLE_DRAIN = 2,
    BACKPRESSURE = 3,
    YIELD = 4,
    LOOP_MICROS = 5,
};

void recordCall(Probe& probe, Call call) {
    probe.calls[probe.callCount++] = call;
}

uint32_t loopMicrosUs(void* ctx) {
    auto& probe = *static_cast<Probe*>(ctx);
    recordCall(probe, LOOP_MICROS);
    return 250;
}

void runBleProcess(void* ctx) {
    auto& probe = *static_cast<Probe*>(ctx);
    recordCall(probe, BLE_PROCESS);
}

void runBleDrain(void* ctx) {
    auto& probe = *static_cast<Probe*>(ctx);
    recordCall(probe, BLE_DRAIN);
}

bool readBleBackpressure(void* ctx) {
    auto& probe = *static_cast<Probe*>(ctx);
    recordCall(probe, BACKPRESSURE);
    return true;
}

void yieldOneTick(void* ctx) {
    auto& probe = *static_cast<Probe*>(ctx);
    recordCall(probe, YIELD);
}

LoopIngestModule::Providers validIngestProviders(Probe& probe) {
    LoopIngestModule::Providers providers;
    providers.runBleProcess = runBleProcess;
    providers.bleProcessContext = &probe;
    providers.runBleDrain = runBleDrain;
    providers.bleDrainContext = &probe;
    providers.readBleBackpressure = readBleBackpressure;
    providers.bleBackpressureContext = &probe;
    return providers;
}

LoopTailModule::Providers validTailProviders(Probe& probe) {
    LoopTailModule::Providers providers;
    providers.loopMicrosUs = loopMicrosUs;
    providers.loopMicrosContext = &probe;
    providers.runBleDrain = runBleDrain;
    providers.bleDrainContext = &probe;
    providers.yieldOneTick = yieldOneTick;
    providers.yieldContext = &probe;
    return providers;
}

} // namespace

void setUp() {}
void tearDown() {}

void test_ingest_begin_rejects_missing_operational_providers() {
    Probe probe;
    LoopIngestModule module;
    auto providers = validIngestProviders(probe);

    providers.runBleProcess = nullptr;
    TEST_ASSERT_FALSE(module.begin(providers));

    providers = validIngestProviders(probe);
    providers.runBleDrain = nullptr;
    TEST_ASSERT_FALSE(module.begin(providers));

    providers = validIngestProviders(probe);
    providers.readBleBackpressure = nullptr;
    TEST_ASSERT_FALSE(module.begin(providers));
}

void test_ingest_runs_process_before_drain_and_backpressure_merge() {
    Probe probe;
    LoopIngestModule module;
    TEST_ASSERT_TRUE(module.begin(validIngestProviders(probe)));

    LoopIngestContext context;
    context.bleProcessEnabled = true;
    const LoopIngestResult result = module.process(context);

    const int expected[] = {BLE_PROCESS, BLE_DRAIN, BACKPRESSURE};
    TEST_ASSERT_EQUAL_INT_ARRAY(expected, probe.calls, sizeof(expected) / sizeof(expected[0]));
    TEST_ASSERT_TRUE(result.bleBackpressure);
    TEST_ASSERT_TRUE(result.skipLateNonCoreThisLoop);
    TEST_ASSERT_TRUE(result.overloadLateThisLoop);
}

void test_tail_begin_rejects_missing_operational_providers() {
    Probe probe;
    LoopTailModule module;
    auto providers = validTailProviders(probe);

    providers.loopMicrosUs = nullptr;
    TEST_ASSERT_FALSE(module.begin(providers));

    providers = validTailProviders(probe);
    providers.runBleDrain = nullptr;
    TEST_ASSERT_FALSE(module.begin(providers));

    providers = validTailProviders(probe);
    providers.yieldOneTick = nullptr;
    TEST_ASSERT_FALSE(module.begin(providers));
}

void test_tail_drains_before_yield_and_loop_finalization() {
    Probe probe;
    LoopTailModule module;
    TEST_ASSERT_TRUE(module.begin(validTailProviders(probe)));

    const uint32_t durationUs = module.process(true, 50);

    const int expected[] = {BLE_DRAIN, YIELD, LOOP_MICROS};
    TEST_ASSERT_EQUAL_INT_ARRAY(expected, probe.calls, sizeof(expected) / sizeof(expected[0]));
    TEST_ASSERT_EQUAL_UINT32(200, durationUs);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_ingest_begin_rejects_missing_operational_providers);
    RUN_TEST(test_ingest_runs_process_before_drain_and_backpressure_merge);
    RUN_TEST(test_tail_begin_rejects_missing_operational_providers);
    RUN_TEST(test_tail_drains_before_yield_and_loop_finalization);
    return UNITY_END();
}

#include <unity.h>

#include <cstddef>
#include <cstdint>

#include "../../src/modules/system/loop_ingest_module.cpp"
#include "../../src/modules/system/loop_tail_module.cpp"

namespace {

struct Probe {
    int calls[16] = {};
    size_t callCount = 0;
    uint32_t nowUs = 100;
    uint32_t bleProcessUs = 0;
    uint32_t bleDrainUs = 0;
    uint32_t loopJitterUs = 0;
};

enum Call {
    TIMESTAMP = 1,
    BLE_PROCESS = 2,
    BLE_PROCESS_RECORD = 3,
    BLE_DRAIN = 4,
    BLE_DRAIN_RECORD = 5,
    BACKPRESSURE = 6,
    YIELD = 7,
    LOOP_MICROS = 8,
    LOOP_JITTER_RECORD = 9,
};

void recordCall(Probe& probe, Call call) {
    probe.calls[probe.callCount++] = call;
}

uint32_t timestampUs(void* ctx) {
    auto& probe = *static_cast<Probe*>(ctx);
    recordCall(probe, TIMESTAMP);
    const uint32_t value = probe.nowUs;
    probe.nowUs += 25;
    return value;
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

void recordBleProcessUs(void* ctx, uint32_t elapsedUs) {
    auto& probe = *static_cast<Probe*>(ctx);
    recordCall(probe, BLE_PROCESS_RECORD);
    probe.bleProcessUs = elapsedUs;
}

void runBleDrain(void* ctx) {
    auto& probe = *static_cast<Probe*>(ctx);
    recordCall(probe, BLE_DRAIN);
}

void recordBleDrainUs(void* ctx, uint32_t elapsedUs) {
    auto& probe = *static_cast<Probe*>(ctx);
    recordCall(probe, BLE_DRAIN_RECORD);
    probe.bleDrainUs = elapsedUs;
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

void recordLoopJitterUs(void* ctx, uint32_t jitterUs) {
    auto& probe = *static_cast<Probe*>(ctx);
    recordCall(probe, LOOP_JITTER_RECORD);
    probe.loopJitterUs = jitterUs;
}

LoopIngestModule::Providers validIngestProviders(Probe& probe) {
    LoopIngestModule::Providers providers;
    providers.timestampUs = timestampUs;
    providers.timestampContext = &probe;
    providers.runBleProcess = runBleProcess;
    providers.bleProcessContext = &probe;
    providers.recordBleProcessUs = recordBleProcessUs;
    providers.bleProcessPerfContext = &probe;
    providers.runBleDrain = runBleDrain;
    providers.bleDrainContext = &probe;
    providers.recordBleDrainUs = recordBleDrainUs;
    providers.bleDrainPerfContext = &probe;
    providers.readBleBackpressure = readBleBackpressure;
    providers.bleBackpressureContext = &probe;
    return providers;
}

LoopTailModule::Providers validTailProviders(Probe& probe) {
    LoopTailModule::Providers providers;
    providers.perfTimestampUs = timestampUs;
    providers.perfTimestampContext = &probe;
    providers.loopMicrosUs = loopMicrosUs;
    providers.loopMicrosContext = &probe;
    providers.runBleDrain = runBleDrain;
    providers.bleDrainContext = &probe;
    providers.recordBleDrainUs = recordBleDrainUs;
    providers.bleDrainRecordContext = &probe;
    providers.recordLoopJitterUs = recordLoopJitterUs;
    providers.loopJitterContext = &probe;
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

void test_ingest_begin_rejects_metrics_without_a_clock() {
    Probe probe;
    LoopIngestModule module;
    auto providers = validIngestProviders(probe);
    providers.timestampUs = nullptr;

    TEST_ASSERT_FALSE(module.begin(providers));
}

void test_ingest_runs_process_before_drain_and_backpressure_merge() {
    Probe probe;
    LoopIngestModule module;
    TEST_ASSERT_TRUE(module.begin(validIngestProviders(probe)));

    LoopIngestContext context;
    context.bleProcessEnabled = true;
    const LoopIngestResult result = module.process(context);

    const int expected[] = {TIMESTAMP, BLE_PROCESS, TIMESTAMP,        BLE_PROCESS_RECORD, TIMESTAMP,
                            BLE_DRAIN, TIMESTAMP,   BLE_DRAIN_RECORD, BACKPRESSURE};
    TEST_ASSERT_EQUAL_INT_ARRAY(expected, probe.calls, sizeof(expected) / sizeof(expected[0]));
    TEST_ASSERT_EQUAL_UINT32(25, probe.bleProcessUs);
    TEST_ASSERT_EQUAL_UINT32(25, probe.bleDrainUs);
    TEST_ASSERT_TRUE(result.bleBackpressure);
    TEST_ASSERT_TRUE(result.skipLateNonCoreThisLoop);
    TEST_ASSERT_TRUE(result.overloadLateThisLoop);
}

void test_ingest_accepts_omitted_optional_metrics() {
    Probe probe;
    LoopIngestModule module;
    auto providers = validIngestProviders(probe);
    providers.timestampUs = nullptr;
    providers.recordBleProcessUs = nullptr;
    providers.recordBleDrainUs = nullptr;
    TEST_ASSERT_TRUE(module.begin(providers));

    LoopIngestContext context;
    context.bleProcessEnabled = true;
    module.process(context);

    const int expected[] = {BLE_PROCESS, BLE_DRAIN, BACKPRESSURE};
    TEST_ASSERT_EQUAL_INT_ARRAY(expected, probe.calls, sizeof(expected) / sizeof(expected[0]));
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

void test_tail_begin_rejects_drain_metrics_without_a_clock() {
    Probe probe;
    LoopTailModule module;
    auto providers = validTailProviders(probe);
    providers.perfTimestampUs = nullptr;

    TEST_ASSERT_FALSE(module.begin(providers));
}

void test_tail_drains_before_yield_and_loop_finalization() {
    Probe probe;
    LoopTailModule module;
    TEST_ASSERT_TRUE(module.begin(validTailProviders(probe)));

    const uint32_t durationUs = module.process(true, 50);

    const int expected[] = {TIMESTAMP, BLE_DRAIN, TIMESTAMP, BLE_DRAIN_RECORD, YIELD, LOOP_MICROS, LOOP_JITTER_RECORD};
    TEST_ASSERT_EQUAL_INT_ARRAY(expected, probe.calls, sizeof(expected) / sizeof(expected[0]));
    TEST_ASSERT_EQUAL_UINT32(25, probe.bleDrainUs);
    TEST_ASSERT_EQUAL_UINT32(200, durationUs);
    TEST_ASSERT_EQUAL_UINT32(200, probe.loopJitterUs);
}

void test_tail_accepts_omitted_optional_metrics() {
    Probe probe;
    LoopTailModule module;
    auto providers = validTailProviders(probe);
    providers.perfTimestampUs = nullptr;
    providers.recordBleDrainUs = nullptr;
    providers.recordLoopJitterUs = nullptr;
    TEST_ASSERT_TRUE(module.begin(providers));

    const uint32_t durationUs = module.process(true, 50);

    const int expected[] = {BLE_DRAIN, YIELD, LOOP_MICROS};
    TEST_ASSERT_EQUAL_INT_ARRAY(expected, probe.calls, sizeof(expected) / sizeof(expected[0]));
    TEST_ASSERT_EQUAL_UINT32(200, durationUs);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_ingest_begin_rejects_missing_operational_providers);
    RUN_TEST(test_ingest_begin_rejects_metrics_without_a_clock);
    RUN_TEST(test_ingest_runs_process_before_drain_and_backpressure_merge);
    RUN_TEST(test_ingest_accepts_omitted_optional_metrics);
    RUN_TEST(test_tail_begin_rejects_missing_operational_providers);
    RUN_TEST(test_tail_begin_rejects_drain_metrics_without_a_clock);
    RUN_TEST(test_tail_drains_before_yield_and_loop_finalization);
    RUN_TEST(test_tail_accepts_omitted_optional_metrics);
    return UNITY_END();
}

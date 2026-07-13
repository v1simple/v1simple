#include <unity.h>

#include "../mocks/Arduino.h"
#include "../mocks/mock_heap_caps_state.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../src/perf_metrics.h"
PerfCounters perfCounters;

void perfRecordSdFlushUs(uint32_t) {}
static uint32_t g_perf_session_window_resets = 0;
void perfMetricsResetSessionWindow() { g_perf_session_window_resets++; }

#include "../../src/psram_freertos_alloc.cpp"
#include "../../src/littlefs_mount.cpp"
#include "../../src/storage_manager.cpp"
#include "../../src/perf_sd_logger.cpp"

void setUp() {
    mock_reset_heap_caps();
    mock_reset_queue_create_state();
    mock_reset_task_create_state();
    perfCounters.reset();
    g_perf_session_window_resets = 0;
}

void tearDown() {}

void test_receive_snapshot_times_out_when_queue_empty() {
    PerfSdLogger logger;

    logger.begin(true);

    PerfSdSnapshot snapshot{};
    TEST_ASSERT_FALSE(logger.receiveSnapshotForTest(snapshot, pdMS_TO_TICKS(1000)));
}

void test_receive_snapshot_dequeues_enqueued_item() {
    PerfSdLogger logger;
    logger.begin(true);

    PerfSdSnapshot expected{};
    expected.millisTs = 1234;
    expected.rx = 77;
    expected.queueHighWater = 5;
    expected.displayUpdates = 9;
    expected.pushNowFailures = 2;
    expected.wifiPriorityMode = 1;
    TEST_ASSERT_TRUE(logger.enqueue(expected));

    PerfSdSnapshot actual{};
    TEST_ASSERT_TRUE(logger.receiveSnapshotForTest(actual, pdMS_TO_TICKS(1000)));
    TEST_ASSERT_EQUAL_UINT32(expected.millisTs, actual.millisTs);
    TEST_ASSERT_EQUAL_UINT32(expected.rx, actual.rx);
    TEST_ASSERT_EQUAL_UINT32(expected.queueHighWater, actual.queueHighWater);
    TEST_ASSERT_EQUAL_UINT32(expected.displayUpdates, actual.displayUpdates);
    TEST_ASSERT_EQUAL_UINT32(expected.pushNowFailures, actual.pushNowFailures);
    TEST_ASSERT_EQUAL_UINT8(expected.wifiPriorityMode, actual.wifiPriorityMode);
}

void test_begin_allocates_csv_psram_buffer_and_internal_staging() {
    PerfSdLogger logger;

    logger.begin(true);

    TEST_ASSERT_TRUE(logger.isEnabled());
    TEST_ASSERT_TRUE(logger.csvLineBufferAllocatedForTest());
    TEST_ASSERT_TRUE(logger.writeStagingBufferAllocatedForTest());
    TEST_ASSERT_EQUAL_UINT32(3, g_mock_heap_caps_malloc_calls);
    TEST_ASSERT_EQUAL_UINT32(1, g_mock_queue_create_state.staticCalls);
    TEST_ASSERT_EQUAL_UINT32(0, g_mock_queue_create_state.dynamicCalls);
    TEST_ASSERT_EQUAL_UINT32(1, g_mock_task_create_state.capsCalls);
}

void test_begin_disables_logging_when_csv_psram_buffer_fails() {
    g_mock_heap_caps_fail_on_call = 1;
    PerfSdLogger logger;

    logger.begin(true);

    TEST_ASSERT_FALSE(logger.isEnabled());
    TEST_ASSERT_FALSE(logger.csvLineBufferAllocatedForTest());
    TEST_ASSERT_FALSE(logger.writeStagingBufferAllocatedForTest());
    TEST_ASSERT_EQUAL_UINT32(6144, g_mock_heap_caps_last_malloc_size);
    TEST_ASSERT_EQUAL_UINT32(MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM,
                             g_mock_heap_caps_last_malloc_caps);
    TEST_ASSERT_EQUAL_UINT32(0, g_mock_queue_create_state.staticCalls);
    TEST_ASSERT_EQUAL_UINT32(0, g_mock_task_create_state.capsCalls);
}

void test_begin_disables_logging_when_internal_staging_buffer_fails() {
    g_mock_heap_caps_fail_on_call = 2;
    PerfSdLogger logger;

    logger.begin(true);

    TEST_ASSERT_FALSE(logger.isEnabled());
    TEST_ASSERT_FALSE(logger.csvLineBufferAllocatedForTest());
    TEST_ASSERT_FALSE(logger.writeStagingBufferAllocatedForTest());
    TEST_ASSERT_EQUAL_UINT32(512, g_mock_heap_caps_last_malloc_size);
    TEST_ASSERT_EQUAL_UINT32(MALLOC_CAP_DMA | MALLOC_CAP_8BIT,
                             g_mock_heap_caps_last_malloc_caps);
    TEST_ASSERT_EQUAL_UINT32(1, g_mock_heap_caps_free_calls);
    TEST_ASSERT_EQUAL_UINT32(0, g_mock_queue_create_state.staticCalls);
    TEST_ASSERT_EQUAL_UINT32(0, g_mock_task_create_state.capsCalls);
}

void test_start_new_session_resets_perf_window_when_enabled() {
    PerfSdLogger logger;
    logger.begin(true);

    logger.startNewSession();

    TEST_ASSERT_EQUAL_UINT32(1, g_perf_session_window_resets);
}

void test_start_new_session_skips_perf_window_reset_when_disabled() {
    PerfSdLogger logger;

    logger.startNewSession();

    TEST_ASSERT_EQUAL_UINT32(0, g_perf_session_window_resets);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_receive_snapshot_times_out_when_queue_empty);
    RUN_TEST(test_receive_snapshot_dequeues_enqueued_item);
    RUN_TEST(test_begin_allocates_csv_psram_buffer_and_internal_staging);
    RUN_TEST(test_begin_disables_logging_when_csv_psram_buffer_fails);
    RUN_TEST(test_begin_disables_logging_when_internal_staging_buffer_fails);
    RUN_TEST(test_start_new_session_resets_perf_window_when_enabled);
    RUN_TEST(test_start_new_session_skips_perf_window_reset_when_disabled);
    return UNITY_END();
}

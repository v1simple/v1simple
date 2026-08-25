#include <unity.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "../mocks/storage_manager.h"
#include "../../src/modules/event_log/product_event_csv.cpp"
#include "../../src/modules/event_log/product_event_log.cpp"

unsigned long mockMillis = 0;
unsigned long mockMicros = 0;

namespace {

const std::filesystem::path root = std::filesystem::temp_directory_path() / "v1simple_product_event_log_test";

StorageManager makeStorage(fs::FS& filesystem) {
    StorageManager storage;
    storage.reset();
    storage.setFilesystem(&filesystem, true);
    return storage;
}

ProductEvent endEvent(uint32_t ms) {
    ProductEvent event{};
    event.ms = ms;
    event.source = ProductEventSource::V1;
    event.kind = ProductEventKind::END;
    event.id = 1;
    event.sequence = 2;
    return event;
}

ProductEvent multiRowEvent(uint32_t ms) {
    ProductEvent event{};
    event.ms = ms;
    event.source = ProductEventSource::V1;
    event.kind = ProductEventKind::BEGIN;
    event.id = 1;
    event.sequence = 1;
    event.data.v1.count = 2;
    event.data.v1.alerts[0] = ProductV1Alert{34700, 2, 1, 8, 0, 1};
    event.data.v1.alerts[1] = ProductV1Alert{24150, 1, 2, 0, 4, 0};
    return event;
}

size_t serializedBytes(const ProductEvent& event) {
    size_t bytes = 0;
    char row[256];
    for (size_t item = 0; item < productEventRowCount(event); ++item) {
        bytes += serializeProductEventRow(event, item, row, sizeof(row));
    }
    return bytes;
}

void writeSizedPriorFile(size_t size) {
    const std::filesystem::path events = root / "events";
    std::filesystem::create_directories(events);
    std::ofstream output(events / "events_1.csv", std::ios::binary);
    if (size > 0) {
        output.seekp(static_cast<std::streamoff>(size - 1));
        output.put('x');
    }
}

} // namespace

void setUp() {
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root, error);
    HealthCounters::reset();
    mock_reset_queue_create_state();
    mock_reset_task_create_state();
    StorageManager::resetMockSdLockState();
    fs::mock_reset_fs_write_budget();
}
void tearDown() {}

void test_queue_is_single_static_bounded_and_full_coalesces_gap() {
    fs::FS filesystem(root);
    StorageManager storage = makeStorage(filesystem);
    ProductEventLog log;
    TEST_ASSERT_TRUE(log.begin(10, storage));
    TEST_ASSERT_EQUAL_UINT32(1, g_mock_queue_create_state.staticCalls);
    TEST_ASSERT_EQUAL_UINT32(0, g_mock_queue_create_state.dynamicCalls);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(2048, ProductEventLog::kQueueStorageBytes);

    for (size_t i = 0; i < ProductEventLog::kQueueCapacity; ++i) {
        ProductEvent event = endEvent(static_cast<uint32_t>(100 + i));
        TEST_ASSERT_TRUE(log.enqueueForTest(event));
    }
    TEST_ASSERT_FALSE(log.enqueueForTest(endEvent(999)));
    TEST_ASSERT_EQUAL_UINT32(1, HealthCounters::eventDrops());
    ProductEvent gap{};
    TEST_ASSERT_TRUE(log.takeGapForTest(gap));
    TEST_ASSERT_EQUAL_UINT32(1, gap.data.gap.lost);
    TEST_ASSERT_EQUAL_UINT32(999, gap.data.gap.firstMs);
    TEST_ASSERT_EQUAL_UINT32(999, gap.data.gap.lastMs);
    log.resetForTest();
}

void test_event_file_is_lazy_and_uses_exact_combined_schema() {
    fs::FS filesystem(root);
    StorageManager storage = makeStorage(filesystem);
    ProductEventLog log;
    TEST_ASSERT_TRUE(log.begin(11, storage));
    TEST_ASSERT_FALSE(std::filesystem::exists(root / "events" / "events_11.csv"));
    TEST_ASSERT_TRUE(log.enqueueForTest(endEvent(500)));
    TEST_ASSERT_TRUE(log.drainOneForTest());

    std::ifstream input(root / "events" / "events_11.csv", std::ios::binary);
    const std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    TEST_ASSERT_EQUAL_STRING("# product_event_schema=1\n"
                             "ms,source,event,id,sequence,item,count,payload\n"
                             "500,V1,END,1,2,0,1,state=EMPTY\n",
                             contents.c_str());
    log.resetForTest();
}

void test_first_write_failure_disables_writer_without_retry() {
    fs::FS filesystem(root);
    StorageManager storage = makeStorage(filesystem);
    ProductEventLog log;
    TEST_ASSERT_TRUE(log.begin(12, storage));
    fs::mock_set_fs_write_budget(0);
    TEST_ASSERT_TRUE(log.enqueueForTest(endEvent(600)));
    TEST_ASSERT_FALSE(log.drainOneForTest());
    TEST_ASSERT_FALSE(log.enabled());
    TEST_ASSERT_FALSE(log.accepting());
    TEST_ASSERT_EQUAL_UINT32(1, HealthCounters::eventDrops());
    TEST_ASSERT_FALSE(log.enqueueForTest(endEvent(700)));
    TEST_ASSERT_EQUAL_UINT32(2, HealthCounters::eventDrops());
    log.resetForTest();
}

void test_stop_drains_queue_and_confirms_exit_before_storage_handoff() {
    fs::FS filesystem(root);
    StorageManager storage = makeStorage(filesystem);
    ProductEventLog log;
    TEST_ASSERT_TRUE(log.begin(30, storage));
    TEST_ASSERT_TRUE(log.enqueueForTest(endEvent(100)));
    TEST_ASSERT_TRUE(log.enqueueForTest(endEvent(200)));

    TEST_ASSERT_FALSE(log.stopAndFlush(300, 0));
    TEST_ASSERT_FALSE(log.writerStopped());
    TEST_ASSERT_EQUAL_UINT32(1, HealthCounters::eventShutdownFailures());

    log.runWriterForTest();
    TEST_ASSERT_TRUE(log.writerStopped());
    TEST_ASSERT_TRUE(log.stopAndFlush(301, 0));

    StorageManager::SDLockTimed subsequentStorage(storage.getSDMutex(), 1);
    TEST_ASSERT_TRUE(subsequentStorage);
    const std::string contents = [&]() {
        std::ifstream input(root / "events" / "events_30.csv", std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }();
    TEST_ASSERT_NOT_EQUAL(std::string::npos, contents.find("100,V1,END"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, contents.find("200,V1,END"));
    log.resetForTest();
}

void test_shutdown_close_failure_is_reported_and_storage_handoff_stays_blocked() {
    fs::FS filesystem(root);
    StorageManager storage = makeStorage(filesystem);
    ProductEventLog log;
    TEST_ASSERT_TRUE(log.begin(31, storage));
    TEST_ASSERT_TRUE(log.enqueueForTest(endEvent(100)));
    TEST_ASSERT_TRUE(log.drainOneForTest());

    TEST_ASSERT_FALSE(log.stopAndFlush(200, 0));
    StorageManager::mockSdLockState.failNextBlockingLock = true;
    log.runWriterForTest();
    TEST_ASSERT_TRUE(log.writerStopped());
    TEST_ASSERT_FALSE(log.stopAndFlush(201, 0));
    TEST_ASSERT_EQUAL_UINT32(1, HealthCounters::eventShutdownFailures());
    log.resetForTest();
}

void test_aborted_shutdown_restarts_once_and_accepts_new_event() {
    fs::FS filesystem(root);
    StorageManager storage = makeStorage(filesystem);
    ProductEventLog log;
    TEST_ASSERT_TRUE(log.begin(32, storage));
    TEST_ASSERT_TRUE(log.enqueueForTest(endEvent(100)));
    TEST_ASSERT_FALSE(log.stopAndFlush(200, 0));
    log.runWriterForTest();
    TEST_ASSERT_TRUE(log.stopAndFlush(201, 0));

    TEST_ASSERT_TRUE(log.resumeAfterAbortedShutdown(0));
    TEST_ASSERT_TRUE(log.accepting());
    TEST_ASSERT_EQUAL_UINT32(2, g_mock_task_create_state.standardCalls);
    TEST_ASSERT_TRUE(log.resumeAfterAbortedShutdown(0));
    TEST_ASSERT_EQUAL_UINT32(2, g_mock_task_create_state.standardCalls);

    TEST_ASSERT_TRUE(log.enqueueForTest(endEvent(300)));
    TEST_ASSERT_FALSE(log.stopAndFlush(400, 0));
    log.runWriterForTest();
    TEST_ASSERT_TRUE(log.stopAndFlush(401, 0));

    std::ifstream input(root / "events" / "events_32.csv", std::ios::binary);
    const std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    TEST_ASSERT_NOT_EQUAL(std::string::npos, contents.find("100,V1,END"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, contents.find("300,V1,END"));
    log.resetForTest();
}

void test_resume_cancels_live_stop_without_duplicate_task() {
    fs::FS filesystem(root);
    StorageManager storage = makeStorage(filesystem);
    ProductEventLog log;
    TEST_ASSERT_TRUE(log.begin(33, storage));
    TEST_ASSERT_FALSE(log.stopAndFlush(100, 0));
    TEST_ASSERT_TRUE(log.resumeAfterAbortedShutdown(0));
    TEST_ASSERT_TRUE(log.accepting());
    TEST_ASSERT_EQUAL_UINT32(1, g_mock_task_create_state.standardCalls);
    TEST_ASSERT_TRUE(log.resumeAfterAbortedShutdown(0));
    TEST_ASSERT_EQUAL_UINT32(1, g_mock_task_create_state.standardCalls);
    log.resetForTest();
}

void runRetentionBoundary(int32_t finalDelta, bool expectWrite) {
    const ProductEvent event = multiRowEvent(500);
    const size_t headerBytes = sizeof(kProductEventSchemaHeader) - 1;
    const size_t eventBytes = serializedBytes(event);
    const int64_t priorBytes = static_cast<int64_t>(ProductEventLog::kMaxTotalBytes - headerBytes - eventBytes) +
                               static_cast<int64_t>(finalDelta);
    TEST_ASSERT_GREATER_OR_EQUAL_INT64(0, priorBytes);
    writeSizedPriorFile(static_cast<size_t>(priorBytes));

    fs::FS filesystem(root);
    StorageManager storage = makeStorage(filesystem);
    ProductEventLog log;
    TEST_ASSERT_TRUE(log.begin(40, storage));
    TEST_ASSERT_TRUE(log.enqueueForTest(event));
    TEST_ASSERT_EQUAL(expectWrite, log.drainOneForTest());

    const std::filesystem::path active = root / "events" / "events_40.csv";
    if (expectWrite) {
        TEST_ASSERT_TRUE(log.accepting());
        TEST_ASSERT_EQUAL_UINT32(0, HealthCounters::eventRetentionExhaustions());
    } else {
        TEST_ASSERT_FALSE(log.accepting());
        TEST_ASSERT_FALSE(log.enabled());
        TEST_ASSERT_EQUAL_UINT32(1, HealthCounters::eventRetentionExhaustions());
        TEST_ASSERT_EQUAL_UINT32(1, HealthCounters::eventDrops());
    }
    log.resetForTest();
    TEST_ASSERT_EQUAL_UINT64(expectWrite ? headerBytes + eventBytes : headerBytes,
                             std::filesystem::file_size(active));
}

void test_runtime_retention_multirow_immediately_below_limit() { runRetentionBoundary(-1, true); }

void test_runtime_retention_multirow_exactly_at_limit() { runRetentionBoundary(0, true); }

void test_runtime_retention_multirow_above_limit_preserves_rows() { runRetentionBoundary(1, false); }

void test_boot_retention_reserves_one_slot_for_the_lazy_active_file() {
    const std::filesystem::path events = root / "events";
    std::filesystem::create_directories(events);
    for (uint32_t bootId = 1; bootId <= ProductEventLog::kMaxFiles; ++bootId) {
        std::ofstream(events / ("events_" + std::to_string(bootId) + ".csv")) << "old\n";
    }

    fs::FS filesystem(root);
    StorageManager storage = makeStorage(filesystem);
    ProductEventLog log;
    TEST_ASSERT_TRUE(log.begin(21, storage));
    TEST_ASSERT_FALSE(std::filesystem::exists(events / "events_1.csv"));
    TEST_ASSERT_EQUAL_UINT32(ProductEventLog::kMaxFiles - 1,
                             static_cast<uint32_t>(std::distance(std::filesystem::directory_iterator(events),
                                                                 std::filesystem::directory_iterator{})));

    TEST_ASSERT_TRUE(log.enqueueForTest(endEvent(800)));
    TEST_ASSERT_TRUE(log.drainOneForTest());
    TEST_ASSERT_TRUE(std::filesystem::exists(events / "events_21.csv"));
    TEST_ASSERT_EQUAL_UINT32(ProductEventLog::kMaxFiles,
                             static_cast<uint32_t>(std::distance(std::filesystem::directory_iterator(events),
                                                                 std::filesystem::directory_iterator{})));
    log.resetForTest();
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_queue_is_single_static_bounded_and_full_coalesces_gap);
    RUN_TEST(test_event_file_is_lazy_and_uses_exact_combined_schema);
    RUN_TEST(test_first_write_failure_disables_writer_without_retry);
    RUN_TEST(test_boot_retention_reserves_one_slot_for_the_lazy_active_file);
    RUN_TEST(test_stop_drains_queue_and_confirms_exit_before_storage_handoff);
    RUN_TEST(test_shutdown_close_failure_is_reported_and_storage_handoff_stays_blocked);
    RUN_TEST(test_aborted_shutdown_restarts_once_and_accepts_new_event);
    RUN_TEST(test_resume_cancels_live_stop_without_duplicate_task);
    RUN_TEST(test_runtime_retention_multirow_immediately_below_limit);
    RUN_TEST(test_runtime_retention_multirow_exactly_at_limit);
    RUN_TEST(test_runtime_retention_multirow_above_limit_preserves_rows);
    return UNITY_END();
}

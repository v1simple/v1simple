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
    TEST_ASSERT_EQUAL_UINT32(1, HealthCounters::eventDrops());
    TEST_ASSERT_FALSE(log.enqueueForTest(endEvent(700)));
    TEST_ASSERT_EQUAL_UINT32(2, HealthCounters::eventDrops());
    log.resetForTest();
}

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
    return UNITY_END();
}

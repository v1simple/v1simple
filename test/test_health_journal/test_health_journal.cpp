#include <unity.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "../mocks/storage_manager.h"
#include "../../src/modules/health/health_journal.cpp"

namespace {

const std::filesystem::path root = std::filesystem::temp_directory_path() / "v1simple_health_journal_test";

std::string readFile(const char* name) {
    std::ifstream input(root / name, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

} // namespace

void setUp() {
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root, error);
    StorageManager::resetMockSdLockState();
    HealthCounters::reset();
}
void tearDown() {}

void test_boot_ready_end_schema_and_drop_aggregates() {
    fs::FS filesystem(root);
    StorageManager storage;
    storage.reset();
    storage.setFilesystem(&filesystem, true);
    HealthJournal journal;

    TEST_ASSERT_TRUE(journal.begin(storage, 42, "image-abc", "POWERON", true, false));
    journal.ready(1234);
    HealthCounters::recordInputDrop(2);
    HealthCounters::recordEventDrop(3);
    journal.end(5678);

    TEST_ASSERT_EQUAL_STRING("# health_schema=1\n"
                             "BOOT,boot=42,image=image-abc,reset=POWERON,previous=CLEAN,panic=NONE\n"
                             "READY,boot=42,ms=1234\n"
                             "END,boot=42,ms=5678,result=CLEAN,input_drop=2,event_drop=3,"
                             "event_shutdown_fail=0,event_retention_full=0\n",
                             readFile("health.log").c_str());
}

void test_boot_rotation_is_bounded_to_one_previous_file() {
    std::ofstream existing(root / "health.log", std::ios::binary);
    std::string full(HealthJournal::kMaxBytes, 'x');
    existing.write(full.data(), static_cast<std::streamsize>(full.size()));
    existing.close();

    fs::FS filesystem(root);
    StorageManager storage;
    storage.reset();
    storage.setFilesystem(&filesystem, true);
    HealthJournal journal;
    TEST_ASSERT_TRUE(journal.begin(storage, 7, "img", "SW", false, true));
    TEST_ASSERT_TRUE(std::filesystem::exists(root / "health.prev.log"));
    TEST_ASSERT_TRUE(readFile("health.log").rfind("# health_schema=1\nBOOT,", 0) == 0);
}

void test_lock_failure_disables_journal_for_boot_without_retry() {
    fs::FS filesystem(root);
    StorageManager storage;
    storage.reset();
    storage.setFilesystem(&filesystem, true);
    StorageManager::mockSdLockState.failNextBlockingLock = true;
    HealthJournal journal;
    TEST_ASSERT_FALSE(journal.begin(storage, 1, "img", "POWERON", false, false));
    TEST_ASSERT_FALSE(journal.enabled());
    TEST_ASSERT_EQUAL_UINT32(1, StorageManager::mockSdLockState.blockingAcquireCalls);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_boot_ready_end_schema_and_drop_aggregates);
    RUN_TEST(test_boot_rotation_is_bounded_to_one_previous_file);
    RUN_TEST(test_lock_failure_disables_journal_for_boot_without_retry);
    return UNITY_END();
}

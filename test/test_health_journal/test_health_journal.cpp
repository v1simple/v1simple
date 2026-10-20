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
    fs::mock_reset_fs_write_budget();
    fs::mock_reset_fs_open_state();
    fs::mock_reset_fs_remove_state();
    fs::mock_reset_fs_rename_state();
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

void test_aborted_shutdown_records_resume_and_later_final_counters_in_same_boot() {
    fs::FS filesystem(root);
    StorageManager storage;
    storage.reset();
    storage.setFilesystem(&filesystem, true);
    HealthJournal journal;
    TEST_ASSERT_TRUE(journal.begin(storage, 42, "img", "POWERON", false, false));
    journal.ready(1000);
    journal.resumeAfterAbortedShutdown(1500); // No preceding END to cancel.
    journal.end(2000);
    journal.end(2100); // END remains idempotent until an actual resume.
    journal.resumeAfterAbortedShutdown(3000);
    journal.resumeAfterAbortedShutdown(3100);
    HealthCounters::recordInputDrop(7);
    HealthCounters::recordEventDrop(3);
    journal.ready(4000); // Resume must not invent a new boot/ready interval.
    journal.end(9000);
    journal.end(9100);

    TEST_ASSERT_EQUAL_STRING("# health_schema=1\n"
                             "BOOT,boot=42,image=img,reset=POWERON,previous=UNCLEAN,panic=NONE\n"
                             "READY,boot=42,ms=1000\n"
                             "END,boot=42,ms=2000,result=CLEAN,input_drop=0,event_drop=0,"
                             "event_shutdown_fail=0,event_retention_full=0\n"
                             "RESUME,boot=42,ms=3000\n"
                             "END,boot=42,ms=9000,result=CLEAN,input_drop=7,event_drop=3,"
                             "event_shutdown_fail=0,event_retention_full=0\n",
                             readFile("health.log").c_str());
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
void test_short_writes_are_quarantined_before_next_boot_append() {
    for (int stage = 0; stage < 5; ++stage) {
        setUp();
        if (stage == 1) {
            std::ofstream(root / "health.log", std::ios::binary) << "# health_schema=1\n";
        }
        fs::FS filesystem(root);
        StorageManager storage;
        storage.reset();
        storage.setFilesystem(&filesystem, true);
        HealthJournal journal;
        if (stage < 2) {
            fs::mock_set_fs_write_budget(1);
            TEST_ASSERT_FALSE(journal.begin(storage, 1, "img", "SW", false, false));
        } else {
            TEST_ASSERT_TRUE(journal.begin(storage, 1, "img", "SW", false, false));
            if (stage == 4) journal.end(5);
            fs::mock_set_fs_write_budget(1);
            if (stage == 2) journal.ready(10);
            else if (stage == 3) journal.end(10);
            else journal.resumeAfterAbortedShutdown(10);
            TEST_ASSERT_FALSE(journal.enabled());
        }
        const std::string damaged = readFile("health.log");
        fs::mock_reset_fs_write_budget();
        HealthJournal next;
        TEST_ASSERT_TRUE(next.begin(storage, 2, "img", "SW", false, false));
        TEST_ASSERT_EQUAL_STRING(damaged.c_str(), readFile("health.prev.log").c_str());
        TEST_ASSERT_TRUE(readFile("health.log").rfind("# health_schema=1\nBOOT,boot=2,", 0) == 0);
    }
}

void test_read_failure_preserves_existing_log_until_healthy_boot() {
    for (int kind = 0; kind < 3; ++kind) {
        setUp();
        const std::string prior = kind == 0 ? "END,boot=1,result=CL" :
                                  kind == 1 ? std::string(HealthJournal::kMaxBytes - 1, 'x') + "\n" :
                                              "# health_schema=1\nBOOT,boot=1\n";
        std::ofstream(root / "health.log", std::ios::binary) << prior;
        fs::FS filesystem(root);
        StorageManager storage;
        storage.reset();
        storage.setFilesystem(&filesystem, true);
        fs::mock_exists_uses_read_open(true);
        fs::mock_fail_next_read_open("/health.log");
        HealthJournal failed;
        TEST_ASSERT_FALSE(failed.begin(storage, 2, "img", "SW", false, false));
        failed.ready(10);
        failed.end(20);
        failed.resumeAfterAbortedShutdown(30);
        TEST_ASSERT_EQUAL_STRING(prior.c_str(), readFile("health.log").c_str());
        TEST_ASSERT_FALSE(std::filesystem::exists(root / "health.prev.log"));

        // A later boot can read and classify the exact bytes the failed boot
        // left untouched, quarantining torn/full logs and appending healthy ones.
        HealthJournal next;
        TEST_ASSERT_TRUE(next.begin(storage, 3, "img", "SW", false, false));
        next.ready(40);
        next.end(50);
        if (kind < 2) {
            TEST_ASSERT_EQUAL_STRING(prior.c_str(), readFile("health.prev.log").c_str());
            TEST_ASSERT_TRUE(readFile("health.log").rfind("# health_schema=1\nBOOT,boot=3,", 0) == 0);
            TEST_ASSERT_LESS_THAN(HealthJournal::kMaxBytes, readFile("health.log").size());
        } else {
            TEST_ASSERT_FALSE(std::filesystem::exists(root / "health.prev.log"));
            TEST_ASSERT_TRUE(readFile("health.log").rfind(prior + "BOOT,boot=3,", 0) == 0);
        }
    }
}

void test_persistent_read_failure_and_metadata_error_do_not_admit_append() {
    fs::FS filesystem(root);
    StorageManager storage;
    storage.reset();
    storage.setFilesystem(&filesystem, true);
    const std::string prior = "END,boot=1,result=CL";
    std::ofstream(root / "health.log", std::ios::binary) << prior;
    fs::mock_fail_read_opens_for_full_path((root / "health.log").c_str());
    HealthJournal journal;
    TEST_ASSERT_FALSE(journal.begin(storage, 2, "img", "SW", false, false));
    TEST_ASSERT_FALSE(journal.begin(storage, 3, "img", "SW", false, false));
    TEST_ASSERT_EQUAL_STRING(prior.c_str(), readFile("health.log").c_str());
    fs::mock_reset_fs_open_state();

    // A non-directory mount path produces ENOTDIR, not proven absence.
    std::filesystem::remove_all(root);
    std::ofstream(root, std::ios::binary) << "unavailable mount";
    TEST_ASSERT_FALSE(journal.begin(storage, 4, "img", "SW", false, false));
    TEST_ASSERT_FALSE(journal.enabled());
}

void test_failed_previous_removal_or_rotation_preserves_logs_and_retries() {
    for (bool failRemoval : {true, false}) {
        setUp();
        const std::string prior = "END,boot=1,result=CL";
        std::ofstream(root / "health.log", std::ios::binary) << prior;
        std::ofstream(root / "health.prev.log", std::ios::binary) << "older\n";
        fs::FS filesystem(root);
        StorageManager storage;
        storage.reset();
        storage.setFilesystem(&filesystem, true);
        if (failRemoval) fs::mock_fail_next_remove("/health.prev.log");
        else fs::mock_fail_rename_on_call(1);
        HealthJournal failed;
        TEST_ASSERT_FALSE(failed.begin(storage, 2, "img", "SW", false, false));
        TEST_ASSERT_EQUAL_STRING(prior.c_str(), readFile("health.log").c_str());
        if (failRemoval) TEST_ASSERT_EQUAL_STRING("older\n", readFile("health.prev.log").c_str());
        HealthJournal next;
        TEST_ASSERT_TRUE(next.begin(storage, 3, "img", "SW", false, false));
        TEST_ASSERT_EQUAL_STRING(prior.c_str(), readFile("health.prev.log").c_str());
        TEST_ASSERT_TRUE(readFile("health.log").rfind("# health_schema=1\nBOOT,boot=3,", 0) == 0);
    }
}

void test_failed_cached_file_size_does_not_bypass_tail_or_size_validation() {
    for (bool full : {false, true}) {
        setUp();
        const std::string prior = full ? std::string(HealthJournal::kMaxBytes - 1, 'x') + "\n"
                                       : "END,boot=1,result=CL";
        std::ofstream(root / "health.log", std::ios::binary) << prior;
        fs::FS filesystem(root);
        StorageManager storage;
        storage.reset();
        storage.setFilesystem(&filesystem, true);
        fs::mock_zero_file_size_for_full_path((root / "health.log").c_str());
        HealthJournal journal;
        TEST_ASSERT_TRUE(journal.begin(storage, 2, "img", "SW", false, false));
        TEST_ASSERT_EQUAL_STRING(prior.c_str(), readFile("health.prev.log").c_str());
        TEST_ASSERT_TRUE(readFile("health.log").rfind("# health_schema=1\nBOOT,boot=2,", 0) == 0);
        journal.ready(10);
        journal.end(20);
        const std::string healthy = readFile("health.log");
        HealthJournal next;
        TEST_ASSERT_TRUE(next.begin(storage, 3, "img", "SW", true, false));
        TEST_ASSERT_TRUE(readFile("health.log").rfind(healthy + "BOOT,boot=3,", 0) == 0);
    }
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_boot_ready_end_schema_and_drop_aggregates);
    RUN_TEST(test_aborted_shutdown_records_resume_and_later_final_counters_in_same_boot);
    RUN_TEST(test_boot_rotation_is_bounded_to_one_previous_file);
    RUN_TEST(test_lock_failure_disables_journal_for_boot_without_retry);
    RUN_TEST(test_short_writes_are_quarantined_before_next_boot_append);
    RUN_TEST(test_read_failure_preserves_existing_log_until_healthy_boot);
    RUN_TEST(test_persistent_read_failure_and_metadata_error_do_not_admit_append);
    RUN_TEST(test_failed_previous_removal_or_rotation_preserves_logs_and_retries);
    RUN_TEST(test_failed_cached_file_size_does_not_bypass_tail_or_size_validation);
    return UNITY_END();
}

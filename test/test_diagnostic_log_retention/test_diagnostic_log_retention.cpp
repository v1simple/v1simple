#include <unity.h>

#include "../../src/diagnostic_log_retention.cpp"

#include <filesystem>

namespace {

std::filesystem::path testRoot;
fs::FS* filesystem = nullptr;

void writeFile(const char* path) {
    File file = filesystem->open(path, FILE_WRITE);
    TEST_ASSERT_TRUE(file);
    TEST_ASSERT_EQUAL_UINT32(1, file.print("x"));
    file.close();
}

void addManagedFiles(const char* directory, const char* prefix, uint32_t firstBootId, uint32_t lastBootId) {
    filesystem->mkdir(directory);
    for (uint32_t bootId = firstBootId; bootId <= lastBootId; ++bootId) {
        char path[96];
        std::snprintf(path, sizeof(path), "%s/%s%lu-%08lx.csv", directory, prefix, static_cast<unsigned long>(bootId),
                      static_cast<unsigned long>(bootId));
        writeFile(path);
    }
}

} // namespace

void setUp() {
    testRoot = std::filesystem::temp_directory_path() / "v1simple_diagnostic_log_retention_test";
    std::error_code error;
    std::filesystem::remove_all(testRoot, error);
    filesystem = new fs::FS(testRoot);
}

void tearDown() {
    delete filesystem;
    filesystem = nullptr;
    std::error_code error;
    std::filesystem::remove_all(testRoot, error);
}

void test_prunes_oldest_exact_managed_names_only() {
    addManagedFiles("/perf", "perf_boot_", 7, 12);
    writeFile("/perf/perf.csv");
    writeFile("/perf/notes.csv");
    writeFile("/perf/perf_boot_bad.csv");
    filesystem->mkdir("/perf/perf_boot_6-00000006.csv");

    const DiagnosticLogRetention::PruneResult result =
        DiagnosticLogRetention::pruneManagedDirectory(*filesystem, "/perf", "perf_boot_", 12, 3);

    TEST_ASSERT_EQUAL_UINT32(6, result.matchedFiles);
    TEST_ASSERT_EQUAL_UINT32(3, result.removedFiles);
    TEST_ASSERT_EQUAL_UINT32(0, result.failedRemovals);
    TEST_ASSERT_FALSE(filesystem->exists("/perf/perf_boot_7-00000007.csv"));
    TEST_ASSERT_FALSE(filesystem->exists("/perf/perf_boot_8-00000008.csv"));
    TEST_ASSERT_FALSE(filesystem->exists("/perf/perf_boot_9-00000009.csv"));
    TEST_ASSERT_TRUE(filesystem->exists("/perf/perf_boot_10-0000000a.csv"));
    TEST_ASSERT_TRUE(filesystem->exists("/perf/perf_boot_11-0000000b.csv"));
    TEST_ASSERT_TRUE(filesystem->exists("/perf/perf_boot_12-0000000c.csv"));
    TEST_ASSERT_TRUE(filesystem->exists("/perf/perf.csv"));
    TEST_ASSERT_TRUE(filesystem->exists("/perf/notes.csv"));
    TEST_ASSERT_TRUE(filesystem->exists("/perf/perf_boot_bad.csv"));
    TEST_ASSERT_TRUE(filesystem->exists("/perf/perf_boot_6-00000006.csv"));
}

void test_accepts_legacy_and_token_names_but_rejects_near_matches() {
    filesystem->mkdir("/alp");
    writeFile("/alp/alp_1.csv");
    writeFile("/alp/alp_2-aBcD1234.csv");
    writeFile("/alp/alp_3-1234567.csv");
    writeFile("/alp/alp_4-123456789.csv");
    writeFile("/alp/alp_5-1234567g.csv");
    writeFile("/alp/alp_6.csv.bak");
    writeFile("/alp/alp_4294967296.csv");

    const DiagnosticLogRetention::PruneResult result =
        DiagnosticLogRetention::pruneManagedDirectory(*filesystem, "/alp", "alp_", 2, 0);

    TEST_ASSERT_EQUAL_UINT32(2, result.matchedFiles);
    TEST_ASSERT_EQUAL_UINT32(2, result.removedFiles);
    TEST_ASSERT_FALSE(filesystem->exists("/alp/alp_1.csv"));
    TEST_ASSERT_FALSE(filesystem->exists("/alp/alp_2-aBcD1234.csv"));
    TEST_ASSERT_TRUE(filesystem->exists("/alp/alp_3-1234567.csv"));
    TEST_ASSERT_TRUE(filesystem->exists("/alp/alp_4-123456789.csv"));
    TEST_ASSERT_TRUE(filesystem->exists("/alp/alp_5-1234567g.csv"));
    TEST_ASSERT_TRUE(filesystem->exists("/alp/alp_6.csv.bak"));
    TEST_ASSERT_TRUE(filesystem->exists("/alp/alp_4294967296.csv"));
}

void test_boot_age_order_is_safe_across_uint32_rollover() {
    filesystem->mkdir("/encounters");
    writeFile("/encounters/encounters_4294967294.csv");
    writeFile("/encounters/encounters_4294967295.csv");
    writeFile("/encounters/encounters_0.csv");
    writeFile("/encounters/encounters_1.csv");

    const DiagnosticLogRetention::PruneResult result =
        DiagnosticLogRetention::pruneManagedDirectory(*filesystem, "/encounters", "encounters_", 1, 3);

    TEST_ASSERT_EQUAL_UINT32(1, result.removedFiles);
    TEST_ASSERT_FALSE(filesystem->exists("/encounters/encounters_4294967294.csv"));
    TEST_ASSERT_TRUE(filesystem->exists("/encounters/encounters_4294967295.csv"));
    TEST_ASSERT_TRUE(filesystem->exists("/encounters/encounters_0.csv"));
    TEST_ASSERT_TRUE(filesystem->exists("/encounters/encounters_1.csv"));
}

void test_boot_prune_reserves_one_current_file_slot_per_category() {
    addManagedFiles("/perf", "perf_boot_", 1, 21);
    addManagedFiles("/alp", "alp_", 1, 21);
    addManagedFiles("/encounters", "encounters_", 1, 21);

    const DiagnosticLogRetention::BootPruneResult result =
        DiagnosticLogRetention::pruneBeforeCurrentBoot(*filesystem, 22);

    TEST_ASSERT_EQUAL_UINT32(2, result.perf.removedFiles);
    TEST_ASSERT_EQUAL_UINT32(2, result.alp.removedFiles);
    TEST_ASSERT_EQUAL_UINT32(2, result.encounters.removedFiles);
    TEST_ASSERT_EQUAL_UINT32(20, DiagnosticLogLimits::MANAGED_FILES_PER_CATEGORY);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(DiagnosticLogLimits::MAX_LISTED_FILES,
                                     DiagnosticLogLimits::MANAGED_CATEGORY_COUNT *
                                         DiagnosticLogLimits::MANAGED_FILES_PER_CATEGORY);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_prunes_oldest_exact_managed_names_only);
    RUN_TEST(test_accepts_legacy_and_token_names_but_rejects_near_matches);
    RUN_TEST(test_boot_age_order_is_safe_across_uint32_rollover);
    RUN_TEST(test_boot_prune_reserves_one_current_file_slot_per_category);
    return UNITY_END();
}

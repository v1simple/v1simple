/**
 * Perf SD Logger — storage lifecycle contracts.
 *
 * The perf logger's SD path is production-only (native builds use mocks), so
 * these tests pin the source contract textually, following the pattern
 * established for the ALP logger's handle-reuse contract.
 *
 * Contract under test: warm-at-boot. The perf file's first writes in a boot (/perf
 * mkdir, CSV create, header, session marker) carry FAT-allocation cost that
 * was observed at 10-25 ms per write on a worn card. begin() must pay that
 * cost during setup — before BLE connects and before enabled_ flips true — so
 * successful warm-up keeps it off flush boundaries while alerts are in flight.
 */

#include <unity.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {
std::string readProjectFile(const char* relativePath) {
    const std::filesystem::path path = std::filesystem::path(PROJECT_DIR) / relativePath;
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        return {};
    }
    return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
}
} // namespace

void setUp() {}
void tearDown() {}

void test_perf_sd_logger_begin_warms_storage_at_boot() {
    const std::string source = readProjectFile("src/perf_sd_logger.cpp");
    TEST_ASSERT_FALSE(source.empty());

    const size_t beginStart = source.find("void PerfSdLogger::begin(");
    TEST_ASSERT_NOT_EQUAL(std::string::npos, beginStart);
    const size_t enabledTrue = source.find("enabled_ = true;", beginStart);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, enabledTrue);
    const std::string beginBody = source.substr(beginStart, enabledTrue - beginStart);

    // Warm-up runs under the SD lock, opens the persistent handle through the
    // same helper the writer task uses, and writes header + session marker.
    TEST_ASSERT_NOT_EQUAL(std::string::npos, beginBody.find("StorageManager::SDLockBlocking"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, beginBody.find("ensurePersistentFileLocked(*fs)"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, beginBody.find("ensureCsvHeaderAndSessionMarker(persistentFile_)"));
}

void test_perf_sd_logger_writer_and_warmup_share_one_open_path() {
    // The open/retry sequence must exist exactly once, in the shared helper —
    // not duplicated between the writer task and the boot warm-up.
    const std::string source = readProjectFile("src/perf_sd_logger.cpp");
    TEST_ASSERT_FALSE(source.empty());

    const std::string openLine = "persistentFile_ = fs.open(csvPath, FILE_APPEND, true);";
    const size_t first = source.find(openLine);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, first);
    const size_t second = source.find(openLine, first + openLine.size());
    TEST_ASSERT_NOT_EQUAL(std::string::npos, second);
    TEST_ASSERT_EQUAL(std::string::npos, source.find(openLine, second + openLine.size()));

    const size_t helperStart = source.find("bool PerfSdLogger::ensurePersistentFileLocked(fs::FS& fs)");
    TEST_ASSERT_NOT_EQUAL(std::string::npos, helperStart);
    const size_t helperEnd = source.find("\nbool PerfSdLogger::", helperStart + 1);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, helperEnd);
    TEST_ASSERT_TRUE(first > helperStart && second < helperEnd);

    // The writer task path delegates to the helper instead of reopening.
    const size_t appendStart = source.find("bool PerfSdLogger::appendSnapshotLine(");
    TEST_ASSERT_NOT_EQUAL(std::string::npos, appendStart);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, source.find("ensurePersistentFileLocked(*fs)", appendStart));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_perf_sd_logger_begin_warms_storage_at_boot);
    RUN_TEST(test_perf_sd_logger_writer_and_warmup_share_one_open_path);
    return UNITY_END();
}

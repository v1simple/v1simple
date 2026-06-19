#include <unity.h>

#include <fstream>
#include <string>

namespace {

std::string readTextFile(const char* path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        return {};
    }
    return std::string((std::istreambuf_iterator<char>(stream)),
                       std::istreambuf_iterator<char>());
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_perf_sd_snapshot_stays_on_flat_path() {
    const std::string source = readTextFile("src/perf_metrics.cpp");

    TEST_ASSERT_FALSE_MESSAGE(source.empty(), "failed to read src/perf_metrics.cpp");
    TEST_ASSERT_EQUAL(std::string::npos,
                      source.find("PerfRuntimeMetricsSnapshot runtimeSnapshot{};"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          source.find("const RuntimeSnapshotCaptureContext ctx = captureRuntimeSnapshotContext();"));
    TEST_ASSERT_NOT_EQUAL(
        std::string::npos,
        source.find(
            "populateFlatSnapshot(snapshot, ctx, PerfRuntimeSnapshotMode::CaptureAndResetWindowPeaks);"));
}

void test_wifi_file_streaming_uses_small_chunk_buffer() {
    const std::string source = readTextFile("src/wifi_manager_helpers.cpp");

    TEST_ASSERT_FALSE_MESSAGE(source.empty(), "failed to read src/wifi_manager_helpers.cpp");
    TEST_ASSERT_EQUAL(std::string::npos, source.find("uint8_t buf[1024];"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          source.find("constexpr size_t kStreamChunkBytes = 256;"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, source.find("uint8_t buf[kStreamChunkBytes];"));
}

void test_obd_parser_avoids_line_matrix_scratch() {
    const std::string source = readTextFile("src/modules/obd/obd_elm327_parser.cpp");

    TEST_ASSERT_FALSE_MESSAGE(source.empty(), "failed to read src/modules/obd/obd_elm327_parser.cpp");
    TEST_ASSERT_EQUAL(std::string::npos,
                      source.find("char lines[kMaxNormalizedLines][kMaxLineLength]"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          source.find("bool nextNormalizedLine(const char* trimmed,"));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_perf_sd_snapshot_stays_on_flat_path);
    RUN_TEST(test_wifi_file_streaming_uses_small_chunk_buffer);
    RUN_TEST(test_obd_parser_avoids_line_matrix_scratch);
    return UNITY_END();
}

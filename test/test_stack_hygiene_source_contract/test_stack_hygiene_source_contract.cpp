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
    const std::string source = readTextFile("src/perf_snapshot.cpp");

    TEST_ASSERT_FALSE_MESSAGE(source.empty(), "failed to read src/perf_snapshot.cpp");
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

void test_sd_audio_uses_one_persistent_static_worker() {
    const std::string source = readTextFile("src/audio_voice.cpp");

    TEST_ASSERT_FALSE_MESSAGE(source.empty(), "failed to read src/audio_voice.cpp");
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          source.find("if (sdAudioWorkerHandle == nullptr)"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          source.find("xTaskCreateStaticPinnedToCore("));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          source.find("ulTaskNotifyTake(pdTRUE, portMAX_DELAY)"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          source.find("xTaskNotifyGive(sdAudioWorkerHandle)"));
    TEST_ASSERT_EQUAL(std::string::npos, source.find("vTaskDelete(nullptr)"));
}

void test_main_loop_is_explicitly_watched_and_only_feeds_on_exit() {
    const std::string source = readTextFile("src/main.cpp");

    TEST_ASSERT_FALSE_MESSAGE(source.empty(), "failed to read src/main.cpp");
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          source.find("esp_task_wdt_add(nullptr)"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          source.find("class MainLoopWatchdogFeedOnExit"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          source.find("~MainLoopWatchdogFeedOnExit()"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          source.find("MainLoopWatchdogFeedOnExit watchdogFeed;"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          source.find("esp_task_wdt_reset()"));
}

void test_graceful_shutdown_feeds_between_bounded_drain_steps() {
    const std::string source = readTextFile("src/main_setup_helpers.cpp");

    TEST_ASSERT_FALSE_MESSAGE(source.empty(), "failed to read src/main_setup_helpers.cpp");
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          source.find("feedLoopTaskWatchdogDuringShutdown()"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          source.find("shutdownBleBondBackupWriter(1500);"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          source.find("shutdownDeferredSettingsBackupWriter(1500);"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          source.find("(void)esp_task_wdt_reset();"));
}

void test_short_lived_task_stack_watermarks_are_reported() {
    const std::string source = readTextFile("src/perf_report.cpp");

    TEST_ASSERT_FALSE_MESSAGE(source.empty(), "failed to read src/perf_report.cpp");
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          source.find("audio_pcm_stack_high_water_bytes()"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          source.find("audio_sd_stack_high_water_bytes()"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          source.find("bleClient.discoveryTaskStackMinFreeBytes()"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          source.find("bleBondBackupWriterStats().writerStackMinFreeBytes"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          source.find("audioPcmFreeBytes != UINT32_MAX"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          source.find("discoveryFreeBytes != UINT32_MAX"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          source.find("bleBondFreeBytes != UINT32_MAX"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          source.find("stackLocationLabel(bleBondSampled, false)"));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_perf_sd_snapshot_stays_on_flat_path);
    RUN_TEST(test_wifi_file_streaming_uses_small_chunk_buffer);
    RUN_TEST(test_obd_parser_avoids_line_matrix_scratch);
    RUN_TEST(test_sd_audio_uses_one_persistent_static_worker);
    RUN_TEST(test_main_loop_is_explicitly_watched_and_only_feeds_on_exit);
    RUN_TEST(test_graceful_shutdown_feeds_between_bounded_drain_steps);
    RUN_TEST(test_short_lived_task_stack_watermarks_are_reported);
    return UNITY_END();
}

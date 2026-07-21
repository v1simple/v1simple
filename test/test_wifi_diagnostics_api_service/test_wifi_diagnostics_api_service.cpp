#include <unity.h>

#include <filesystem>
#include <string>
#include <vector>

#include "../../src/modules/wifi/wifi_diagnostics_api_service.cpp"

namespace {

const std::filesystem::path TEST_ROOT = std::filesystem::temp_directory_path() / "v1simple_wifi_diagnostics_api_test";
const std::filesystem::path PANIC_ROOT =
    std::filesystem::temp_directory_path() / "v1simple_wifi_diagnostics_panic_test";

fs::FS* testFilesystem = nullptr;
fs::FS* panicFilesystem = nullptr;
int uiActivityCalls = 0;

WifiDiagnosticsApiService::Runtime makeRuntime(bool maintenance = true, bool storageReady = true, bool sdCard = true) {
    WifiDiagnosticsApiService::Runtime runtime;
    runtime.filesystem = testFilesystem;
    runtime.storageReady = storageReady;
    runtime.sdCard = sdCard;
    runtime.maintenanceBootActive = maintenance;
    runtime.markUiActivity = [](void*) { ++uiActivityCalls; };
    return runtime;
}

void writeFile(const char* path, const char* contents) {
    File file = testFilesystem->open(path, FILE_WRITE);
    TEST_ASSERT_TRUE(file);
    TEST_ASSERT_EQUAL_UINT32(std::strlen(contents), file.print(contents));
    file.close();
}

bool responseContains(const WebServer& server, const char* needle) {
    return std::strstr(server.lastBody.c_str(), needle) != nullptr;
}

} // namespace

void setUp() {
    std::error_code error;
    std::filesystem::remove_all(TEST_ROOT, error);
    std::filesystem::remove_all(PANIC_ROOT, error);
    std::filesystem::create_directories(TEST_ROOT, error);
    std::filesystem::create_directories(PANIC_ROOT, error);
    delete testFilesystem;
    delete panicFilesystem;
    testFilesystem = new fs::FS(TEST_ROOT);
    panicFilesystem = new fs::FS(PANIC_ROOT);
    uiActivityCalls = 0;
}

void tearDown() {
    delete testFilesystem;
    testFilesystem = nullptr;
    delete panicFilesystem;
    panicFilesystem = nullptr;
    std::error_code error;
    std::filesystem::remove_all(TEST_ROOT, error);
    std::filesystem::remove_all(PANIC_ROOT, error);
}

void test_path_guard_accepts_only_canonical_diagnostic_files() {
    TEST_ASSERT_TRUE(WifiDiagnosticsApiService::isAllowedLogPath("/poweroff.log"));
    TEST_ASSERT_TRUE(WifiDiagnosticsApiService::isAllowedLogPath("/panic.txt"));
    TEST_ASSERT_TRUE(WifiDiagnosticsApiService::isAllowedLogPath("/perf/perf_boot_1.csv"));
    TEST_ASSERT_TRUE(WifiDiagnosticsApiService::isAllowedLogPath("/alp/alp_1-deadbeef.csv"));

    TEST_ASSERT_FALSE(WifiDiagnosticsApiService::isAllowedLogPath(nullptr));
    TEST_ASSERT_FALSE(WifiDiagnosticsApiService::isAllowedLogPath("poweroff.log"));
    TEST_ASSERT_FALSE(WifiDiagnosticsApiService::isAllowedLogPath("/settings.json"));
    TEST_ASSERT_FALSE(WifiDiagnosticsApiService::isAllowedLogPath("/perf/../settings.json"));
    TEST_ASSERT_FALSE(WifiDiagnosticsApiService::isAllowedLogPath("/perf/subdir/file.csv"));
    TEST_ASSERT_FALSE(WifiDiagnosticsApiService::isAllowedLogPath("/alp/%2e%2e/settings.json"));
    TEST_ASSERT_FALSE(WifiDiagnosticsApiService::isAllowedLogPath("/alp/.hidden"));
    TEST_ASSERT_FALSE(WifiDiagnosticsApiService::isAllowedLogPath("/alp/file name.csv"));
}

void test_list_is_maintenance_only_and_requires_sd_card() {
    WebServer server(80);
    WifiDiagnosticsApiService::handleApiList(server, makeRuntime(false));
    TEST_ASSERT_EQUAL_INT(409, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "maintenance_mode_required"));
    TEST_ASSERT_EQUAL_INT(1, uiActivityCalls);

    WebServer noSdServer(80);
    WifiDiagnosticsApiService::handleApiList(noSdServer, makeRuntime(true, true, false));
    TEST_ASSERT_EQUAL_INT(503, noSdServer.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(noSdServer, "sd_card_unavailable"));
    TEST_ASSERT_EQUAL_INT(2, uiActivityCalls);
}

void test_list_returns_only_allowlisted_files_and_latest_power_evidence() {
    writeFile("/poweroff.log", "[old] stale shutdown\n"
                               "[1] POWER OFF INITIATED\n"
                               "[2] source=external\n"
                               "[3] voltage=5000mV\n"
                               "[4] strategy=deep_sleep_external_power\n"
                               "[5] batteryLatch=LOW\n"
                               "[6] wake=BOOT_GPIO0 trigger=active_low\n"
                               "[7] OUTCOME mode=deep_sleep_external_power batteryLatch=LOW wake=BOOT_GPIO0\n"
                               "[8] BOOT reset=DEEPSLEEP\n");
    writeFile("/panic.txt", "SD file must not impersonate the LittleFS panic breadcrumb");
    writeFile("/perf/perf_boot_1.csv", "a,b\n1,2\n");
    writeFile("/alp/alp_1.csv", "event\nlaser\n");
    writeFile("/settings.json", "secret");
    writeFile("/perf/.hidden", "hidden");
    writeFile("/perf/nested/escape.csv", "nested");

    WebServer server(80);
    WifiDiagnosticsApiService::handleApiList(server, makeRuntime());

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"path\":\"/poweroff.log\""));
    TEST_ASSERT_FALSE(responseContains(server, "\"path\":\"/panic.txt\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"path\":\"/perf/perf_boot_1.csv\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"path\":\"/alp/alp_1.csv\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"lastPoweroffEvidence\":\"[1] POWER OFF INITIATED"));
    TEST_ASSERT_TRUE(responseContains(server, "[7] OUTCOME mode=deep_sleep_external_power"));
    TEST_ASSERT_TRUE(responseContains(server, "[8] BOOT reset=DEEPSLEEP"));
    TEST_ASSERT_FALSE(responseContains(server, "stale shutdown"));
    TEST_ASSERT_FALSE(responseContains(server, "settings.json"));
    TEST_ASSERT_FALSE(responseContains(server, ".hidden"));
    TEST_ASSERT_FALSE(responseContains(server, "escape.csv"));
    TEST_ASSERT_EQUAL_INT(1, uiActivityCalls);
}

void test_download_streams_allowed_file_with_safe_headers() {
    writeFile("/perf/perf_boot_7.csv", "header\nrow\n");
    WebServer server(80);
    server.setArg("path", "/perf/perf_boot_7.csv");
    server.setClientMaxWriteSize(3);

    WifiDiagnosticsApiService::handleApiDownload(server, makeRuntime());

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_STRING("text/csv", server.lastContentType.c_str());
    TEST_ASSERT_EQUAL_STRING("header\nrow\n", server.lastBody.c_str());
    TEST_ASSERT_EQUAL_STRING("attachment; filename=\"perf_boot_7.csv\"",
                             server.sentHeader("Content-Disposition").c_str());
    TEST_ASSERT_EQUAL_STRING("no-store", server.sentHeader("Cache-Control").c_str());
    TEST_ASSERT_EQUAL_UINT32(2, server.contentLengthHistory.size());
    TEST_ASSERT_EQUAL_UINT32(std::strlen("header\nrow\n"), server.contentLengthHistory[0]);
    TEST_ASSERT_EQUAL_UINT32(CONTENT_LENGTH_NOT_SET, server.contentLengthHistory[1]);
    TEST_ASSERT_EQUAL_UINT32(CONTENT_LENGTH_NOT_SET, server.lastContentLength);
    TEST_ASSERT_TRUE(server.clientWriteCalls() > 1);
    TEST_ASSERT_EQUAL_INT(1, uiActivityCalls);
}

void test_interrupted_download_closes_with_an_incomplete_declared_body() {
    writeFile("/poweroff.log", "0123456789");
    WebServer server(80);
    server.setArg("path", "/poweroff.log");
    server.setClientDisconnectAfterBytes(4);

    WifiDiagnosticsApiService::handleApiDownload(server, makeRuntime());

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_STRING("0123", server.lastBody.c_str());
    TEST_ASSERT_EQUAL_UINT32(10, server.contentLengthHistory[0]);
    TEST_ASSERT_EQUAL_UINT32(CONTENT_LENGTH_NOT_SET, server.contentLengthHistory[1]);
    TEST_ASSERT_FALSE(server.client().connected());
}

void test_stream_policy_retries_partial_progress_and_feeds_watchdog() {
    using WifiDiagnosticsStream::AttemptResult;
    using WifiDiagnosticsStream::AttemptState;

    const uint8_t payload[] = {'a', 'b', 'c', 'd', 'e'};
    const std::vector<AttemptResult> results = {
        {AttemptState::WouldBlock, 0},
        {AttemptState::Progress, 2},
        {AttemptState::Progress, 1},
        {AttemptState::Progress, 2},
    };
    size_t resultIndex = 0;
    uint32_t nowMs = 10;
    size_t waitCalls = 0;
    size_t feedCalls = 0;
    auto attempt = [&results, &resultIndex](const uint8_t*, size_t) { return results[resultIndex++]; };
    auto now = [&nowMs]() { return nowMs; };
    auto wait = [&nowMs, &waitCalls](uint16_t delayMs) {
        nowMs += delayMs;
        ++waitCalls;
    };
    auto feed = [&feedCalls]() { ++feedCalls; };

    TEST_ASSERT_TRUE(WifiDiagnosticsStream::writeAll(payload, sizeof(payload), attempt, now, wait, feed, nowMs));
    TEST_ASSERT_EQUAL_UINT32(results.size(), resultIndex);
    TEST_ASSERT_EQUAL_UINT32(1, waitCalls);
    TEST_ASSERT_EQUAL_UINT32(results.size(), feedCalls);
}

void test_stream_policy_bounds_stalls_total_time_and_wraparound() {
    using WifiDiagnosticsStream::AttemptResult;
    using WifiDiagnosticsStream::AttemptState;

    const uint8_t payload[] = {'x'};
    uint32_t nowMs = UINT32_MAX - 1;
    size_t waitCalls = 0;
    size_t feedCalls = 0;
    auto attempt = [](const uint8_t*, size_t) { return AttemptResult{AttemptState::WouldBlock, 0}; };
    auto now = [&nowMs]() { return nowMs; };
    auto wait = [&nowMs, &waitCalls](uint16_t delayMs) {
        nowMs += delayMs;
        ++waitCalls;
    };
    auto feed = [&feedCalls]() { ++feedCalls; };
    WifiDiagnosticsStream::Config config;
    config.stallTimeoutMs = 3;
    config.totalTimeoutMs = 20;
    config.retryDelayMs = 1;

    TEST_ASSERT_FALSE(
        WifiDiagnosticsStream::writeAll(payload, sizeof(payload), attempt, now, wait, feed, nowMs, config));
    TEST_ASSERT_EQUAL_UINT32(3, waitCalls);
    TEST_ASSERT_EQUAL_UINT32(4, feedCalls);

    nowMs = 100;
    size_t attempts = 0;
    auto slowProgress = [&nowMs, &attempts](const uint8_t*, size_t) {
        ++attempts;
        nowMs += 2;
        return AttemptResult{AttemptState::Progress, 1};
    };
    config.stallTimeoutMs = 10;
    config.totalTimeoutMs = 3;
    const uint8_t twoBytes[] = {'x', 'y'};
    TEST_ASSERT_FALSE(
        WifiDiagnosticsStream::writeAll(twoBytes, sizeof(twoBytes), slowProgress, now, wait, feed, nowMs, config));
    TEST_ASSERT_EQUAL_UINT32(2, attempts);
}

void test_stream_policy_fails_closed_on_disconnect_error_or_invalid_progress() {
    using WifiDiagnosticsStream::AttemptResult;
    using WifiDiagnosticsStream::AttemptState;

    const uint8_t payload[] = {'x'};
    uint32_t nowMs = 0;
    auto now = [&nowMs]() { return nowMs; };
    auto wait = [&nowMs](uint16_t delayMs) { nowMs += delayMs; };
    auto feed = []() {};

    auto disconnected = [](const uint8_t*, size_t) { return AttemptResult{AttemptState::Disconnected, 0}; };
    TEST_ASSERT_FALSE(WifiDiagnosticsStream::writeAll(payload, sizeof(payload), disconnected, now, wait, feed, 0));

    auto error = [](const uint8_t*, size_t) { return AttemptResult{AttemptState::Error, 0}; };
    TEST_ASSERT_FALSE(WifiDiagnosticsStream::writeAll(payload, sizeof(payload), error, now, wait, feed, 0));

    auto invalid = [](const uint8_t*, size_t) { return AttemptResult{AttemptState::Progress, 2}; };
    TEST_ASSERT_FALSE(WifiDiagnosticsStream::writeAll(payload, sizeof(payload), invalid, now, wait, feed, 0));
}

void test_panic_breadcrumb_uses_distinct_littlefs_source() {
    File panic = panicFilesystem->open("/panic.txt", FILE_WRITE);
    TEST_ASSERT_TRUE(panic);
    TEST_ASSERT_EQUAL_UINT32(24, panic.print("littlefs crash evidence\n"));
    panic.close();

    WifiDiagnosticsApiService::Runtime runtime = makeRuntime();
    runtime.panicFilesystem = panicFilesystem;

    WebServer listServer(80);
    WifiDiagnosticsApiService::handleApiList(listServer, runtime);
    TEST_ASSERT_EQUAL_INT(200, listServer.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(listServer, "\"path\":\"/panic.txt\""));

    WebServer downloadServer(80);
    downloadServer.setArg("path", "/panic.txt");
    WifiDiagnosticsApiService::handleApiDownload(downloadServer, runtime);
    TEST_ASSERT_EQUAL_INT(200, downloadServer.lastStatusCode);
    TEST_ASSERT_EQUAL_STRING("littlefs crash evidence\n", downloadServer.lastBody.c_str());
}

void test_panic_breadcrumb_never_falls_back_to_sd() {
    writeFile("/panic.txt", "spoofed SD panic evidence\n");

    WifiDiagnosticsApiService::Runtime runtime = makeRuntime();
    WebServer listServer(80);
    WifiDiagnosticsApiService::handleApiList(listServer, runtime);
    TEST_ASSERT_EQUAL_INT(200, listServer.lastStatusCode);
    TEST_ASSERT_FALSE(responseContains(listServer, "\"path\":\"/panic.txt\""));

    WebServer downloadServer(80);
    downloadServer.setArg("path", "/panic.txt");
    WifiDiagnosticsApiService::handleApiDownload(downloadServer, runtime);
    TEST_ASSERT_EQUAL_INT(503, downloadServer.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(downloadServer, "storage_unavailable"));
}

void test_list_bounds_directory_entries_scanned() {
    const std::filesystem::path perfRoot = TEST_ROOT / "perf";
    for (size_t i = 0; i < WifiDiagnosticsApiService::MAX_SCANNED_ENTRIES + 8; ++i) {
        std::filesystem::create_directories(perfRoot / ("ignored_dir_" + std::to_string(i)));
    }

    WebServer server(80);
    WifiDiagnosticsApiService::handleApiList(server, makeRuntime());

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"maxScannedEntries\":128"));
    TEST_ASSERT_TRUE(responseContains(server, "\"truncated\":true"));
}

void test_download_rejects_traversal_missing_and_oversized_files() {
    WebServer traversal(80);
    traversal.setArg("path", "/perf/../settings.json");
    WifiDiagnosticsApiService::handleApiDownload(traversal, makeRuntime());
    TEST_ASSERT_EQUAL_INT(400, traversal.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(traversal, "invalid_path"));

    WebServer missing(80);
    missing.setArg("path", "/panic.txt");
    WifiDiagnosticsApiService::Runtime missingRuntime = makeRuntime();
    missingRuntime.panicFilesystem = panicFilesystem;
    WifiDiagnosticsApiService::handleApiDownload(missing, missingRuntime);
    TEST_ASSERT_EQUAL_INT(404, missing.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(missing, "log_not_found"));

    File oversized = testFilesystem->open("/perf/oversized.csv", FILE_WRITE);
    TEST_ASSERT_TRUE(oversized);
    TEST_ASSERT_TRUE(oversized.seek(static_cast<uint32_t>(WifiDiagnosticsApiService::MAX_DOWNLOAD_BYTES + 1)));
    TEST_ASSERT_EQUAL_UINT32(1, oversized.write(static_cast<uint8_t>('x')));
    oversized.close();

    WebServer tooLarge(80);
    tooLarge.setArg("path", "/perf/oversized.csv");
    WifiDiagnosticsApiService::handleApiDownload(tooLarge, makeRuntime());
    TEST_ASSERT_EQUAL_INT(413, tooLarge.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(tooLarge, "log_too_large"));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_path_guard_accepts_only_canonical_diagnostic_files);
    RUN_TEST(test_list_is_maintenance_only_and_requires_sd_card);
    RUN_TEST(test_list_returns_only_allowlisted_files_and_latest_power_evidence);
    RUN_TEST(test_download_streams_allowed_file_with_safe_headers);
    RUN_TEST(test_interrupted_download_closes_with_an_incomplete_declared_body);
    RUN_TEST(test_stream_policy_retries_partial_progress_and_feeds_watchdog);
    RUN_TEST(test_stream_policy_bounds_stalls_total_time_and_wraparound);
    RUN_TEST(test_stream_policy_fails_closed_on_disconnect_error_or_invalid_progress);
    RUN_TEST(test_panic_breadcrumb_uses_distinct_littlefs_source);
    RUN_TEST(test_panic_breadcrumb_never_falls_back_to_sd);
    RUN_TEST(test_list_bounds_directory_entries_scanned);
    RUN_TEST(test_download_rejects_traversal_missing_and_oversized_files);
    return UNITY_END();
}

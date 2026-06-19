#include <unity.h>

#include <cstring>

#include "../mocks/mock_heap_caps_state.h"
#include "../mocks/esp_heap_caps.h"
#include "../../src/modules/wifi/wifi_json_document.h"
#include "../../src/modules/wifi/backup_api_service.h"

#ifndef ARDUINO
SerialClass Serial;
#endif

unsigned long mockMillis = 0;
unsigned long mockMicros = 0;

namespace BackupPayloadBuilder {

bool isRecognizedBackupType(const char* type) {
    return type != nullptr &&
           (std::strcmp(type, "v1simple_backup") == 0 ||
            std::strcmp(type, "v1simple_sd_backup") == 0);
}

}  // namespace BackupPayloadBuilder

namespace BackupApiService {

bool sendCachedBackupSnapshot(WebServer& server,
                              BackupSnapshotCache& /*cache*/,
                              uint32_t /*settingsRevision*/,
                              uint32_t /*profileRevision*/,
                              BackupSnapshotBuildFn /*buildSnapshot*/,
                              void* /*buildCtx*/,
                              uint32_t (*/*millisFn*/)(void* ctx),
                              void* /*millisCtx*/) {
    server.send(200, "application/json", "{\"cached\":true}");
    return true;
}

}  // namespace BackupApiService

#include "../../src/modules/wifi/backup_api_service.cpp"

namespace {

bool responseContains(const WebServer& server, const char* needle) {
    return std::strstr(server.lastBody.c_str(), needle) != nullptr;
}

struct FakeRuntime {
    bool applyBackupReturn = true;
    int applyBackupCalls = 0;
    int syncAfterRestoreCalls = 0;
    int buildDocumentCalls = 0;
    int buildDocumentWithOptionsCalls = 0;
    bool lastIncludeWifiPasswords = false;
    int profilesRestored = 0;
    bool lastFullRestore = false;
    String lastBackupType;
};

BackupApiService::BackupRuntime makeRuntime(FakeRuntime& runtime) {
    BackupApiService::BackupRuntime apiRuntime;
    apiRuntime.ctx = &runtime;
    apiRuntime.getBackupRevision = [](void* /*ctx*/) -> uint32_t { return 7; };
    apiRuntime.getCatalogRevision = [](void* /*ctx*/) -> uint32_t { return 9; };
    apiRuntime.buildDocument = [](JsonDocument& doc, uint32_t snapshotMs, void* ctx) {
        auto* fakeRuntime = static_cast<FakeRuntime*>(ctx);
        fakeRuntime->buildDocumentCalls++;
        doc["_type"] = "v1simple_backup";
        doc["timestamp"] = snapshotMs;
        doc["includeWifiPasswords"] = false;
    };
    apiRuntime.buildDocumentWithOptions =
        [](JsonDocument& doc, uint32_t snapshotMs, bool includeWifiPasswords, void* ctx) {
            auto* fakeRuntime = static_cast<FakeRuntime*>(ctx);
            fakeRuntime->buildDocumentWithOptionsCalls++;
            fakeRuntime->lastIncludeWifiPasswords = includeWifiPasswords;
            doc["_type"] = "v1simple_backup";
            doc["timestamp"] = snapshotMs;
            doc["includeWifiPasswords"] = includeWifiPasswords;
        };
    apiRuntime.applyBackup = [](const JsonDocument& doc,
                                bool fullRestore,
                                int& profilesRestored,
                                void* ctx) {
        auto* fakeRuntime = static_cast<FakeRuntime*>(ctx);
        fakeRuntime->applyBackupCalls++;
        fakeRuntime->lastFullRestore = fullRestore;
        fakeRuntime->lastBackupType = doc["_type"].as<const char*>() ?
                                          doc["_type"].as<const char*>() :
                                          "";
        profilesRestored = fakeRuntime->profilesRestored;
        return fakeRuntime->applyBackupReturn;
    };
    apiRuntime.syncAfterRestore = [](void* ctx) {
        static_cast<FakeRuntime*>(ctx)->syncAfterRestoreCalls++;
    };
    return apiRuntime;
}

}  // namespace

void setUp() {
    mockMillis = 1000;
    mockMicros = 1000000;
    mock_reset_heap_caps();
}

void tearDown() {}

uint32_t fixedMillis(void* /*ctx*/) {
    return 4321;
}

void test_backup_default_uses_cached_snapshot_without_passwords() {
    WebServer server(80);
    BackupApiService::BackupSnapshotCache cache;
    FakeRuntime runtime;

    BackupApiService::handleApiBackup(server,
                                      cache,
                                      makeRuntime(runtime),
                                      nullptr,
                                      nullptr,
                                      fixedMillis,
                                      nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"cached\":true"));
    TEST_ASSERT_EQUAL_STRING(
        "attachment; filename=\"v1simple_backup.json\"",
        server.sentHeader("Content-Disposition").c_str());
    TEST_ASSERT_EQUAL_INT(0, runtime.buildDocumentWithOptionsCalls);
}

void test_backup_include_passwords_streams_uncached_password_export() {
    WebServer server(80);
    BackupApiService::BackupSnapshotCache cache;
    FakeRuntime runtime;
    server.setArg("includePasswords", "1");

    BackupApiService::handleApiBackup(server,
                                      cache,
                                      makeRuntime(runtime),
                                      nullptr,
                                      nullptr,
                                      fixedMillis,
                                      nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"includeWifiPasswords\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"timestamp\":4321"));
    TEST_ASSERT_EQUAL_STRING(
        "attachment; filename=\"v1simple_backup_with_wifi_passwords.json\"",
        server.sentHeader("Content-Disposition").c_str());
    TEST_ASSERT_EQUAL_INT(1, runtime.buildDocumentWithOptionsCalls);
    TEST_ASSERT_TRUE(runtime.lastIncludeWifiPasswords);
}

void test_restore_missing_body_returns_400_without_apply_or_sync() {
    WebServer server(80);
    FakeRuntime runtime;

    BackupApiService::handleApiRestore(server,
                                       makeRuntime(runtime),
                                       nullptr,
                                       nullptr,
                                       nullptr,
                                       nullptr);

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "No JSON body provided"));
    TEST_ASSERT_EQUAL_INT(0, runtime.applyBackupCalls);
    TEST_ASSERT_EQUAL_INT(0, runtime.syncAfterRestoreCalls);
}

void test_restore_invalid_json_returns_400_without_apply_or_sync() {
    WebServer server(80);
    FakeRuntime runtime;
    server.setArg("plain", "{bad");

    BackupApiService::handleApiRestore(server,
                                       makeRuntime(runtime),
                                       nullptr,
                                       nullptr,
                                       nullptr,
                                       nullptr);

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "Invalid JSON"));
    TEST_ASSERT_EQUAL_INT(0, runtime.applyBackupCalls);
    TEST_ASSERT_EQUAL_INT(0, runtime.syncAfterRestoreCalls);
}

void test_restore_invalid_backup_type_returns_400_without_apply_or_sync() {
    WebServer server(80);
    FakeRuntime runtime;
    server.setArg("plain", "{\"_type\":\"unsupported\",\"brightness\":77}");

    BackupApiService::handleApiRestore(server,
                                       makeRuntime(runtime),
                                       nullptr,
                                       nullptr,
                                       nullptr,
                                       nullptr);

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "Invalid backup format"));
    TEST_ASSERT_EQUAL_INT(0, runtime.applyBackupCalls);
    TEST_ASSERT_EQUAL_INT(0, runtime.syncAfterRestoreCalls);
}

void test_restore_apply_failure_returns_500_and_skips_sync() {
    WebServer server(80);
    FakeRuntime runtime;
    runtime.applyBackupReturn = false;
    server.setArg("plain", "{\"_type\":\"v1simple_backup\",\"brightness\":77}");

    BackupApiService::handleApiRestore(server,
                                       makeRuntime(runtime),
                                       nullptr,
                                       nullptr,
                                       nullptr,
                                       nullptr);

    TEST_ASSERT_EQUAL_INT(500, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "Failed to persist restored settings"));
    TEST_ASSERT_EQUAL_INT(1, runtime.applyBackupCalls);
    TEST_ASSERT_TRUE(runtime.lastFullRestore);
    TEST_ASSERT_EQUAL_STRING("v1simple_backup", runtime.lastBackupType.c_str());
    TEST_ASSERT_EQUAL_INT(0, runtime.syncAfterRestoreCalls);
}

void test_restore_success_syncs_runtime_and_reports_profiles_restored() {
    WebServer server(80);
    FakeRuntime runtime;
    runtime.profilesRestored = 2;
    server.setArg("plain", "{\"_type\":\"v1simple_backup\",\"brightness\":77}");

    BackupApiService::handleApiRestore(server,
                                       makeRuntime(runtime),
                                       nullptr,
                                       nullptr,
                                       nullptr,
                                       nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"success\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "Settings restored successfully (2 profiles)"));
    TEST_ASSERT_EQUAL_INT(1, runtime.applyBackupCalls);
    TEST_ASSERT_TRUE(runtime.lastFullRestore);
    TEST_ASSERT_EQUAL_STRING("v1simple_backup", runtime.lastBackupType.c_str());
    TEST_ASSERT_EQUAL_INT(1, runtime.syncAfterRestoreCalls);
}

void test_restore_success_uses_wifi_json_allocator() {
    WebServer server(80);
    FakeRuntime runtime;
    server.setArg(
        "plain",
        "{\"_type\":\"v1simple_backup\",\"profiles\":["
        "{\"name\":\"A\",\"settings\":{\"byte0\":1}},"
        "{\"name\":\"B\",\"settings\":{\"byte0\":2}}]}");

    mock_reset_heap_caps_tracking();
    BackupApiService::handleApiRestore(server,
                                       makeRuntime(runtime),
                                       nullptr,
                                       nullptr,
                                       nullptr,
                                       nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(1, runtime.applyBackupCalls);
    TEST_ASSERT_GREATER_THAN_UINT32(0u, g_mock_heap_caps_malloc_calls);
    TEST_ASSERT_EQUAL_UINT32(WifiJson::kPsramCaps, g_mock_heap_caps_last_malloc_caps);
    TEST_ASSERT_EQUAL_UINT32(0u, g_mock_heap_caps_outstanding_allocations);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_backup_default_uses_cached_snapshot_without_passwords);
    RUN_TEST(test_backup_include_passwords_streams_uncached_password_export);
    RUN_TEST(test_restore_missing_body_returns_400_without_apply_or_sync);
    RUN_TEST(test_restore_invalid_json_returns_400_without_apply_or_sync);
    RUN_TEST(test_restore_invalid_backup_type_returns_400_without_apply_or_sync);
    RUN_TEST(test_restore_apply_failure_returns_500_and_skips_sync);
    RUN_TEST(test_restore_success_syncs_runtime_and_reports_profiles_restored);
    RUN_TEST(test_restore_success_uses_wifi_json_allocator);
    return UNITY_END();
}

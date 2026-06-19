#include <unity.h>
#include <cstring>

#include <ArduinoJson.h>

#include "../mocks/mock_heap_caps_state.h"
#include "../mocks/esp_heap_caps.h"
#include "../../src/modules/wifi/wifi_json_document.h"
#include "../../src/modules/wifi/backup_snapshot_cache.h"
#include "../../src/modules/wifi/backup_snapshot_cache.cpp"

#ifndef ARDUINO
SerialClass Serial;
#endif

unsigned long mockMillis = 0;
unsigned long mockMicros = 0;

namespace {

bool responseContains(const WebServer& server, const char* needle) {
    return std::strstr(server.lastBody.c_str(), needle) != nullptr;
}

String makeBlob(size_t size, char fill) {
    String blob;
    for (size_t i = 0; i < size; ++i) {
        blob += fill;
    }
    return blob;
}

struct FakeSnapshotSource {
    int buildCalls = 0;
    String name = "initial";
    String blob = "tiny";
};

void buildFnImpl(JsonDocument& doc, uint32_t snapshotMs, void* ctx) {
    auto* source = static_cast<FakeSnapshotSource*>(ctx);
    source->buildCalls++;
    doc["_type"] = "v1simple_backup";
    doc["timestamp"] = snapshotMs;
    doc["name"] = source->name;
    doc["blob"] = source->blob;
}

uint32_t getMillis(void* ctx) {
    return *static_cast<uint32_t*>(ctx);
}

void releaseCache(BackupApiService::BackupSnapshotCache& cache) {
    BackupApiService::releaseBackupSnapshotCache(cache);
}

}  // namespace

void setUp() {
    mockMillis = 1000;
    mockMicros = 1000000;
    mock_reset_heap_caps();
}

void tearDown() {}

void test_first_get_builds_and_caches_snapshot() {
    WebServer server(80);
    BackupApiService::BackupSnapshotCache cache;
    FakeSnapshotSource source;
    uint32_t now = 1111;

    const bool cached = BackupApiService::sendCachedBackupSnapshot(
        server,
        cache,
        7,
        9,
        buildFnImpl,
        &source,
        getMillis,
        &now);

    TEST_ASSERT_TRUE(cached);
    TEST_ASSERT_EQUAL_INT(1, source.buildCalls);
    TEST_ASSERT_TRUE(cache.valid);
    TEST_ASSERT_EQUAL_UINT32(1111, cache.snapshotMs);
    TEST_ASSERT_EQUAL_UINT32(7, cache.settingsRevision);
    TEST_ASSERT_EQUAL_UINT32(9, cache.profileRevision);
    TEST_ASSERT_TRUE(cache.inPsram);
    TEST_ASSERT_GREATER_THAN_UINT32(1u, g_mock_heap_caps_malloc_calls);
    TEST_ASSERT_EQUAL_UINT32(MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM, g_mock_heap_caps_last_malloc_caps);
    TEST_ASSERT_EQUAL_UINT32(WifiJson::kPsramCaps, g_mock_heap_caps_last_malloc_caps);
    TEST_ASSERT_TRUE(cache.capacity >= (cache.length + 1u));
    TEST_ASSERT_EQUAL_UINT32(0u, cache.capacity % 256u);
    TEST_ASSERT_TRUE(responseContains(server, "\"timestamp\":1111"));
    TEST_ASSERT_TRUE(responseContains(server, "\"name\":\"initial\""));
    releaseCache(cache);
}

void test_second_get_with_unchanged_revisions_reuses_cached_bytes() {
    WebServer server(80);
    BackupApiService::BackupSnapshotCache cache;
    FakeSnapshotSource source;
    uint32_t now = 2222;

    BackupApiService::sendCachedBackupSnapshot(
        server,
        cache,
        1,
        1,
        buildFnImpl,
        &source,
        getMillis,
        &now);
    const String firstBody = server.lastBody;
    const uint32_t firstSnapshotMs = cache.snapshotMs;
    const uint32_t mallocCallsAfterFirst = g_mock_heap_caps_malloc_calls;

    source.name = "changed";
    source.blob = "changed";
    now = 3333;

    const bool cached = BackupApiService::sendCachedBackupSnapshot(
        server,
        cache,
        1,
        1,
        buildFnImpl,
        &source,
        getMillis,
        &now);

    TEST_ASSERT_TRUE(cached);
    TEST_ASSERT_EQUAL_INT(1, source.buildCalls);
    TEST_ASSERT_EQUAL_STRING(firstBody.c_str(), server.lastBody.c_str());
    TEST_ASSERT_EQUAL_UINT32(firstSnapshotMs, cache.snapshotMs);
    TEST_ASSERT_EQUAL_UINT32(mallocCallsAfterFirst, g_mock_heap_caps_malloc_calls);
    TEST_ASSERT_TRUE(responseContains(server, "\"name\":\"initial\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"timestamp\":2222"));
    releaseCache(cache);
}

void test_settings_revision_change_forces_rebuild() {
    WebServer server(80);
    BackupApiService::BackupSnapshotCache cache;
    FakeSnapshotSource source;
    uint32_t now = 4444;

    BackupApiService::sendCachedBackupSnapshot(
        server,
        cache,
        1,
        1,
        buildFnImpl,
        &source,
        getMillis,
        &now);

    source.name = "settings-rebuild";
    now = 5555;

    const bool cached = BackupApiService::sendCachedBackupSnapshot(
        server,
        cache,
        2,
        1,
        buildFnImpl,
        &source,
        getMillis,
        &now);

    TEST_ASSERT_TRUE(cached);
    TEST_ASSERT_EQUAL_INT(2, source.buildCalls);
    TEST_ASSERT_EQUAL_UINT32(5555, cache.snapshotMs);
    TEST_ASSERT_EQUAL_UINT32(2, cache.settingsRevision);
    TEST_ASSERT_TRUE(responseContains(server, "\"name\":\"settings-rebuild\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"timestamp\":5555"));
    releaseCache(cache);
}

void test_profile_revision_change_forces_rebuild() {
    WebServer server(80);
    BackupApiService::BackupSnapshotCache cache;
    FakeSnapshotSource source;
    uint32_t now = 6666;

    BackupApiService::sendCachedBackupSnapshot(
        server,
        cache,
        1,
        1,
        buildFnImpl,
        &source,
        getMillis,
        &now);

    source.name = "profile-rebuild";
    now = 7777;

    const bool cached = BackupApiService::sendCachedBackupSnapshot(
        server,
        cache,
        1,
        2,
        buildFnImpl,
        &source,
        getMillis,
        &now);

    TEST_ASSERT_TRUE(cached);
    TEST_ASSERT_EQUAL_INT(2, source.buildCalls);
    TEST_ASSERT_EQUAL_UINT32(7777, cache.snapshotMs);
    TEST_ASSERT_EQUAL_UINT32(2, cache.profileRevision);
    TEST_ASSERT_TRUE(responseContains(server, "\"name\":\"profile-rebuild\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"timestamp\":7777"));
    releaseCache(cache);
}

void test_psram_failure_falls_back_to_internal_cache() {
    WebServer server(80);
    BackupApiService::BackupSnapshotCache cache;
    FakeSnapshotSource source;
    uint32_t now = 8888;
    // The backup document now uses WifiJson; fail the cache-buffer PSRAM
    // allocation after document-pool allocations have completed.
    g_mock_heap_caps_fail_on_call = 4u;

    const bool cached = BackupApiService::sendCachedBackupSnapshot(
        server,
        cache,
        3,
        4,
        buildFnImpl,
        &source,
        getMillis,
        &now);

    TEST_ASSERT_TRUE(cached);
    TEST_ASSERT_EQUAL_INT(1, source.buildCalls);
    TEST_ASSERT_EQUAL_UINT32(5, g_mock_heap_caps_malloc_calls);
    TEST_ASSERT_EQUAL_UINT32(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL, g_mock_heap_caps_last_malloc_caps);
    TEST_ASSERT_FALSE(cache.inPsram);
    releaseCache(cache);
}

void test_total_allocation_failure_streams_fresh_doc_without_reusing_stale_cache() {
    WebServer server(80);
    BackupApiService::BackupSnapshotCache cache;
    FakeSnapshotSource source;
    uint32_t now = 9000;

    BackupApiService::sendCachedBackupSnapshot(
        server,
        cache,
        1,
        1,
        buildFnImpl,
        &source,
        getMillis,
        &now);

    const String cachedBody = server.lastBody;
    const uint32_t cachedSnapshotMs = cache.snapshotMs;
    const uint32_t cachedSettingsRevision = cache.settingsRevision;
    const uint32_t cachedProfileRevision = cache.profileRevision;
    const size_t cachedLength = cache.length;

    source.name = "uncached-fallback";
    source.blob = makeBlob(cache.capacity + 128u, 'x');
    now = 9010;
    mock_reset_heap_caps();
    // Fail the cache-buffer PSRAM allocation and its internal fallback while
    // allowing the WifiJson snapshot document allocation to succeed.
    g_mock_heap_caps_fail_call_mask = 0x18u;

    const bool cachedRefresh = BackupApiService::sendCachedBackupSnapshot(
        server,
        cache,
        2,
        1,
        buildFnImpl,
        &source,
        getMillis,
        &now);

    TEST_ASSERT_FALSE(cachedRefresh);
    TEST_ASSERT_EQUAL_INT(2, source.buildCalls);
    TEST_ASSERT_TRUE(responseContains(server, "\"name\":\"uncached-fallback\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"timestamp\":9010"));
    TEST_ASSERT_EQUAL_STRING(cachedBody.c_str(), String(cache.data ? cache.data : "").c_str());
    TEST_ASSERT_EQUAL_UINT32(cachedSnapshotMs, cache.snapshotMs);
    TEST_ASSERT_EQUAL_UINT32(cachedSettingsRevision, cache.settingsRevision);
    TEST_ASSERT_EQUAL_UINT32(cachedProfileRevision, cache.profileRevision);
    TEST_ASSERT_EQUAL_UINT(cachedLength, cache.length);

    now = 9020;
    mock_reset_heap_caps();

    const bool cachedRebuild = BackupApiService::sendCachedBackupSnapshot(
        server,
        cache,
        2,
        1,
        buildFnImpl,
        &source,
        getMillis,
        &now);

    TEST_ASSERT_TRUE(cachedRebuild);
    TEST_ASSERT_EQUAL_INT(3, source.buildCalls);
    TEST_ASSERT_EQUAL_UINT32(9020, cache.snapshotMs);
    TEST_ASSERT_EQUAL_UINT32(2, cache.settingsRevision);
    TEST_ASSERT_TRUE(responseContains(server, "\"name\":\"uncached-fallback\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"timestamp\":9020"));
    releaseCache(cache);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_first_get_builds_and_caches_snapshot);
    RUN_TEST(test_second_get_with_unchanged_revisions_reuses_cached_bytes);
    RUN_TEST(test_settings_revision_change_forces_rebuild);
    RUN_TEST(test_profile_revision_change_forces_rebuild);
    RUN_TEST(test_psram_failure_falls_back_to_internal_cache);
    RUN_TEST(test_total_allocation_failure_streams_fresh_doc_without_reusing_stale_cache);
    return UNITY_END();
}

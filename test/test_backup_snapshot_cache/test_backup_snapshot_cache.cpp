#include <unity.h>

#include <cstring>

#include "../mocks/mock_heap_caps_state.h"
#include "../mocks/esp_heap_caps.h"
#include "../../src/modules/wifi/backup_snapshot_cache.h"

#ifndef ARDUINO
SerialClass Serial;
#endif

unsigned long mockMillis = 0;
unsigned long mockMicros = 0;

#include "../../src/modules/wifi/backup_snapshot_cache.cpp"

namespace {

struct BuildProbe {
    bool safe = true;
    int calls = 0;
    const char* marker = "valid";
};

BackupApiService::BackupSnapshotBuildResult buildSnapshot(JsonDocument& doc, uint32_t, void* ctx) {
    auto* probe = static_cast<BuildProbe*>(ctx);
    probe->calls++;
    doc["marker"] = probe->marker;
    return BackupApiService::BackupSnapshotBuildResult{probe->safe};
}

uint32_t fixedMillis(void*) {
    return 1234;
}

bool contains(const String& body, const char* value) {
    return body.indexOf(value) >= 0;
}

} // namespace

void setUp() {
    mock_reset_heap_caps();
}

void tearDown() {}

void test_incomplete_snapshot_returns_503_and_preserves_prior_valid_cache() {
    BackupApiService::BackupSnapshotCache cache;
    BuildProbe probe;
    WebServer first(80);

    TEST_ASSERT_TRUE(BackupApiService::sendCachedBackupSnapshot(
        first, cache, 1, 1, buildSnapshot, &probe, fixedMillis, nullptr));
    TEST_ASSERT_EQUAL_INT(200, first.lastStatusCode);
    TEST_ASSERT_TRUE(cache.valid);
    TEST_ASSERT_TRUE(contains(first.lastBody, "valid"));
    const uint32_t priorSettingsRevision = cache.settingsRevision;
    const uint32_t priorProfileRevision = cache.profileRevision;
    const String priorBytes(cache.data);

    probe.safe = false;
    probe.marker = "incomplete";
    WebServer rejected(80);
    TEST_ASSERT_FALSE(BackupApiService::sendCachedBackupSnapshot(
        rejected, cache, 2, 2, buildSnapshot, &probe, fixedMillis, nullptr));
    TEST_ASSERT_EQUAL_INT(503, rejected.lastStatusCode);
    TEST_ASSERT_TRUE(contains(rejected.lastBody, "backup_snapshot_incomplete"));
    TEST_ASSERT_TRUE(cache.valid);
    TEST_ASSERT_EQUAL_UINT32(priorSettingsRevision, cache.settingsRevision);
    TEST_ASSERT_EQUAL_UINT32(priorProfileRevision, cache.profileRevision);
    TEST_ASSERT_EQUAL_STRING(priorBytes.c_str(), cache.data);

    WebServer cached(80);
    TEST_ASSERT_TRUE(BackupApiService::sendCachedBackupSnapshot(
        cached, cache, 1, 1, buildSnapshot, &probe, fixedMillis, nullptr));
    TEST_ASSERT_EQUAL_INT(200, cached.lastStatusCode);
    TEST_ASSERT_TRUE(contains(cached.lastBody, "valid"));
    TEST_ASSERT_FALSE(contains(cached.lastBody, "incomplete"));
    TEST_ASSERT_EQUAL_INT(2, probe.calls);

    BackupApiService::releaseBackupSnapshotCache(cache);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_incomplete_snapshot_returns_503_and_preserves_prior_valid_cache);
    return UNITY_END();
}

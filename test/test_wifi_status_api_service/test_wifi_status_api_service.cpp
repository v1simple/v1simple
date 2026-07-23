#include <unity.h>
#include <cstring>
#include <functional>

#include <ArduinoJson.h>
#include "../mocks/mock_heap_caps_state.h"
#include "../mocks/esp_heap_caps.h"
#include "../../include/wifi_rate_limiter.h"
#include "../../src/modules/wifi/wifi_json_document.h"
#include "../../src/modules/wifi/wifi_status_api_service.h"
#include "../../src/modules/wifi/wifi_status_api_service.cpp"  // Pull implementation for UNIT_TEST.

#ifndef ARDUINO
SerialClass Serial;
#endif

unsigned long mockMillis = 0;
unsigned long mockMicros = 0;

static bool responseContains(const WebServer& server, const char* needle) {
    return std::strstr(server.lastBody.c_str(), needle) != nullptr;
}

static void releaseCache(WifiStatusApiService::StatusJsonCache& cache, unsigned long& cacheTime) {
    WifiStatusApiService::releaseStatusJsonCache(cache, cacheTime);
}

struct FakeStatusRuntime {
    bool setupModeActive = false;
    bool staConnected = false;
    String staIp = "";
    String apIp = "192.168.35.5";
    String connectedSsid = "";
    int32_t rssi = 0;
    bool staEnabled = false;
    String staSavedSsid = "";
    String apSsid = "V1-Simple";

    unsigned long uptimeSeconds = 0;
    uint32_t heapFree = 0;
    String hostname = "v1simple";
    String firmwareVersion = "test-fw";

    bool timeValid = false;

    uint16_t batteryVoltageMv = 0;
    uint8_t batteryPercentage = 0;
    bool batteryOnBattery = false;
    bool batteryHasBattery = false;

    bool v1Connected = false;
    std::function<void(JsonObject)> mergeStatus;
    std::function<void(JsonObject)> mergeStatus2;
    std::function<void(JsonObject)> mergeAlert;

    int setupModeActiveCalls = 0;
};

static WifiStatusApiService::StatusRuntime makeRuntime(FakeStatusRuntime& rt) {
    WifiStatusApiService::StatusRuntime r;
    r.setupModeActive = [](void* ctx) {
        auto* rtp = static_cast<FakeStatusRuntime*>(ctx);
        rtp->setupModeActiveCalls++;
        return rtp->setupModeActive;
    };
    r.setupModeActiveCtx = &rt;
    r.staConnected = [](void* ctx) { return static_cast<FakeStatusRuntime*>(ctx)->staConnected; };
    r.staConnectedCtx = &rt;
    r.staIp = [](void* ctx) { return static_cast<FakeStatusRuntime*>(ctx)->staIp; };
    r.staIpCtx = &rt;
    r.apIp = [](void* ctx) { return static_cast<FakeStatusRuntime*>(ctx)->apIp; };
    r.apIpCtx = &rt;
    r.connectedSsid = [](void* ctx) { return static_cast<FakeStatusRuntime*>(ctx)->connectedSsid; };
    r.connectedSsidCtx = &rt;
    r.rssi = [](void* ctx) { return static_cast<FakeStatusRuntime*>(ctx)->rssi; };
    r.rssiCtx = &rt;
    r.staEnabled = [](void* ctx) { return static_cast<FakeStatusRuntime*>(ctx)->staEnabled; };
    r.staEnabledCtx = &rt;
    r.staSavedSsid = [](void* ctx) { return static_cast<FakeStatusRuntime*>(ctx)->staSavedSsid; };
    r.staSavedSsidCtx = &rt;
    r.apSsid = [](void* ctx) { return static_cast<FakeStatusRuntime*>(ctx)->apSsid; };
    r.apSsidCtx = &rt;
    r.uptimeSeconds = [](void* ctx) { return static_cast<FakeStatusRuntime*>(ctx)->uptimeSeconds; };
    r.uptimeSecondsCtx = &rt;
    r.heapFree = [](void* ctx) { return static_cast<FakeStatusRuntime*>(ctx)->heapFree; };
    r.heapFreeCtx = &rt;
    r.hostname = [](void* ctx) { return static_cast<FakeStatusRuntime*>(ctx)->hostname; };
    r.hostnameCtx = &rt;
    r.firmwareVersion = [](void* ctx) { return static_cast<FakeStatusRuntime*>(ctx)->firmwareVersion; };
    r.firmwareVersionCtx = &rt;
    r.batteryVoltageMv = [](void* ctx) { return static_cast<FakeStatusRuntime*>(ctx)->batteryVoltageMv; };
    r.batteryVoltageMvCtx = &rt;
    r.batteryPercentage = [](void* ctx) { return static_cast<FakeStatusRuntime*>(ctx)->batteryPercentage; };
    r.batteryPercentageCtx = &rt;
    r.batteryOnBattery = [](void* ctx) { return static_cast<FakeStatusRuntime*>(ctx)->batteryOnBattery; };
    r.batteryOnBatteryCtx = &rt;
    r.batteryHasBattery = [](void* ctx) { return static_cast<FakeStatusRuntime*>(ctx)->batteryHasBattery; };
    r.batteryHasBatteryCtx = &rt;
    r.v1Connected = [](void* ctx) { return static_cast<FakeStatusRuntime*>(ctx)->v1Connected; };
    r.v1ConnectedCtx = &rt;
    r.mergeStatus = [](JsonObject obj, void* ctx) {
        auto* rtp = static_cast<FakeStatusRuntime*>(ctx);
        if (rtp->mergeStatus) rtp->mergeStatus(obj);
    };
    r.mergeStatusCtx = &rt;
    r.mergeStatus2 = [](JsonObject obj, void* ctx) {
        auto* rtp = static_cast<FakeStatusRuntime*>(ctx);
        if (rtp->mergeStatus2) rtp->mergeStatus2(obj);
    };
    r.mergeStatus2Ctx = &rt;
    r.mergeAlert = [](JsonObject obj, void* ctx) {
        auto* rtp = static_cast<FakeStatusRuntime*>(ctx);
        if (rtp->mergeAlert) rtp->mergeAlert(obj);
    };
    r.mergeAlertCtx = &rt;
    return r;
}

void setUp() {
    mockMillis = 1000;
    mockMicros = 1000000;
    mock_reset_heap_caps();
}

void tearDown() {}

void test_handle_status_builds_core_payload() {
    WebServer server(80);
    FakeStatusRuntime rt;
    rt.setupModeActive = true;
    rt.staConnected = true;
    rt.staIp = "10.0.0.24";
    rt.connectedSsid = "HomeWiFi";
    rt.rssi = -55;
    rt.staEnabled = true;
    rt.staSavedSsid = "SavedWiFi";
    rt.uptimeSeconds = 321;
    rt.heapFree = 65432;
    rt.firmwareVersion = "1.2.3-test";
    rt.batteryVoltageMv = 4042;
    rt.batteryPercentage = 84;
    rt.batteryOnBattery = true;
    rt.batteryHasBattery = true;
    rt.v1Connected = true;

    WifiStatusApiService::StatusJsonCache cache;
    unsigned long cacheTime = 0;
    unsigned long now = 1000;

    WifiStatusApiService::handleApiStatus(
        server,
        makeRuntime(rt),
        cache,
        cacheTime,
        500,
        [](void* ctx) -> unsigned long { return *static_cast<unsigned long*>(ctx); }, &now,
        nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"setup_mode\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"sta_connected\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"sta_ip\":\"10.0.0.24\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"ap_ip\":\"192.168.35.5\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"ssid\":\"HomeWiFi\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"sta_enabled\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"sta_ssid\":\"SavedWiFi\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"uptime\":321"));
    TEST_ASSERT_TRUE(responseContains(server, "\"heap_free\":65432"));
    TEST_ASSERT_TRUE(responseContains(server, "\"firmware_version\":\"1.2.3-test\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"voltage_mv\":4042"));
    TEST_ASSERT_TRUE(responseContains(server, "\"percentage\":84"));
    TEST_ASSERT_TRUE(responseContains(server, "\"v1_connected\":true"));
    TEST_ASSERT_EQUAL_UINT32(1000, cacheTime);
    TEST_ASSERT_EQUAL_INT(1, rt.setupModeActiveCalls);
    releaseCache(cache, cacheTime);
}

void test_handle_status_merges_legacy_status_and_alert_json() {
    WebServer server(80);
    FakeStatusRuntime rt;
    rt.v1Connected = true;
    rt.mergeStatus = [](JsonObject obj) {
        obj["foo"] = 123;
        obj["v1_connected"] = false;
    };
    rt.mergeAlert = [](JsonObject obj) {
        obj["band"] = "Ka";
    };

    WifiStatusApiService::StatusJsonCache cache;
    unsigned long cacheTime = 0;
    unsigned long now = 2000;

    WifiStatusApiService::handleApiStatus(
        server,
        makeRuntime(rt),
        cache,
        cacheTime,
        500,
        [](void* ctx) -> unsigned long { return *static_cast<unsigned long*>(ctx); }, &now,
        nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"foo\":123"));
    TEST_ASSERT_TRUE(responseContains(server, "\"v1_connected\":false"));
    TEST_ASSERT_TRUE(responseContains(server, "\"alert\":{\"band\":\"Ka\"}"));
    releaseCache(cache, cacheTime);
}

void test_handle_status_preserves_nested_wifi_merges() {
    WebServer server(80);
    FakeStatusRuntime rt;
    rt.mergeStatus = [](JsonObject obj) {
        JsonObject wifi = obj["wifi"].as<JsonObject>();
        wifi["low_dma_cooldown_ms"] = 9000;
        JsonObject autoStart = wifi["auto_start"].to<JsonObject>();
        autoStart["gate"] = "waiting_dma";
    };

    WifiStatusApiService::StatusJsonCache cache;
    unsigned long cacheTime = 0;
    unsigned long now = 2100;

    WifiStatusApiService::handleApiStatus(
        server,
        makeRuntime(rt),
        cache,
        cacheTime,
        500,
        [](void* ctx) -> unsigned long { return *static_cast<unsigned long*>(ctx); }, &now,
        nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"low_dma_cooldown_ms\":9000"));
    TEST_ASSERT_TRUE(responseContains(server, "\"auto_start\":{\"gate\":\"waiting_dma\"}"));
    releaseCache(cache, cacheTime);
}

void test_handle_status_surfaces_appended_maintenance_boot_fields() {
    WebServer server(80);
    FakeStatusRuntime rt;
    rt.mergeStatus2 = [](JsonObject obj) {
        obj["maintenanceBoot"] = true;
        obj["maintenanceBootUptimeMs"] = 4321;
    };

    WifiStatusApiService::StatusJsonCache cache;
    unsigned long cacheTime = 0;
    unsigned long now = 2200;

    WifiStatusApiService::handleApiStatus(
        server,
        makeRuntime(rt),
        cache,
        cacheTime,
        500,
        [](void* ctx) -> unsigned long { return *static_cast<unsigned long*>(ctx); }, &now,
        nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"maintenanceBoot\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"maintenanceBootUptimeMs\":4321"));
    releaseCache(cache, cacheTime);
}

void test_handle_status_cache_hit_reuses_cached_payload() {
    WebServer server(80);
    FakeStatusRuntime rt;
    rt.apSsid = "InitialAP";

    WifiStatusApiService::StatusJsonCache cache;
    unsigned long cacheTime = 0;
    unsigned long now = 1000;

    WifiStatusApiService::handleApiStatus(
        server,
        makeRuntime(rt),
        cache,
        cacheTime,
        500,
        [](void* ctx) -> unsigned long { return *static_cast<unsigned long*>(ctx); }, &now,
        nullptr, nullptr);

    const String firstBody = server.lastBody;
    TEST_ASSERT_TRUE(responseContains(server, "\"ssid\":\"InitialAP\""));
    TEST_ASSERT_EQUAL_INT(1, rt.setupModeActiveCalls);

    rt.apSsid = "ChangedAP";
    now = 1200;  // within 500ms TTL

    WifiStatusApiService::handleApiStatus(
        server,
        makeRuntime(rt),
        cache,
        cacheTime,
        500,
        [](void* ctx) -> unsigned long { return *static_cast<unsigned long*>(ctx); }, &now,
        nullptr, nullptr);

    TEST_ASSERT_EQUAL_STRING(firstBody.c_str(), server.lastBody.c_str());
    TEST_ASSERT_TRUE(responseContains(server, "\"ssid\":\"InitialAP\""));
    TEST_ASSERT_EQUAL_INT(1, rt.setupModeActiveCalls);
    releaseCache(cache, cacheTime);
}

void test_handle_status_cache_expiry_rebuilds_payload() {
    WebServer server(80);
    FakeStatusRuntime rt;
    rt.apSsid = "InitialAP";

    WifiStatusApiService::StatusJsonCache cache;
    unsigned long cacheTime = 0;
    unsigned long now = 1000;

    WifiStatusApiService::handleApiStatus(
        server,
        makeRuntime(rt),
        cache,
        cacheTime,
        500,
        [](void* ctx) -> unsigned long { return *static_cast<unsigned long*>(ctx); }, &now,
        nullptr, nullptr);

    TEST_ASSERT_TRUE(responseContains(server, "\"ssid\":\"InitialAP\""));
    TEST_ASSERT_EQUAL_INT(1, rt.setupModeActiveCalls);

    rt.apSsid = "ChangedAP";
    now = 2000;  // past 500ms TTL

    WifiStatusApiService::handleApiStatus(
        server,
        makeRuntime(rt),
        cache,
        cacheTime,
        500,
        [](void* ctx) -> unsigned long { return *static_cast<unsigned long*>(ctx); }, &now,
        nullptr, nullptr);

    TEST_ASSERT_TRUE(responseContains(server, "\"ssid\":\"ChangedAP\""));
    TEST_ASSERT_EQUAL_INT(2, rt.setupModeActiveCalls);
    releaseCache(cache, cacheTime);
}

void test_handle_status_prefers_psram_cache_allocation() {
    WebServer server(80);
    FakeStatusRuntime rt;
    WifiStatusApiService::StatusJsonCache cache;
    unsigned long cacheTime = 0;

    WifiStatusApiService::handleApiStatus(
        server,
        makeRuntime(rt),
        cache,
        cacheTime,
        500,
        [](void* /*ctx*/) -> unsigned long { return 1000UL; }, nullptr,
        nullptr, nullptr);

    TEST_ASSERT_GREATER_THAN_UINT32(1u, g_mock_heap_caps_malloc_calls);
    TEST_ASSERT_EQUAL_UINT32(MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM, g_mock_heap_caps_last_malloc_caps);
    TEST_ASSERT_EQUAL_UINT32(WifiJson::kPsramCaps, g_mock_heap_caps_last_malloc_caps);
    TEST_ASSERT_NOT_NULL(cache.data);
    TEST_ASSERT_TRUE(cache.inPsram);
    TEST_ASSERT_TRUE(cache.capacity >= cache.length + 1u);
    releaseCache(cache, cacheTime);
}

void test_handle_status_falls_back_to_internal_cache_allocation() {
    WebServer server(80);
    FakeStatusRuntime rt;
    WifiStatusApiService::StatusJsonCache cache;
    unsigned long cacheTime = 0;

    // The status document itself now uses the PSRAM-preferring WifiJson allocator.
    // Fail the first cache-buffer PSRAM allocation after the document pool has
    // been built, then verify the serialized cache falls back to internal RAM.
    g_mock_heap_caps_fail_on_call = 6u;

    WifiStatusApiService::handleApiStatus(
        server,
        makeRuntime(rt),
        cache,
        cacheTime,
        500,
        [](void* /*ctx*/) -> unsigned long { return 1000UL; }, nullptr,
        nullptr, nullptr);

    TEST_ASSERT_EQUAL_UINT32(7, g_mock_heap_caps_malloc_calls);
    TEST_ASSERT_EQUAL_UINT32(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL, g_mock_heap_caps_last_malloc_caps);
    TEST_ASSERT_NOT_NULL(cache.data);
    TEST_ASSERT_FALSE(cache.inPsram);
    releaseCache(cache, cacheTime);
}

void test_handle_status_allocation_failure_falls_back_to_uncached_send() {
    WebServer server(80);
    FakeStatusRuntime rt;
    rt.apSsid = "FallbackAP";
    WifiStatusApiService::StatusJsonCache cache;
    unsigned long cacheTime = 0;

    // Fail the cache-buffer PSRAM allocation and its internal fallback while
    // allowing the WifiJson document allocation to succeed.
    g_mock_heap_caps_fail_call_mask = 0x60u;

    WifiStatusApiService::handleApiStatus(
        server,
        makeRuntime(rt),
        cache,
        cacheTime,
        500,
        [](void* /*ctx*/) -> unsigned long { return 1000UL; }, nullptr,
        nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"ssid\":\"FallbackAP\""));
    TEST_ASSERT_NULL(cache.data);
    TEST_ASSERT_EQUAL_UINT(0, cache.capacity);
    TEST_ASSERT_EQUAL_UINT(0, cache.length);
    TEST_ASSERT_FALSE(cache.inPsram);
    TEST_ASSERT_EQUAL_UINT32(0, cacheTime);
}

void test_handle_status_invalidation_forces_rebuild_within_ttl() {
    WebServer server(80);
    FakeStatusRuntime rt;
    rt.apSsid = "InitialAP";
    WifiStatusApiService::StatusJsonCache cache;
    unsigned long cacheTime = 0;
    unsigned long now = 1000;

    WifiStatusApiService::handleApiStatus(
        server,
        makeRuntime(rt),
        cache,
        cacheTime,
        500,
        [](void* ctx) -> unsigned long { return *static_cast<unsigned long*>(ctx); }, &now,
        nullptr, nullptr);

    TEST_ASSERT_TRUE(responseContains(server, "\"ssid\":\"InitialAP\""));
    TEST_ASSERT_EQUAL_INT(1, rt.setupModeActiveCalls);

    WifiStatusApiService::invalidateStatusJsonCache(cache, cacheTime);
    rt.apSsid = "UpdatedAP";
    now = 1200;

    WifiStatusApiService::handleApiStatus(
        server,
        makeRuntime(rt),
        cache,
        cacheTime,
        500,
        [](void* ctx) -> unsigned long { return *static_cast<unsigned long*>(ctx); }, &now,
        nullptr, nullptr);

    TEST_ASSERT_TRUE(responseContains(server, "\"ssid\":\"UpdatedAP\""));
    TEST_ASSERT_EQUAL_UINT32(1200, cacheTime);
    TEST_ASSERT_EQUAL_INT(2, rt.setupModeActiveCalls);
    releaseCache(cache, cacheTime);
}

void test_release_status_cache_frees_buffer_and_resets_state() {
    WebServer server(80);
    FakeStatusRuntime rt;
    WifiStatusApiService::StatusJsonCache cache;
    unsigned long cacheTime = 0;

    WifiStatusApiService::handleApiStatus(
        server,
        makeRuntime(rt),
        cache,
        cacheTime,
        500,
        [](void* /*ctx*/) -> unsigned long { return 1000UL; }, nullptr,
        nullptr, nullptr);

    TEST_ASSERT_NOT_NULL(cache.data);
    const uint32_t freeCallsBeforeRelease = g_mock_heap_caps_free_calls;
    WifiStatusApiService::releaseStatusJsonCache(cache, cacheTime);

    TEST_ASSERT_EQUAL_UINT32(freeCallsBeforeRelease + 1u, g_mock_heap_caps_free_calls);
    TEST_ASSERT_NULL(cache.data);
    TEST_ASSERT_EQUAL_UINT(0, cache.capacity);
    TEST_ASSERT_EQUAL_UINT(0, cache.length);
    TEST_ASSERT_FALSE(cache.inPsram);
    TEST_ASSERT_EQUAL_UINT32(0, cacheTime);
}

void test_sustained_status_polling_marks_activity_without_consuming_mutation_capacity() {
    WebServer server(80);
    FakeStatusRuntime rt;
    WifiStatusApiService::StatusJsonCache cache;
    unsigned long cacheTime = 0;
    SlidingWindowRateLimiter mutationLimiter;
    for (size_t i = 0; i + 1 < SlidingWindowRateLimiter::MAX_REQUESTS; ++i) {
        TEST_ASSERT_TRUE(mutationLimiter.evaluate(1000).allowed);
    }

    int uiActivityCalls = 0;

    constexpr size_t statusPollCount = 256;
    for (size_t i = 0; i < statusPollCount; ++i) {
        WifiStatusApiService::handleApiStatus(
            server,
            makeRuntime(rt),
            cache,
            cacheTime,
            500,
            [](void* /*ctx*/) -> unsigned long { return 1000UL; }, nullptr,
            [](void* ctx) { ++*static_cast<int*>(ctx); }, &uiActivityCalls);
    }

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(statusPollCount, uiActivityCalls);
    TEST_ASSERT_EQUAL_INT(1, rt.setupModeActiveCalls);

    const SlidingWindowRateLimitDecision finalMutation = mutationLimiter.evaluate(1001);
    TEST_ASSERT_TRUE(finalMutation.allowed);
    TEST_ASSERT_EQUAL_UINT32(SlidingWindowRateLimiter::MAX_REQUESTS, finalMutation.requestCount);
    TEST_ASSERT_FALSE(mutationLimiter.evaluate(1002).allowed);
    releaseCache(cache, cacheTime);
}

void test_handle_api_status_delegates_without_activity_callback() {
    WebServer server(80);
    FakeStatusRuntime rt;
    rt.apSsid = "StatusApiAP";
    WifiStatusApiService::StatusJsonCache cache;
    unsigned long cacheTime = 0;

    WifiStatusApiService::handleApiStatus(
        server,
        makeRuntime(rt),
        cache,
        cacheTime,
        500,
        [](void* /*ctx*/) -> unsigned long { return 2000UL; }, nullptr,
        nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"ssid\":\"StatusApiAP\""));
    TEST_ASSERT_EQUAL_INT(1, rt.setupModeActiveCalls);
    releaseCache(cache, cacheTime);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_handle_status_builds_core_payload);
    RUN_TEST(test_handle_status_merges_legacy_status_and_alert_json);
    RUN_TEST(test_handle_status_preserves_nested_wifi_merges);
    RUN_TEST(test_handle_status_surfaces_appended_maintenance_boot_fields);
    RUN_TEST(test_handle_status_cache_hit_reuses_cached_payload);
    RUN_TEST(test_handle_status_cache_expiry_rebuilds_payload);
    RUN_TEST(test_handle_status_prefers_psram_cache_allocation);
    RUN_TEST(test_handle_status_falls_back_to_internal_cache_allocation);
    RUN_TEST(test_handle_status_allocation_failure_falls_back_to_uncached_send);
    RUN_TEST(test_handle_status_invalidation_forces_rebuild_within_ttl);
    RUN_TEST(test_release_status_cache_frees_buffer_and_resets_state);
    RUN_TEST(test_sustained_status_polling_marks_activity_without_consuming_mutation_capacity);
    RUN_TEST(test_handle_api_status_delegates_without_activity_callback);
    return UNITY_END();
}

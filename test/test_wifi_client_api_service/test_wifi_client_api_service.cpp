#include <unity.h>
#include <cstring>
#include <vector>

#include "../mocks/mock_heap_caps_state.h"
#include "../mocks/esp_heap_caps.h"
#include "../../src/modules/wifi/wifi_json_document.h"
#include "../../src/modules/wifi/wifi_client_api_service.h"
#include "../../src/modules/wifi/wifi_client_api_service.cpp" // Pull implementation for UNIT_TEST.

#ifndef ARDUINO
SerialClass Serial;
#endif

unsigned long mockMillis = 0;
unsigned long mockMicros = 0;

static bool responseContains(const WebServer& server, const char* needle) {
    return std::strstr(server.lastBody.c_str(), needle) != nullptr;
}

void setUp() {
    mockMillis = 1000;
    mockMicros = 1000000;
    mock_reset_heap_caps();
}

void tearDown() {}

struct FakeRuntime {
    bool enabled = false;
    String savedSsid;
    const char* stateName = "disabled";
    bool scanRunning = false;
    bool connected = false;
    WifiClientApiService::ConnectedNetworkPayload connectedNetwork;

    bool scanInProgress = false;
    bool hasCompletedResults = false;
    std::vector<WifiClientApiService::ScannedNetworkPayload> scannedNetworks;
    bool startScanReturn = false;

    int disconnectCalls = 0;
    int forgetClientCalls = 0;
    int enableWithSavedNetworkCalls = 0;
    bool enableWithSavedNetworkReturn = true;
    int disableClientCalls = 0;
    int startScanCalls = 0;

    std::vector<WifiClientApiService::SavedNetworkSlotPayload> savedNetworks;
    int getSavedNetworksCalls = 0;
    bool upsertSavedNetworkReturn = true;
    size_t upsertSavedNetworkIndexOut = 0;
    int upsertSavedNetworkCalls = 0;
    WifiClientApiService::SavedNetworkUpsertPayload lastUpsertRequest;
    bool deleteSavedNetworkReturn = true;
    int deleteSavedNetworkCalls = 0;
    size_t lastDeleteIndex = 99;
    bool testSavedNetworkReturn = true;
    int testSavedNetworkCalls = 0;
    size_t lastTestIndex = 99;
};

static WifiClientApiService::Runtime makeRuntime(FakeRuntime& rt) {
    return WifiClientApiService::Runtime{
        [](void* ctx) { return static_cast<FakeRuntime*>(ctx)->enabled; },
        &rt,
        [](void* ctx) { return static_cast<FakeRuntime*>(ctx)->savedSsid; },
        &rt,
        [](void* ctx) -> const char* { return static_cast<FakeRuntime*>(ctx)->stateName; },
        &rt,
        [](void* ctx) { return static_cast<FakeRuntime*>(ctx)->scanRunning; },
        &rt,
        [](void* ctx) { return static_cast<FakeRuntime*>(ctx)->connected; },
        &rt,
        [](void* ctx) { return static_cast<FakeRuntime*>(ctx)->connectedNetwork; },
        &rt,
        [](void* ctx) { return static_cast<FakeRuntime*>(ctx)->scanInProgress; },
        &rt,
        [](void* ctx) { return static_cast<FakeRuntime*>(ctx)->hasCompletedResults; },
        &rt,
        [](void* ctx) { return static_cast<FakeRuntime*>(ctx)->scannedNetworks; },
        &rt,
        [](void* ctx) {
            auto* r = static_cast<FakeRuntime*>(ctx);
            r->startScanCalls++;
            return r->startScanReturn;
        },
        &rt,
        [](void* ctx) { static_cast<FakeRuntime*>(ctx)->disconnectCalls++; },
        &rt,
        [](void* ctx) { static_cast<FakeRuntime*>(ctx)->forgetClientCalls++; },
        &rt,
        [](void* ctx) {
            auto* r = static_cast<FakeRuntime*>(ctx);
            r->enableWithSavedNetworkCalls++;
            if (r->enableWithSavedNetworkReturn) {
                r->enabled = true;
            }
            return r->enableWithSavedNetworkReturn;
        },
        &rt,
        [](void* ctx) { static_cast<FakeRuntime*>(ctx)->disableClientCalls++; },
        &rt,
    };
}

static WifiClientApiService::Runtime makeNetworkRuntime(FakeRuntime& rt) {
    WifiClientApiService::Runtime runtime = makeRuntime(rt);
    runtime.maintenanceBootActive = true;
    runtime.getSavedNetworks = [](void* ctx) {
        auto* r = static_cast<FakeRuntime*>(ctx);
        r->getSavedNetworksCalls++;
        return r->savedNetworks;
    };
    runtime.getSavedNetworksCtx = &rt;
    runtime.upsertSavedNetwork = [](const WifiClientApiService::SavedNetworkUpsertPayload& request, size_t& indexOut,
                                    void* ctx) {
        auto* r = static_cast<FakeRuntime*>(ctx);
        r->upsertSavedNetworkCalls++;
        r->lastUpsertRequest = request;
        indexOut = r->upsertSavedNetworkIndexOut;
        return r->upsertSavedNetworkReturn;
    };
    runtime.upsertSavedNetworkCtx = &rt;
    runtime.deleteSavedNetwork = [](size_t index, void* ctx) {
        auto* r = static_cast<FakeRuntime*>(ctx);
        r->deleteSavedNetworkCalls++;
        r->lastDeleteIndex = index;
        return r->deleteSavedNetworkReturn;
    };
    runtime.deleteSavedNetworkCtx = &rt;
    runtime.testSavedNetwork = [](size_t index, void* ctx) {
        auto* r = static_cast<FakeRuntime*>(ctx);
        r->testSavedNetworkCalls++;
        r->lastTestIndex = index;
        return r->testSavedNetworkReturn;
    };
    runtime.testSavedNetworkCtx = &rt;
    return runtime;
}

void test_handle_status_connected_includes_network_fields() {
    WebServer server(80);
    FakeRuntime rt;
    rt.enabled = true;
    rt.savedSsid = "SavedNet";
    rt.stateName = "connected";
    rt.scanRunning = false;
    rt.connected = true;
    rt.connectedNetwork.ssid = "LiveNet";
    rt.connectedNetwork.connectedSlotIndex = 2;
    rt.connectedNetwork.ip = "192.168.1.42";
    rt.connectedNetwork.rssi = -61;

    WifiClientApiService::handleApiStatus(server, makeRuntime(rt), nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"enabled\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"savedSSID\":\"SavedNet\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"state\":\"connected\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"connectedSSID\":\"LiveNet\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"connectedSlotIndex\":2"));
    TEST_ASSERT_TRUE(responseContains(server, "\"ip\":\"192.168.1.42\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"rssi\":-61"));
    TEST_ASSERT_TRUE(responseContains(server, "\"scanRunning\":false"));
}

void test_handle_status_disconnected_omits_connected_fields() {
    WebServer server(80);
    FakeRuntime rt;
    rt.enabled = false;
    rt.savedSsid = "";
    rt.stateName = "disabled";
    rt.scanRunning = true;
    rt.connected = false;

    WifiClientApiService::handleApiStatus(server, makeRuntime(rt), nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"state\":\"disabled\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"scanRunning\":true"));
    TEST_ASSERT_FALSE(responseContains(server, "\"connectedSSID\""));
    TEST_ASSERT_FALSE(responseContains(server, "\"connectedSlotIndex\""));
    TEST_ASSERT_FALSE(responseContains(server, "\"ip\""));
}

void test_handle_status_repeated_requests_release_wifi_json_allocations() {
    WebServer server(80);
    FakeRuntime rt;
    rt.enabled = true;
    rt.savedSsid = "SavedNet";
    rt.stateName = "connected";
    rt.connected = true;
    rt.connectedNetwork.ssid = "LiveNet";
    rt.connectedNetwork.ip = "192.168.4.10";
    rt.connectedNetwork.rssi = -55;

    mock_reset_heap_caps_tracking();

    for (int i = 0; i < 5; ++i) {
        WifiClientApiService::handleApiStatus(server, makeRuntime(rt), nullptr, nullptr);
    }

    TEST_ASSERT_GREATER_THAN_UINT32(0u, g_mock_heap_caps_malloc_calls);
    TEST_ASSERT_EQUAL_UINT32(WifiJson::kPsramCaps, g_mock_heap_caps_last_malloc_caps);
    TEST_ASSERT_GREATER_THAN_UINT32(0u, g_mock_heap_caps_free_calls);
    TEST_ASSERT_EQUAL_UINT32(0u, g_mock_heap_caps_outstanding_allocations);
}

void test_handle_scan_completed_includes_networks() {
    WebServer server(80);
    FakeRuntime rt;
    rt.scanRunning = true;
    rt.scanInProgress = false;
    rt.hasCompletedResults = true;

    WifiClientApiService::ScannedNetworkPayload first;
    first.ssid = "OpenNet";
    first.rssi = -42;
    first.secure = false;
    rt.scannedNetworks.push_back(first);

    WifiClientApiService::ScannedNetworkPayload second;
    second.ssid = "SecureNet";
    second.rssi = -70;
    second.secure = true;
    rt.scannedNetworks.push_back(second);

    WifiClientApiService::handleApiScan(server, makeRuntime(rt), nullptr, nullptr, nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"scanning\":false"));
    TEST_ASSERT_TRUE(responseContains(server, "\"ssid\":\"OpenNet\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"secure\":false"));
    TEST_ASSERT_TRUE(responseContains(server, "\"ssid\":\"SecureNet\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"secure\":true"));
}

void test_handle_enable_rejects_non_boolean_enabled() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("plain", "{\"enabled\":\"true\"}");

    mock_reset_heap_caps_tracking();

    WifiClientApiService::handleApiEnable(server, makeRuntime(rt), nullptr, nullptr, nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"success\":false"));
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"Missing enabled field\""));
    TEST_ASSERT_EQUAL_INT(0, rt.enableWithSavedNetworkCalls);
    TEST_ASSERT_EQUAL_INT(0, rt.disableClientCalls);
    TEST_ASSERT_GREATER_THAN_UINT32(0u, g_mock_heap_caps_malloc_calls);
    TEST_ASSERT_EQUAL_UINT32(WifiJson::kPsramCaps, g_mock_heap_caps_last_malloc_caps);
    TEST_ASSERT_EQUAL_UINT32(0u, g_mock_heap_caps_outstanding_allocations);
}

void test_handle_enable_accepts_boolean_enabled() {
    WebServer server(80);
    FakeRuntime rt;
    rt.savedSsid = "";
    server.setArg("plain", "{\"enabled\":true}");

    WifiClientApiService::handleApiEnable(server, makeRuntime(rt), nullptr, nullptr, nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"success\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"WiFi client enabled\""));
    TEST_ASSERT_EQUAL_INT(1, rt.enableWithSavedNetworkCalls);
    TEST_ASSERT_EQUAL_INT(0, rt.disableClientCalls);
}

void test_handle_enable_missing_field_uses_expected_payload() {
    WebServer server(80);
    FakeRuntime rt;

    WifiClientApiService::handleApiEnable(server, makeRuntime(rt), nullptr, nullptr, nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"success\":false"));
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"Missing enabled field\""));
    TEST_ASSERT_EQUAL_INT(0, rt.enableWithSavedNetworkCalls);
    TEST_ASSERT_EQUAL_INT(0, rt.disableClientCalls);
}

void test_handle_status_connected_uses_runtime_payload() {
    WebServer server(80);
    FakeRuntime rt;
    rt.enabled = true;
    rt.savedSsid = "SavedNet";
    rt.stateName = "connected";
    rt.scanRunning = false;
    rt.connected = true;
    rt.connectedNetwork.ssid = "LiveNet";
    rt.connectedNetwork.ip = "192.168.4.10";
    rt.connectedNetwork.rssi = -55;

    WifiClientApiService::handleApiStatus(server, makeRuntime(rt), nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"enabled\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"savedSSID\":\"SavedNet\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"state\":\"connected\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"connectedSSID\":\"LiveNet\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"ip\":\"192.168.4.10\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"rssi\":-55"));
    TEST_ASSERT_TRUE(responseContains(server, "\"scanRunning\":false"));
}

void test_handle_scan_in_progress_returns_scanning_true() {
    WebServer server(80);
    FakeRuntime rt;
    rt.scanRunning = true;
    rt.scanInProgress = true;

    WifiClientApiService::handleApiScan(server, makeRuntime(rt), nullptr, nullptr, nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"scanning\":true"));
    TEST_ASSERT_EQUAL_INT(0, rt.startScanCalls);
}

void test_handle_scan_completed_returns_networks() {
    WebServer server(80);
    FakeRuntime rt;
    rt.scanRunning = true;
    rt.scanInProgress = false;
    rt.hasCompletedResults = true;

    WifiClientApiService::ScannedNetworkPayload net;
    net.ssid = "Garage";
    net.rssi = -48;
    net.secure = true;
    rt.scannedNetworks.push_back(net);

    WifiClientApiService::handleApiScan(server, makeRuntime(rt), nullptr, nullptr, nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"scanning\":false"));
    TEST_ASSERT_TRUE(responseContains(server, "\"ssid\":\"Garage\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"secure\":true"));
    TEST_ASSERT_EQUAL_INT(0, rt.startScanCalls);
}

void test_handle_scan_starts_new_scan_when_no_results() {
    WebServer server(80);
    FakeRuntime rt;
    rt.scanRunning = false;
    rt.scanInProgress = false;
    rt.hasCompletedResults = false;
    rt.startScanReturn = true;

    WifiClientApiService::handleApiScan(server, makeRuntime(rt), nullptr, nullptr, nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"scanning\":true"));
    TEST_ASSERT_EQUAL_INT(1, rt.startScanCalls);
}

void test_handle_scan_post_requests_fresh_generation_before_serving_cached_results() {
    WebServer server(80);
    FakeRuntime rt;
    rt.scanRunning = false;
    rt.scanInProgress = false;
    rt.hasCompletedResults = true;
    rt.startScanReturn = true;

    WifiClientApiService::ScannedNetworkPayload stale;
    stale.ssid = "StaleNetwork";
    stale.rssi = -80;
    stale.secure = true;
    rt.scannedNetworks.push_back(stale);

    WifiClientApiService::handleApiScan(server, makeRuntime(rt), nullptr, nullptr, nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"scanning\":true"));
    TEST_ASSERT_FALSE(responseContains(server, "StaleNetwork"));
    TEST_ASSERT_EQUAL_INT(1, rt.startScanCalls);
}

void test_handle_scan_status_does_not_start_new_scan() {
    WebServer server(80);
    FakeRuntime rt;
    rt.scanRunning = false;
    rt.scanInProgress = false;
    rt.hasCompletedResults = false;

    WifiClientApiService::handleApiScanStatus(server, makeRuntime(rt), nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"scanning\":false"));
    TEST_ASSERT_TRUE(responseContains(server, "\"networks\":[]"));
    TEST_ASSERT_EQUAL_INT(0, rt.startScanCalls);
}

void test_handle_networks_requires_maintenance_mode() {
    WebServer server(80);
    FakeRuntime rt;
    WifiClientApiService::Runtime runtime = makeNetworkRuntime(rt);
    runtime.maintenanceBootActive = false;

    WifiClientApiService::handleApiNetworks(server, runtime, nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(409, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"maintenance_required\""));
    TEST_ASSERT_EQUAL_INT(0, rt.getSavedNetworksCalls);
}

void test_handle_networks_returns_saved_slots_without_passwords() {
    WebServer server(80);
    FakeRuntime rt;
    WifiClientApiService::SavedNetworkSlotPayload slot;
    slot.index = 1;
    slot.ssid = "GarageWiFi";
    slot.label = "Garage";
    slot.priority = 2;
    slot.lastConnectedAtSec = 123;
    slot.configured = true;
    slot.hasPassword = true;
    rt.savedNetworks.push_back(slot);

    WifiClientApiService::handleApiNetworks(server, makeNetworkRuntime(rt), nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"slots\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"index\":1"));
    TEST_ASSERT_TRUE(responseContains(server, "\"ssid\":\"GarageWiFi\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"label\":\"Garage\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"priority\":2"));
    TEST_ASSERT_TRUE(responseContains(server, "\"lastConnectedAtSec\":123"));
    TEST_ASSERT_TRUE(responseContains(server, "\"configured\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"hasPassword\":true"));
    TEST_ASSERT_FALSE(responseContains(server, "secret"));
    TEST_ASSERT_EQUAL_INT(1, rt.getSavedNetworksCalls);
}

void test_handle_networks_save_upserts_manual_network() {
    WebServer server(80);
    FakeRuntime rt;
    rt.upsertSavedNetworkIndexOut = 3;
    server.setArg("plain", "{\"index\":3,\"ssid\":\"PhoneHotspot\",\"password\":\"secret\","
                           "\"label\":\"Phone\",\"priority\":1}");

    WifiClientApiService::handleApiNetworksSave(server, makeNetworkRuntime(rt), nullptr, nullptr, nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"success\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"index\":3"));
    TEST_ASSERT_EQUAL_INT(1, rt.upsertSavedNetworkCalls);
    TEST_ASSERT_TRUE(rt.lastUpsertRequest.hasIndex);
    TEST_ASSERT_EQUAL_UINT32(3u, rt.lastUpsertRequest.index);
    TEST_ASSERT_EQUAL_STRING("PhoneHotspot", rt.lastUpsertRequest.ssid.c_str());
    TEST_ASSERT_TRUE(rt.lastUpsertRequest.hasPassword);
    TEST_ASSERT_EQUAL_STRING("secret", rt.lastUpsertRequest.password.c_str());
    TEST_ASSERT_TRUE(rt.lastUpsertRequest.hasLabel);
    TEST_ASSERT_EQUAL_STRING("Phone", rt.lastUpsertRequest.label.c_str());
    TEST_ASSERT_TRUE(rt.lastUpsertRequest.hasPriority);
    TEST_ASSERT_EQUAL_UINT8(1, rt.lastUpsertRequest.priority);
}

void test_handle_networks_save_requires_maintenance_mode() {
    WebServer server(80);
    FakeRuntime rt;
    WifiClientApiService::Runtime runtime = makeNetworkRuntime(rt);
    runtime.maintenanceBootActive = false;
    server.setArg("plain", "{\"ssid\":\"GarageWiFi\"}");

    WifiClientApiService::handleApiNetworksSave(server, runtime, nullptr, nullptr, nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(409, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"maintenance_required\""));
    TEST_ASSERT_EQUAL_INT(0, rt.upsertSavedNetworkCalls);
}

void test_handle_networks_save_omitted_password_preserves_existing_secret() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("plain", "{\"index\":0,\"ssid\":\"GarageWiFi\",\"label\":\"Garage\"}");

    WifiClientApiService::handleApiNetworksSave(server, makeNetworkRuntime(rt), nullptr, nullptr, nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(1, rt.upsertSavedNetworkCalls);
    TEST_ASSERT_FALSE(rt.lastUpsertRequest.hasPassword);
    TEST_ASSERT_TRUE(rt.lastUpsertRequest.hasLabel);
    TEST_ASSERT_EQUAL_STRING("Garage", rt.lastUpsertRequest.label.c_str());
}

void test_handle_networks_save_rejects_missing_ssid() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("plain", "{\"label\":\"Garage\"}");

    WifiClientApiService::handleApiNetworksSave(server, makeNetworkRuntime(rt), nullptr, nullptr, nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"message\":\"SSID required\""));
    TEST_ASSERT_EQUAL_INT(0, rt.upsertSavedNetworkCalls);
}

void test_handle_networks_delete_deletes_slot() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("plain", "{\"index\":2}");

    WifiClientApiService::handleApiNetworksDelete(server, makeNetworkRuntime(rt), nullptr, nullptr, nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"success\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"index\":2"));
    TEST_ASSERT_EQUAL_INT(1, rt.deleteSavedNetworkCalls);
    TEST_ASSERT_EQUAL_UINT32(2u, rt.lastDeleteIndex);
}

void test_handle_networks_delete_requires_maintenance_mode() {
    WebServer server(80);
    FakeRuntime rt;
    WifiClientApiService::Runtime runtime = makeNetworkRuntime(rt);
    runtime.maintenanceBootActive = false;
    server.setArg("plain", "{\"index\":2}");

    WifiClientApiService::handleApiNetworksDelete(server, runtime, nullptr, nullptr, nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(409, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"maintenance_required\""));
    TEST_ASSERT_EQUAL_INT(0, rt.deleteSavedNetworkCalls);
}

void test_handle_networks_test_starts_slot_connection() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("plain", "{\"index\":1}");

    WifiClientApiService::handleApiNetworksTest(server, makeNetworkRuntime(rt), nullptr, nullptr, nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"success\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"index\":1"));
    TEST_ASSERT_EQUAL_INT(1, rt.testSavedNetworkCalls);
    TEST_ASSERT_EQUAL_UINT32(1u, rt.lastTestIndex);
}

void test_handle_networks_test_requires_maintenance_mode() {
    WebServer server(80);
    FakeRuntime rt;
    WifiClientApiService::Runtime runtime = makeNetworkRuntime(rt);
    runtime.maintenanceBootActive = false;
    server.setArg("plain", "{\"index\":1}");

    WifiClientApiService::handleApiNetworksTest(server, runtime, nullptr, nullptr, nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(409, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"maintenance_required\""));
    TEST_ASSERT_EQUAL_INT(0, rt.testSavedNetworkCalls);
}

void test_handle_forget_clears_credentials_and_disables_sta() {
    WebServer server(80);
    FakeRuntime rt;

    WifiClientApiService::handleApiForget(server, makeRuntime(rt), nullptr, nullptr, nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"success\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"WiFi credentials forgotten\""));
    TEST_ASSERT_EQUAL_INT(0, rt.disconnectCalls);
    TEST_ASSERT_EQUAL_INT(1, rt.forgetClientCalls);
}

void test_handle_enable_true_with_saved_credentials_starts_connect() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("plain", "{\"enabled\":true}");

    WifiClientApiService::handleApiEnable(server, makeRuntime(rt), nullptr, nullptr, nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"WiFi client enabled\""));
    TEST_ASSERT_EQUAL_INT(1, rt.enableWithSavedNetworkCalls);
    TEST_ASSERT_EQUAL_INT(0, rt.disableClientCalls);
}

void test_handle_enable_true_with_saved_credentials_returns_500_when_connect_fails() {
    WebServer server(80);
    FakeRuntime rt;
    rt.enableWithSavedNetworkReturn = false;
    server.setArg("plain", "{\"enabled\":true}");

    WifiClientApiService::handleApiEnable(server, makeRuntime(rt), nullptr, nullptr, nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(500, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"success\":false"));
    TEST_ASSERT_TRUE(responseContains(server, "\"message\":\"Failed to start connection\""));
    TEST_ASSERT_EQUAL_INT(1, rt.enableWithSavedNetworkCalls);
    TEST_ASSERT_EQUAL_INT(0, rt.disableClientCalls);
    TEST_ASSERT_FALSE(rt.enabled);
}

void test_handle_enable_true_without_saved_credentials_sets_disconnected_state() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("plain", "{\"enabled\":true}");

    WifiClientApiService::handleApiEnable(server, makeRuntime(rt), nullptr, nullptr, nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"WiFi client enabled\""));
    TEST_ASSERT_EQUAL_INT(1, rt.enableWithSavedNetworkCalls);
    TEST_ASSERT_EQUAL_INT(0, rt.disableClientCalls);
}

void test_handle_enable_false_disables_sta_mode() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("plain", "{\"enabled\":false}");

    WifiClientApiService::handleApiEnable(server, makeRuntime(rt), nullptr, nullptr, nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"WiFi client disabled\""));
    TEST_ASSERT_EQUAL_INT(0, rt.disconnectCalls);
    TEST_ASSERT_EQUAL_INT(0, rt.enableWithSavedNetworkCalls);
    TEST_ASSERT_EQUAL_INT(1, rt.disableClientCalls);
}

void test_handle_api_status_marks_ui_activity_and_delegates() {
    WebServer server(80);
    FakeRuntime rt;
    rt.enabled = true;
    rt.stateName = "connected";
    rt.connected = false;
    int uiActivityCalls = 0;

    WifiClientApiService::handleApiStatus(
        server, makeRuntime(rt), [](void* ctx) { (*static_cast<int*>(ctx))++; }, &uiActivityCalls);

    TEST_ASSERT_EQUAL_INT(1, uiActivityCalls);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"state\":\"connected\""));
}

void test_handle_api_scan_rate_limited_short_circuits() {
    WebServer server(80);
    FakeRuntime rt;
    int rateLimitCalls = 0;
    int uiActivityCalls = 0;

    struct Ctx {
        int* rl;
        int* ui;
    };
    Ctx ctx{&rateLimitCalls, &uiActivityCalls};

    WifiClientApiService::handleApiScan(
        server, makeRuntime(rt),
        [](void* c) {
            (*static_cast<Ctx*>(c)->rl)++;
            return false;
        },
        &ctx, [](void* c) { (*static_cast<Ctx*>(c)->ui)++; }, &ctx);

    TEST_ASSERT_EQUAL_INT(1, rateLimitCalls);
    TEST_ASSERT_EQUAL_INT(0, uiActivityCalls);
    TEST_ASSERT_EQUAL_INT(0, rt.startScanCalls);
    TEST_ASSERT_EQUAL_INT(0, server.lastStatusCode);
}

void test_handle_api_disconnect_delegates_when_allowed() {
    WebServer server(80);
    FakeRuntime rt;
    int rateLimitCalls = 0;
    int uiActivityCalls = 0;

    struct Ctx {
        int* rl;
        int* ui;
    };
    Ctx ctx{&rateLimitCalls, &uiActivityCalls};

    WifiClientApiService::handleApiDisconnect(
        server, makeRuntime(rt),
        [](void* c) {
            (*static_cast<Ctx*>(c)->rl)++;
            return true;
        },
        &ctx, [](void* c) { (*static_cast<Ctx*>(c)->ui)++; }, &ctx);

    TEST_ASSERT_EQUAL_INT(1, rateLimitCalls);
    TEST_ASSERT_EQUAL_INT(1, uiActivityCalls);
    TEST_ASSERT_EQUAL_INT(1, rt.disconnectCalls);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
}

void test_handle_api_forget_delegates_when_allowed() {
    WebServer server(80);
    FakeRuntime rt;
    int rateLimitCalls = 0;
    int uiActivityCalls = 0;

    struct Ctx {
        int* rl;
        int* ui;
    };
    Ctx ctx{&rateLimitCalls, &uiActivityCalls};

    WifiClientApiService::handleApiForget(
        server, makeRuntime(rt),
        [](void* c) {
            (*static_cast<Ctx*>(c)->rl)++;
            return true;
        },
        &ctx, [](void* c) { (*static_cast<Ctx*>(c)->ui)++; }, &ctx);

    TEST_ASSERT_EQUAL_INT(1, rateLimitCalls);
    TEST_ASSERT_EQUAL_INT(1, uiActivityCalls);
    TEST_ASSERT_EQUAL_INT(0, rt.disconnectCalls);
    TEST_ASSERT_EQUAL_INT(1, rt.forgetClientCalls);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
}

void test_handle_api_enable_delegates_when_allowed() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("plain", "{\"enabled\":false}");
    int rateLimitCalls = 0;
    int uiActivityCalls = 0;

    struct Ctx {
        int* rl;
        int* ui;
    };
    Ctx ctx{&rateLimitCalls, &uiActivityCalls};

    WifiClientApiService::handleApiEnable(
        server, makeRuntime(rt),
        [](void* c) {
            (*static_cast<Ctx*>(c)->rl)++;
            return true;
        },
        &ctx, [](void* c) { (*static_cast<Ctx*>(c)->ui)++; }, &ctx);

    TEST_ASSERT_EQUAL_INT(1, rateLimitCalls);
    TEST_ASSERT_EQUAL_INT(1, uiActivityCalls);
    TEST_ASSERT_EQUAL_INT(1, rt.disableClientCalls);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_handle_status_connected_includes_network_fields);
    RUN_TEST(test_handle_status_disconnected_omits_connected_fields);
    RUN_TEST(test_handle_status_repeated_requests_release_wifi_json_allocations);
    RUN_TEST(test_handle_scan_completed_includes_networks);
    RUN_TEST(test_handle_enable_rejects_non_boolean_enabled);
    RUN_TEST(test_handle_enable_accepts_boolean_enabled);
    RUN_TEST(test_handle_enable_missing_field_uses_expected_payload);
    RUN_TEST(test_handle_status_connected_uses_runtime_payload);
    RUN_TEST(test_handle_scan_in_progress_returns_scanning_true);
    RUN_TEST(test_handle_scan_completed_returns_networks);
    RUN_TEST(test_handle_scan_starts_new_scan_when_no_results);
    RUN_TEST(test_handle_scan_post_requests_fresh_generation_before_serving_cached_results);
    RUN_TEST(test_handle_scan_status_does_not_start_new_scan);
    RUN_TEST(test_handle_networks_requires_maintenance_mode);
    RUN_TEST(test_handle_networks_returns_saved_slots_without_passwords);
    RUN_TEST(test_handle_networks_save_upserts_manual_network);
    RUN_TEST(test_handle_networks_save_requires_maintenance_mode);
    RUN_TEST(test_handle_networks_save_omitted_password_preserves_existing_secret);
    RUN_TEST(test_handle_networks_save_rejects_missing_ssid);
    RUN_TEST(test_handle_networks_delete_deletes_slot);
    RUN_TEST(test_handle_networks_delete_requires_maintenance_mode);
    RUN_TEST(test_handle_networks_test_starts_slot_connection);
    RUN_TEST(test_handle_networks_test_requires_maintenance_mode);
    RUN_TEST(test_handle_forget_clears_credentials_and_disables_sta);
    RUN_TEST(test_handle_enable_true_with_saved_credentials_starts_connect);
    RUN_TEST(test_handle_enable_true_with_saved_credentials_returns_500_when_connect_fails);
    RUN_TEST(test_handle_enable_true_without_saved_credentials_sets_disconnected_state);
    RUN_TEST(test_handle_enable_false_disables_sta_mode);
    RUN_TEST(test_handle_api_status_marks_ui_activity_and_delegates);
    RUN_TEST(test_handle_api_scan_rate_limited_short_circuits);
    RUN_TEST(test_handle_api_disconnect_delegates_when_allowed);
    RUN_TEST(test_handle_api_forget_delegates_when_allowed);
    RUN_TEST(test_handle_api_enable_delegates_when_allowed);
    return UNITY_END();
}

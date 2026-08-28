#include <unity.h>

#include "../mocks/Arduino.h"
#include "../mocks/WebServer.h"
#include "../../src/modules/wifi/wifi_client_api_service.h"
#include "../../src/modules/wifi/wifi_client_api_service.cpp"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

namespace {

struct Probe {
    bool forgetResult = true;
    bool enableResult = true;
    bool disableResult = true;
    int forgetCalls = 0;
    int enableCalls = 0;
    int disableCalls = 0;
    int testCalls = 0;
    size_t testedIndex = 0;
    bool testResult = true;
    int upsertCalls = 0;
    bool upsertResult = true;
    int deleteCalls = 0;
    bool deleteResult = true;
    int priorityCalls = 0;
    WifiClientApiService::PriorityUpdateStatus priorityResult =
        WifiClientApiService::PriorityUpdateStatus::Success;
    std::vector<WifiClientApiService::SavedNetworkPriorityUpdate> updates;
};

WifiClientApiService::Runtime makeRuntime(Probe& probe) {
    WifiClientApiService::Runtime runtime{};
    runtime.maintenanceBootActive = true;
    runtime.forgetClient = [](void* ctx) {
        auto* state = static_cast<Probe*>(ctx);
        state->forgetCalls++;
        return state->forgetResult;
    };
    runtime.forgetClientCtx = &probe;
    runtime.enableWithSavedNetwork = [](void* ctx) {
        auto* state = static_cast<Probe*>(ctx);
        state->enableCalls++;
        return state->enableResult;
    };
    runtime.enableWithSavedNetworkCtx = &probe;
    runtime.disableClient = [](void* ctx) {
        auto* state = static_cast<Probe*>(ctx);
        state->disableCalls++;
        return state->disableResult;
    };
    runtime.disableClientCtx = &probe;
    runtime.testSavedNetwork = [](size_t index, void* ctx) {
        auto* state = static_cast<Probe*>(ctx);
        state->testCalls++;
        state->testedIndex = index;
        return state->testResult;
    };
    runtime.testSavedNetworkCtx = &probe;
    runtime.upsertSavedNetwork = [](const WifiClientApiService::SavedNetworkUpsertPayload&, size_t& indexOut,
                                    void* ctx) {
        auto* state = static_cast<Probe*>(ctx);
        state->upsertCalls++;
        indexOut = 0;
        return state->upsertResult;
    };
    runtime.upsertSavedNetworkCtx = &probe;
    runtime.deleteSavedNetwork = [](size_t index, void* ctx) {
        auto* state = static_cast<Probe*>(ctx);
        state->deleteCalls++;
        state->testedIndex = index;
        return state->deleteResult;
    };
    runtime.deleteSavedNetworkCtx = &probe;
    runtime.updateSavedNetworkPriorities =
        [](const std::vector<WifiClientApiService::SavedNetworkPriorityUpdate>& updates, void* ctx) {
            auto* state = static_cast<Probe*>(ctx);
            state->priorityCalls++;
            state->updates = updates;
            return state->priorityResult;
        };
    runtime.updateSavedNetworkPrioritiesCtx = &probe;
    return runtime;
}

bool allow(void*) {
    return true;
}

bool contains(const String& body, const char* value) {
    return body.indexOf(value) >= 0;
}

} // namespace

void setUp() {}
void tearDown() {}

void test_forget_persistence_failure_is_not_reported_as_success() {
    WebServer server(80);
    Probe probe;
    probe.forgetResult = false;

    WifiClientApiService::handleApiForget(server, makeRuntime(probe), allow, nullptr, nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(500, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(1, probe.forgetCalls);
    TEST_ASSERT_TRUE(contains(server.lastBody, "settings_persist_failed"));
}

void test_disable_persistence_failure_is_not_reported_as_success() {
    WebServer server(80);
    Probe probe;
    probe.disableResult = false;
    server.setArg("plain", "{\"enabled\":false}");

    WifiClientApiService::handleApiEnable(server, makeRuntime(probe), allow, nullptr, nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(500, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(1, probe.disableCalls);
    TEST_ASSERT_EQUAL_INT(0, probe.enableCalls);
    TEST_ASSERT_TRUE(contains(server.lastBody, "settings_persist_failed"));
}

void test_enable_failure_is_not_reported_as_success() {
    WebServer server(80);
    Probe probe;
    probe.enableResult = false;
    server.setArg("plain", "{\"enabled\":true}");

    WifiClientApiService::handleApiEnable(server, makeRuntime(probe), allow, nullptr, nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(500, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(1, probe.enableCalls);
    TEST_ASSERT_EQUAL_INT(0, probe.disableCalls);
}

void test_maintenance_enable_starts_now_without_deferred_boot_message() {
    WebServer server(80);
    Probe probe;
    server.setArg("plain", "{\"enabled\":true}");

    WifiClientApiService::handleApiEnable(server, makeRuntime(probe), allow, nullptr, nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(1, probe.enableCalls);
    TEST_ASSERT_TRUE(contains(server.lastBody, "WiFi client enabled"));
    TEST_ASSERT_FALSE(contains(server.lastBody, "normal boot"));
}

void test_maintenance_saved_network_test_starts_live_connection() {
    WebServer server(80);
    Probe probe;
    server.setArg("plain", "{\"index\":0}");

    WifiClientApiService::handleApiNetworksTest(server, makeRuntime(probe), allow, nullptr, nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(1, probe.testCalls);
    TEST_ASSERT_EQUAL_UINT(0u, probe.testedIndex);
    TEST_ASSERT_TRUE(contains(server.lastBody, "Connecting"));
    TEST_ASSERT_FALSE(contains(server.lastBody, "ap_only_maintenance"));
}

void test_saved_network_upsert_failure_is_not_reported_as_success() {
    WebServer server(80);
    Probe probe;
    probe.upsertResult = false;
    server.setArg("plain", "{\"ssid\":\"Garage\",\"password\":\"password-one\"}");

    WifiClientApiService::handleApiNetworksSave(server, makeRuntime(probe), allow, nullptr, nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(409, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(1, probe.upsertCalls);
    TEST_ASSERT_TRUE(contains(server.lastBody, "network_save_failed"));
}

void test_saved_network_delete_failure_is_not_reported_as_success() {
    WebServer server(80);
    Probe probe;
    probe.deleteResult = false;
    server.setArg("plain", "{\"index\":2}");

    WifiClientApiService::handleApiNetworksDelete(server, makeRuntime(probe), allow, nullptr, nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(404, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(1, probe.deleteCalls);
    TEST_ASSERT_EQUAL_UINT(2u, probe.testedIndex);
    TEST_ASSERT_TRUE(contains(server.lastBody, "network_delete_failed"));
}

void test_saved_network_connect_failure_is_not_reported_as_success() {
    WebServer server(80);
    Probe probe;
    probe.testResult = false;
    server.setArg("plain", "{\"index\":1}");

    WifiClientApiService::handleApiNetworksTest(server, makeRuntime(probe), allow, nullptr, nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(404, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(1, probe.testCalls);
    TEST_ASSERT_EQUAL_UINT(1u, probe.testedIndex);
    TEST_ASSERT_TRUE(contains(server.lastBody, "network_test_failed"));
}

void test_priority_batch_validates_all_entries_before_callback() {
    WebServer server(80);
    Probe probe;
    server.setArg("plain", "{\"updates\":[{\"index\":0,\"priority\":1},{\"index\":0,\"priority\":2}]}");

    WifiClientApiService::handleApiNetworksPriorities(
        server, makeRuntime(probe), allow, nullptr, nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(0, probe.priorityCalls);
}

void test_priority_batch_maps_persist_failure_to_500() {
    WebServer server(80);
    Probe probe;
    probe.priorityResult = WifiClientApiService::PriorityUpdateStatus::PersistFailed;
    server.setArg("plain", "{\"updates\":[{\"index\":0,\"priority\":1},{\"index\":1,\"priority\":0}]}");

    WifiClientApiService::handleApiNetworksPriorities(
        server, makeRuntime(probe), allow, nullptr, nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(500, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(1, probe.priorityCalls);
    TEST_ASSERT_EQUAL_UINT(2u, probe.updates.size());
    TEST_ASSERT_TRUE(contains(server.lastBody, "settings_persist_failed"));
}

void test_priority_batch_success_returns_200_after_one_callback() {
    WebServer server(80);
    Probe probe;
    server.setArg("plain", "{\"updates\":[{\"index\":0,\"priority\":1},{\"index\":1,\"priority\":0}]}");

    WifiClientApiService::handleApiNetworksPriorities(
        server, makeRuntime(probe), allow, nullptr, nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(1, probe.priorityCalls);
    TEST_ASSERT_EQUAL_UINT(2u, probe.updates.size());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_forget_persistence_failure_is_not_reported_as_success);
    RUN_TEST(test_disable_persistence_failure_is_not_reported_as_success);
    RUN_TEST(test_enable_failure_is_not_reported_as_success);
    RUN_TEST(test_maintenance_enable_starts_now_without_deferred_boot_message);
    RUN_TEST(test_maintenance_saved_network_test_starts_live_connection);
    RUN_TEST(test_saved_network_upsert_failure_is_not_reported_as_success);
    RUN_TEST(test_saved_network_delete_failure_is_not_reported_as_success);
    RUN_TEST(test_saved_network_connect_failure_is_not_reported_as_success);
    RUN_TEST(test_priority_batch_validates_all_entries_before_callback);
    RUN_TEST(test_priority_batch_maps_persist_failure_to_500);
    RUN_TEST(test_priority_batch_success_returns_200_after_one_callback);
    return UNITY_END();
}

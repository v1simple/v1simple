#include <unity.h>

#include "../mocks/Arduino.h"
#include "../mocks/WebServer.h"
#include "../mocks/mock_heap_caps_state.h"
#include "../../src/modules/wifi/wifi_client_api_service.h"
#include "../../src/modules/wifi/wifi_client_api_service.cpp"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

namespace {

using WifiClientApiService::SlotActionStatus;

struct Probe {
    bool forgetResult = true;
    bool enableResult = true;
    bool disableResult = true;
    int forgetCalls = 0;
    int enableCalls = 0;
    int disableCalls = 0;
    int testCalls = 0;
    WifiClientApiService::SavedNetworkSlotRequest slotRequest;
    SlotActionStatus testResult = SlotActionStatus::Success;
    int upsertCalls = 0;
    SlotActionStatus upsertResult = SlotActionStatus::Success;
    WifiClientApiService::SavedNetworkUpsertPayload upsertRequest;
    int deleteCalls = 0;
    SlotActionStatus deleteResult = SlotActionStatus::Success;
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
    runtime.testSavedNetwork = [](const WifiClientApiService::SavedNetworkSlotRequest& request, void* ctx) {
        auto* state = static_cast<Probe*>(ctx);
        state->testCalls++;
        state->slotRequest = request;
        return state->testResult;
    };
    runtime.testSavedNetworkCtx = &probe;
    runtime.upsertSavedNetwork = [](const WifiClientApiService::SavedNetworkUpsertPayload& request, size_t& indexOut,
                                    void* ctx) {
        auto* state = static_cast<Probe*>(ctx);
        state->upsertCalls++;
        state->upsertRequest = request;
        indexOut = 0;
        return state->upsertResult;
    };
    runtime.upsertSavedNetworkCtx = &probe;
    runtime.deleteSavedNetwork = [](const WifiClientApiService::SavedNetworkSlotRequest& request, void* ctx) {
        auto* state = static_cast<Probe*>(ctx);
        state->deleteCalls++;
        state->slotRequest = request;
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

void setUp() {
    mock_reset_heap_caps();
    g_mock_heap_caps_free_observer = nullptr;
}
void tearDown() {
    g_mock_heap_caps_free_observer = nullptr;
    mock_reset_heap_caps();
}

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
    server.setArg("plain", R"({"index":0,"expectedSsid":" Garage "})");

    WifiClientApiService::handleApiNetworksTest(server, makeRuntime(probe), allow, nullptr, nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(1, probe.testCalls);
    TEST_ASSERT_EQUAL_UINT(0u, probe.slotRequest.index);
    TEST_ASSERT_EQUAL_STRING(" Garage ", probe.slotRequest.expectedSsid.c_str());
    TEST_ASSERT_TRUE(contains(server.lastBody, "Connecting"));
    TEST_ASSERT_FALSE(contains(server.lastBody, "ap_only_maintenance"));
}

void test_saved_network_upsert_failure_is_not_reported_as_success() {
    WebServer server(80);
    Probe probe;
    probe.upsertResult = SlotActionStatus::Failed;
    server.setArg("plain", "{\"ssid\":\"Garage\",\"password\":\"password-one\"}");

    WifiClientApiService::handleApiNetworksSave(server, makeRuntime(probe), allow, nullptr, nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(409, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(1, probe.upsertCalls);
    TEST_ASSERT_TRUE(contains(server.lastBody, "network_save_failed"));
}

void test_network_save_requires_exact_identity_for_an_indexed_request() {
    Probe probe;
    const auto runtime = makeRuntime(probe);
    WebServer edit(80);
    edit.setArg("plain", R"({"index":0,"expectedSsid":" Old ","ssid":"New","label":"New label"})");
    WifiClientApiService::handleApiNetworksSave(edit, runtime, allow, nullptr, nullptr, nullptr);
    TEST_ASSERT_EQUAL_INT(200, edit.lastStatusCode);
    TEST_ASSERT_TRUE(probe.upsertRequest.hasExpectedSsid);
    TEST_ASSERT_EQUAL_STRING(" Old ", probe.upsertRequest.expectedSsid.c_str());
    TEST_ASSERT_FALSE(probe.upsertRequest.hasPriority);

    WebServer add(80);
    add.setArg("plain", R"({"index":1,"expectedSsid":"","ssid":"New","priority":0})");
    WifiClientApiService::handleApiNetworksSave(add, runtime, allow, nullptr, nullptr, nullptr);
    TEST_ASSERT_EQUAL_INT(200, add.lastStatusCode);
    TEST_ASSERT_TRUE(probe.upsertRequest.hasExpectedSsid);
    TEST_ASSERT_EQUAL_STRING("", probe.upsertRequest.expectedSsid.c_str());
    TEST_ASSERT_TRUE(probe.upsertRequest.hasPriority);
    TEST_ASSERT_EQUAL_UINT8(0, probe.upsertRequest.priority);

    for (const char* body : {R"({"index":0,"ssid":"New"})",
                            R"({"expectedSsid":"Old","ssid":"New"})",
                            R"({"index":0,"expectedSsid":null,"ssid":"New"})",
                            R"({"index":0,"expectedSsid":"Old\u0000suffix","ssid":"New"})"}) {
        WebServer invalid(80);
        invalid.setArg("plain", body);
        WifiClientApiService::handleApiNetworksSave(invalid, runtime, allow, nullptr, nullptr, nullptr);
        TEST_ASSERT_EQUAL_INT(400, invalid.lastStatusCode);
    }
    TEST_ASSERT_EQUAL_INT(2, probe.upsertCalls);
}

void test_password_input_allocation_failure_cannot_become_open_network() {
    Probe probe;
    const auto runtime = makeRuntime(probe);
    WebServer unavailable;
    unavailable.setArg("plain", R"({"ssid":"Garage","password":"<fixture-long-password>"})");
    mockStringConstructorFailureValue = "<fixture-long-password>";
    WifiClientApiService::handleApiNetworksSave(unavailable, runtime, allow, nullptr, nullptr, nullptr);
    mockStringConstructorFailureValue = nullptr;
    TEST_ASSERT_EQUAL_INT(503, unavailable.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(0, probe.upsertCalls);

    WebServer truncated;
    truncated.setArg("plain", R"({"ssid":"Garage","password":"\u0000suffix"})");
    WifiClientApiService::handleApiNetworksSave(truncated, runtime, allow, nullptr, nullptr, nullptr);
    TEST_ASSERT_EQUAL_INT(400, truncated.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(0, probe.upsertCalls);

    WebServer open;
    open.setArg("plain", R"({"ssid":"Garage","password":""})");
    WifiClientApiService::handleApiNetworksSave(open, runtime, allow, nullptr, nullptr, nullptr);
    TEST_ASSERT_EQUAL_INT(200, open.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(1, probe.upsertCalls);
    TEST_ASSERT_TRUE(probe.upsertRequest.hasPassword);
    TEST_ASSERT_EQUAL_STRING("", probe.upsertRequest.password.c_str());
}

void test_saved_network_delete_failure_is_not_reported_as_success() {
    WebServer server(80);
    Probe probe;
    probe.deleteResult = SlotActionStatus::Failed;
    server.setArg("plain", R"({"index":2,"expectedSsid":"Garage"})");

    WifiClientApiService::handleApiNetworksDelete(server, makeRuntime(probe), allow, nullptr, nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(404, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(1, probe.deleteCalls);
    TEST_ASSERT_EQUAL_UINT(2u, probe.slotRequest.index);
    TEST_ASSERT_TRUE(contains(server.lastBody, "network_delete_failed"));
}

void test_saved_network_connect_failure_is_not_reported_as_success() {
    WebServer server(80);
    Probe probe;
    probe.testResult = SlotActionStatus::Failed;
    server.setArg("plain", R"({"index":1,"expectedSsid":"Garage"})");

    WifiClientApiService::handleApiNetworksTest(server, makeRuntime(probe), allow, nullptr, nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(404, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(1, probe.testCalls);
    TEST_ASSERT_EQUAL_UINT(1u, probe.slotRequest.index);
    TEST_ASSERT_TRUE(contains(server.lastBody, "network_test_failed"));
}

void test_priority_batch_validates_all_entries_before_callback() {
    WebServer server(80);
    Probe probe;
    server.setArg("plain", R"({"updates":[{"index":0,"priority":1,"expectedSsid":"One","expectedPriority":0},)"
                          R"({"index":0,"priority":2,"expectedSsid":"Two","expectedPriority":1}]})");

    WifiClientApiService::handleApiNetworksPriorities(
        server, makeRuntime(probe), allow, nullptr, nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(0, probe.priorityCalls);
}

void test_slot_actions_and_reorder_require_well_formed_preconditions() {
    Probe probe;
    const auto runtime = makeRuntime(probe);
    for (const auto handler : {WifiClientApiService::handleApiNetworksDelete,
                               WifiClientApiService::handleApiNetworksTest}) {
        for (const char* body : {R"({"index":0})", R"({"index":0,"expectedSsid":""})",
                                 R"({"index":0,"expectedSsid":"Garage\u0000suffix"})",
                                 R"({"index":0,"expectedSsid":3})", R"({"index":4,"expectedSsid":"Garage"})"}) {
            WebServer invalid;
            invalid.setArg("plain", body);
            handler(invalid, runtime, allow, nullptr, nullptr, nullptr);
            TEST_ASSERT_EQUAL_INT(400, invalid.lastStatusCode);
        }
    }
    for (const char* body : {R"({"updates":[{"index":0,"priority":1}]})",
                             R"({"updates":[{"index":0,"priority":1,"expectedPriority":0}]})",
                             R"({"updates":[{"index":0,"priority":1,"expectedSsid":"One"}]})",
                             R"({"updates":[{"index":0,"priority":1,"expectedSsid":"One","expectedPriority":256}]})",
                             R"({"updates":[{"index":0,"priority":1,"expectedSsid":"One","expectedPriority":-1}]})",
                             R"({"updates":[{"index":0,"priority":1,"expectedSsid":"One","expectedPriority":0.5}]})"}) {
        WebServer invalid;
        invalid.setArg("plain", body);
        WifiClientApiService::handleApiNetworksPriorities(invalid, runtime, allow, nullptr, nullptr, nullptr);
        TEST_ASSERT_EQUAL_INT(400, invalid.lastStatusCode);
    }
    TEST_ASSERT_EQUAL_INT(0, probe.deleteCalls);
    TEST_ASSERT_EQUAL_INT(0, probe.testCalls);
    TEST_ASSERT_EQUAL_INT(0, probe.priorityCalls);
}

void test_priority_batch_maps_persist_failure_to_500() {
    WebServer server(80);
    Probe probe;
    probe.priorityResult = WifiClientApiService::PriorityUpdateStatus::PersistFailed;
    server.setArg("plain", R"({"updates":[{"index":0,"priority":1,"expectedSsid":" One ","expectedPriority":255},)"
                          R"({"index":1,"priority":0,"expectedSsid":"Two","expectedPriority":1}]})");

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
    server.setArg("plain", R"({"updates":[{"index":0,"priority":1,"expectedSsid":" One ","expectedPriority":255},)"
                          R"({"index":1,"priority":0,"expectedSsid":"Two","expectedPriority":1}]})");

    WifiClientApiService::handleApiNetworksPriorities(
        server, makeRuntime(probe), allow, nullptr, nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(1, probe.priorityCalls);
    TEST_ASSERT_EQUAL_UINT(2u, probe.updates.size());
    TEST_ASSERT_EQUAL_STRING(" One ", probe.updates[0].expectedSsid.c_str());
    TEST_ASSERT_EQUAL_UINT8(255, probe.updates[0].expectedPriority);
}

void test_mutating_json_routes_require_exact_roots_and_accept_valid_requests() {
    Probe probe;
    const auto runtime = makeRuntime(probe);

    WebServer enable(80);
    enable.setArg("plain", "{\"enabled\":true}x");
    WifiClientApiService::handleApiEnable(enable, runtime, allow, nullptr, nullptr, nullptr);
    TEST_ASSERT_EQUAL_INT(400, enable.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(0, probe.enableCalls);
    TEST_ASSERT_EQUAL_INT(0, probe.disableCalls);

    WebServer save(80);
    save.setArg("plain", "{\"ssid\":\"Garage\"}trailing");
    WifiClientApiService::handleApiNetworksSave(save, runtime, allow, nullptr, nullptr, nullptr);
    TEST_ASSERT_EQUAL_INT(400, save.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(0, probe.upsertCalls);

    WebServer remove(80);
    remove.setArg("plain", R"({"index":2,"expectedSsid":"Garage"}{"index":0})");
    WifiClientApiService::handleApiNetworksDelete(remove, runtime, allow, nullptr, nullptr, nullptr);
    TEST_ASSERT_EQUAL_INT(400, remove.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(0, probe.deleteCalls);

    WebServer priorities(80);
    priorities.setArg("plain", R"({"updates":[{"index":0,"priority":1,"expectedSsid":"Garage","expectedPriority":0}]}x)");
    WifiClientApiService::handleApiNetworksPriorities(priorities, runtime, allow, nullptr, nullptr, nullptr);
    TEST_ASSERT_EQUAL_INT(400, priorities.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(0, probe.priorityCalls);

    WebServer test(80);
    test.setArg("plain", R"({"index":1,"expectedSsid":"Garage"}{"index":0})");
    WifiClientApiService::handleApiNetworksTest(test, runtime, allow, nullptr, nullptr, nullptr);
    TEST_ASSERT_EQUAL_INT(400, test.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(0, probe.testCalls);

    WebServer validEnable(80);
    validEnable.setArg("plain", "{\"enabled\":true}");
    WifiClientApiService::handleApiEnable(validEnable, runtime, allow, nullptr, nullptr, nullptr);
    TEST_ASSERT_EQUAL_INT(200, validEnable.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(1, probe.enableCalls);

    WebServer validSave(80);
    validSave.setArg("plain", "{\"ssid\":\"Garage\"}");
    WifiClientApiService::handleApiNetworksSave(validSave, runtime, allow, nullptr, nullptr, nullptr);
    TEST_ASSERT_EQUAL_INT(200, validSave.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(1, probe.upsertCalls);

    WebServer validRemove(80);
    validRemove.setArg("plain", R"({"index":2,"expectedSsid":"Garage"})");
    WifiClientApiService::handleApiNetworksDelete(validRemove, runtime, allow, nullptr, nullptr, nullptr);
    TEST_ASSERT_EQUAL_INT(200, validRemove.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(1, probe.deleteCalls);

    WebServer validPriorities(80);
    validPriorities.setArg("plain", R"({"updates":[{"index":0,"priority":1,"expectedSsid":"Garage","expectedPriority":0}]})");
    WifiClientApiService::handleApiNetworksPriorities(validPriorities, runtime, allow, nullptr, nullptr, nullptr);
    TEST_ASSERT_EQUAL_INT(200, validPriorities.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(1, probe.priorityCalls);

    WebServer validTest(80);
    validTest.setArg("plain", R"({"index":1,"expectedSsid":"Garage"})");
    WifiClientApiService::handleApiNetworksTest(validTest, runtime, allow, nullptr, nullptr, nullptr);
    TEST_ASSERT_EQUAL_INT(200, validTest.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(1, probe.testCalls);
}

void test_mutating_json_route_reports_dom_allocation_failure_without_callback() {
    Probe probe;
    WebServer server(80);
    server.setArg("plain", "{\"enabled\":true}");

    // ExactJsonInput frees its key table immediately before ArduinoJson builds
    // the DOM. Fail both PSRAM and internal-RAM DOM allocation attempts while
    // leaving the later error response allocation available.
    g_mock_heap_caps_free_observer = [](void*) { g_mock_heap_caps_fail_call_mask = 0x3u; };
    WifiClientApiService::handleApiEnable(server, makeRuntime(probe), allow, nullptr, nullptr, nullptr);
    g_mock_heap_caps_free_observer = nullptr;

    TEST_ASSERT_EQUAL_INT(503, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(0, probe.enableCalls);
    TEST_ASSERT_EQUAL_INT(0, probe.disableCalls);
}

void test_saved_network_response_names_logical_order_and_preserves_legacy_key() {
    WebServer server(80);
    Probe probe;
    auto runtime = makeRuntime(probe);
    runtime.getSavedNetworks = [](void*) {
        WifiClientApiService::SavedNetworkSlotPayload slot;
        slot.index = 0;
        slot.ssid = "Garage";
        slot.label = "Garage";
        slot.priority = 0;
        slot.lastConnectedAtSec = 17;
        slot.configured = true;
        return std::vector<WifiClientApiService::SavedNetworkSlotPayload>{slot};
    };

    WifiClientApiService::handleApiNetworks(server, runtime, nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(contains(server.lastBody, "\"lastConnectedOrder\":17"));
    TEST_ASSERT_TRUE(contains(server.lastBody, "\"lastConnectedAtSec\":17"));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_forget_persistence_failure_is_not_reported_as_success);
    RUN_TEST(test_disable_persistence_failure_is_not_reported_as_success);
    RUN_TEST(test_enable_failure_is_not_reported_as_success);
    RUN_TEST(test_maintenance_enable_starts_now_without_deferred_boot_message);
    RUN_TEST(test_maintenance_saved_network_test_starts_live_connection);
    RUN_TEST(test_saved_network_upsert_failure_is_not_reported_as_success);
    RUN_TEST(test_network_save_requires_exact_identity_for_an_indexed_request);
    RUN_TEST(test_password_input_allocation_failure_cannot_become_open_network);
    RUN_TEST(test_saved_network_delete_failure_is_not_reported_as_success);
    RUN_TEST(test_saved_network_connect_failure_is_not_reported_as_success);
    RUN_TEST(test_priority_batch_validates_all_entries_before_callback);
    RUN_TEST(test_slot_actions_and_reorder_require_well_formed_preconditions);
    RUN_TEST(test_priority_batch_maps_persist_failure_to_500);
    RUN_TEST(test_priority_batch_success_returns_200_after_one_callback);
    RUN_TEST(test_mutating_json_routes_require_exact_roots_and_accept_valid_requests);
    RUN_TEST(test_mutating_json_route_reports_dom_allocation_failure_without_callback);
    RUN_TEST(test_saved_network_response_names_logical_order_and_preserves_legacy_key);
    return UNITY_END();
}

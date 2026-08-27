#include <unity.h>

#include "../mocks/Arduino.h"
#include "../mocks/WebServer.h"
#include "../../src/modules/wifi/wifi_autopush_api_service.h"
#include "../../src/modules/wifi/wifi_autopush_api_service.cpp"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

namespace {

struct FakeRuntime {
    WifiAutoPushApiService::SlotUpdateRequest update;
    int updateCalls = 0;
    WifiAutoPushApiService::ProfileAssignmentStatus profileStatus =
        WifiAutoPushApiService::ProfileAssignmentStatus::Success;
    bool persistResult = true;
};

WifiAutoPushApiService::Runtime makeRuntime(FakeRuntime& fake) {
    WifiAutoPushApiService::Runtime runtime{};
    runtime.loadSlotsSnapshot = [](WifiAutoPushApiService::SlotsSnapshot& snapshot, void*) {
        snapshot.slots[0].volume = 0xFF;
        snapshot.slots[0].muteVolume = 0xFF;
        snapshot.slots[0].volumeConfigured = false;
        snapshot.slots[1].volume = 7;
        snapshot.slots[1].muteVolume = 2;
        snapshot.slots[1].volumeConfigured = true;
    };
    runtime.loadPushStatusJson = [](String& json, void*) {
        json = "{\"result\":\"partial\",\"reason\":\"profile_verify_mismatch\"}";
        return true;
    };
    runtime.applySlotUpdate = [](const WifiAutoPushApiService::SlotUpdateRequest& request, void* ctx) {
        auto* state = static_cast<FakeRuntime*>(ctx);
        state->update = request;
        state->updateCalls++;
        return state->persistResult;
    };
    runtime.applySlotUpdateCtx = &fake;
    runtime.validateProfileAssignment = [](const String&, void* ctx) {
        return static_cast<FakeRuntime*>(ctx)->profileStatus;
    };
    runtime.validateProfileAssignmentCtx = &fake;
    return runtime;
}

bool alwaysAllow(void*) {
    return true;
}

bool contains(const String& body, const char* text) {
    return body.indexOf(text) >= 0;
}

void setRequiredSlotArgs(WebServer& server) {
    server.setArg("slot", "0");
    server.setArg("profile", "ROAD");
    server.setArg("mode", "2");
}

} // namespace

void setUp() {}
void tearDown() {}

void test_slots_api_uses_explicit_volume_contract_and_never_emits_255() {
    WebServer server(80);
    FakeRuntime fake;

    WifiAutoPushApiService::handleApiSlots(server, makeRuntime(fake));

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(contains(server.lastBody, "\"volumeConfigured\":false,\"volume\":0,\"muteVolume\":0"));
    TEST_ASSERT_TRUE(contains(server.lastBody, "\"volumeConfigured\":true,\"volume\":7,\"muteVolume\":2"));
    TEST_ASSERT_FALSE(contains(server.lastBody, "255"));
}

void test_slot_save_rejects_one_sided_volume_pair() {
    WebServer server(80);
    FakeRuntime fake;
    setRequiredSlotArgs(server);
    server.setArg("volume", "7");

    WifiAutoPushApiService::handleApiSlotSave(server, makeRuntime(fake), alwaysAllow, nullptr);

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(0, fake.updateCalls);
}

void test_slot_save_can_explicitly_disable_volume_pair() {
    WebServer server(80);
    FakeRuntime fake;
    setRequiredSlotArgs(server);
    server.setArg("volumeConfigured", "false");

    WifiAutoPushApiService::handleApiSlotSave(server, makeRuntime(fake), alwaysAllow, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(1, fake.updateCalls);
    TEST_ASSERT_TRUE(fake.update.hasVolume);
    TEST_ASSERT_TRUE(fake.update.hasMuteVolume);
    TEST_ASSERT_EQUAL_UINT8(0xFF, fake.update.volume);
    TEST_ASSERT_EQUAL_UINT8(0xFF, fake.update.muteVolume);
}

void test_status_api_preserves_terminal_result() {
    WebServer server(80);
    FakeRuntime fake;

    WifiAutoPushApiService::handleApiStatus(server, makeRuntime(fake));

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(contains(server.lastBody, "\"result\":\"partial\""));
    TEST_ASSERT_TRUE(contains(server.lastBody, "profile_verify_mismatch"));
}

void test_slot_save_rejects_nonexistent_profile_without_mutating_settings() {
    WebServer server(80);
    FakeRuntime fake;
    fake.profileStatus = WifiAutoPushApiService::ProfileAssignmentStatus::NotFound;
    setRequiredSlotArgs(server);

    WifiAutoPushApiService::handleApiSlotSave(server, makeRuntime(fake), alwaysAllow, nullptr);

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(0, fake.updateCalls);
    TEST_ASSERT_TRUE(contains(server.lastBody, "does not exist"));
}

void test_slot_save_reports_busy_for_temporarily_unreadable_profile() {
    WebServer server(80);
    FakeRuntime fake;
    fake.profileStatus = WifiAutoPushApiService::ProfileAssignmentStatus::Busy;
    setRequiredSlotArgs(server);

    WifiAutoPushApiService::handleApiSlotSave(server, makeRuntime(fake), alwaysAllow, nullptr);

    TEST_ASSERT_EQUAL_INT(409, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(0, fake.updateCalls);
}

void test_slot_save_requires_persistence_but_accepts_noop_success() {
    WebServer server(80);
    FakeRuntime fake;
    setRequiredSlotArgs(server);
    fake.persistResult = false;
    WifiAutoPushApiService::handleApiSlotSave(server, makeRuntime(fake), alwaysAllow, nullptr);
    TEST_ASSERT_EQUAL_INT(500, server.lastStatusCode);

    WebServer noopServer(80);
    setRequiredSlotArgs(noopServer);
    fake.persistResult = true; // runtime defines this as durable success, changed or no-op
    WifiAutoPushApiService::handleApiSlotSave(noopServer, makeRuntime(fake), alwaysAllow, nullptr);
    TEST_ASSERT_EQUAL_INT(200, noopServer.lastStatusCode);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_slots_api_uses_explicit_volume_contract_and_never_emits_255);
    RUN_TEST(test_slot_save_rejects_one_sided_volume_pair);
    RUN_TEST(test_slot_save_can_explicitly_disable_volume_pair);
    RUN_TEST(test_status_api_preserves_terminal_result);
    RUN_TEST(test_slot_save_rejects_nonexistent_profile_without_mutating_settings);
    RUN_TEST(test_slot_save_reports_busy_for_temporarily_unreadable_profile);
    RUN_TEST(test_slot_save_requires_persistence_but_accepts_noop_success);
    return UNITY_END();
}

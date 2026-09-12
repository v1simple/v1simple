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
    bool profileOwned = false;
    int activationCalls = 0;
    WifiAutoPushApiService::ActivationRequest activation;
};

WifiAutoPushApiService::Runtime makeRuntime(FakeRuntime& fake) {
    WifiAutoPushApiService::Runtime runtime{};
    runtime.loadSlotsSnapshot = [](WifiAutoPushApiService::SlotsSnapshot& snapshot, void* ctx) {
        auto* state = static_cast<FakeRuntime*>(ctx);
        snapshot.profileOwned = state->profileOwned;
        snapshot.slots[0].volume = 0xFF;
        snapshot.slots[0].muteVolume = 0xFF;
        snapshot.slots[0].volumeConfigured = false;
        snapshot.slots[1].volume = 7;
        snapshot.slots[1].muteVolume = 2;
        snapshot.slots[1].volumeConfigured = true;
        snapshot.slots[0].name = "One";
        snapshot.slots[0].profile = "ROAD";
        snapshot.slots[0].alertPersist = 3;
        snapshot.slots[0].priorityArrowOnly = true;
    };
    runtime.loadSlotsSnapshotCtx = &fake;
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
    runtime.applyActivation = [](const WifiAutoPushApiService::ActivationRequest& request, void* ctx) {
        auto* state = static_cast<FakeRuntime*>(ctx);
        state->activation = request;
        state->activationCalls++;
        return state->persistResult;
    };
    runtime.applyActivationCtx = &fake;
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

void test_profile_owned_slots_api_omits_legacy_detector_fields() {
    WebServer server(80);
    FakeRuntime fake;
    fake.profileOwned = true;

    WifiAutoPushApiService::handleApiSlots(server, makeRuntime(fake));

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(contains(server.lastBody, "\"schemaVersion\":2"));
    TEST_ASSERT_TRUE(contains(server.lastBody, "\"detectorConfigurationOwner\":\"profile\""));
    TEST_ASSERT_TRUE(contains(server.lastBody, "\"alertPersist\":3"));
    TEST_ASSERT_TRUE(contains(server.lastBody, "\"priorityArrowOnly\":true"));
    TEST_ASSERT_FALSE(contains(server.lastBody, "\"mode\":"));
    TEST_ASSERT_FALSE(contains(server.lastBody, "\"volumeConfigured\":"));
    TEST_ASSERT_FALSE(contains(server.lastBody, "\"darkMode\":"));
    TEST_ASSERT_FALSE(contains(server.lastBody, "\"muteToZero\":"));
}

void test_profile_owned_slot_save_accepts_only_assignment_and_slot_overlays() {
    WebServer server(80);
    FakeRuntime fake;
    fake.profileOwned = true;
    server.setArg("slot", "0");
    server.setArg("profile", "ROAD");
    server.setArg("name", "Commute");
    server.setArg("color", "1234");
    server.setArg("alertPersist", "4");
    server.setArg("priorityArrowOnly", "true");

    WifiAutoPushApiService::handleApiSlotSave(server, makeRuntime(fake), alwaysAllow, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(1, fake.updateCalls);
    TEST_ASSERT_TRUE(fake.update.profileOwned);
    TEST_ASSERT_EQUAL_STRING("ROAD", fake.update.profile.c_str());
    TEST_ASSERT_TRUE(fake.update.hasName);
    TEST_ASSERT_TRUE(fake.update.hasColor);
    TEST_ASSERT_TRUE(fake.update.hasAlertPersist);
    TEST_ASSERT_TRUE(fake.update.hasPriorityArrowOnly);
    TEST_ASSERT_FALSE(fake.update.hasVolumeConfigured);
    TEST_ASSERT_FALSE(fake.update.hasVolume);
    TEST_ASSERT_FALSE(fake.update.hasMuteVolume);
    TEST_ASSERT_FALSE(fake.update.hasDarkMode);
    TEST_ASSERT_FALSE(fake.update.hasMuteToZero);
}

void test_profile_owned_slot_save_rejects_detector_overrides_atomically() {
    const char* detectorFields[] = {
        "mode", "volumeConfigured", "volume", "muteVol", "muteVolume",
        "mainVolume", "mutedVolume", "darkMode", "muteToZero",
    };
    for (const char* field : detectorFields) {
        WebServer server(80);
        FakeRuntime fake;
        fake.profileOwned = true;
        server.setArg("slot", "0");
        server.setArg("profile", "ROAD");
        server.setArg(field, "1");

        WifiAutoPushApiService::handleApiSlotSave(server, makeRuntime(fake), alwaysAllow, nullptr);

        TEST_ASSERT_EQUAL_INT_MESSAGE(400, server.lastStatusCode, field);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, fake.updateCalls, field);
        TEST_ASSERT_TRUE(contains(server.lastBody, "belong to the selected profile"));
    }
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

void test_activate_reports_persistence_failure_instead_of_success() {
    WebServer server(80);
    FakeRuntime fake;
    fake.persistResult = false;
    server.setArg("slot", "2");
    server.setArg("enable", "true");

    WifiAutoPushApiService::handleApiActivate(server, makeRuntime(fake), alwaysAllow, nullptr);

    TEST_ASSERT_EQUAL_INT(500, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(1, fake.activationCalls);
    TEST_ASSERT_EQUAL_INT(2, fake.activation.slot);
    TEST_ASSERT_TRUE(fake.activation.enable);
    TEST_ASSERT_TRUE(contains(server.lastBody, "settings_persist_failed"));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_slots_api_uses_explicit_volume_contract_and_never_emits_255);
    RUN_TEST(test_slot_save_rejects_one_sided_volume_pair);
    RUN_TEST(test_slot_save_can_explicitly_disable_volume_pair);
    RUN_TEST(test_status_api_preserves_terminal_result);
    RUN_TEST(test_profile_owned_slots_api_omits_legacy_detector_fields);
    RUN_TEST(test_profile_owned_slot_save_accepts_only_assignment_and_slot_overlays);
    RUN_TEST(test_profile_owned_slot_save_rejects_detector_overrides_atomically);
    RUN_TEST(test_slot_save_rejects_nonexistent_profile_without_mutating_settings);
    RUN_TEST(test_slot_save_reports_busy_for_temporarily_unreadable_profile);
    RUN_TEST(test_slot_save_requires_persistence_but_accepts_noop_success);
    RUN_TEST(test_activate_reports_persistence_failure_instead_of_success);
    return UNITY_END();
}

#include <unity.h>

#include <initializer_list>

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
    TEST_ASSERT_TRUE(contains(server.lastBody, "\"schemaVersion\":3"));
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
    server.setArg("name", "COMMUTE");
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

void test_slot_save_rejects_non_roundtrippable_display_name_without_mutation() {
    for (const uint8_t byte : {uint8_t{0x01}, uint8_t{0xff}}) {
        WebServer server(80);
        FakeRuntime fake;
        fake.profileOwned = true;
        server.setArg("slot", "0");
        server.setArg("profile", "ROAD");
        String invalid("BAD");
        invalid += static_cast<char>(byte);
        server.setArg("name", invalid);

        WifiAutoPushApiService::handleApiSlotSave(server, makeRuntime(fake), alwaysAllow, nullptr);

        TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
        TEST_ASSERT_EQUAL_INT(0, fake.updateCalls);
        TEST_ASSERT_TRUE(contains(server.lastBody, "Invalid slot name"));
    }

    WebServer lowercaseServer(80);
    FakeRuntime lowercaseFake;
    lowercaseFake.profileOwned = true;
    lowercaseServer.setArg("slot", "0");
    lowercaseServer.setArg("profile", "ROAD");
    lowercaseServer.setArg("name", "Commute");
    WifiAutoPushApiService::handleApiSlotSave(
        lowercaseServer, makeRuntime(lowercaseFake), alwaysAllow, nullptr);
    TEST_ASSERT_EQUAL_INT(400, lowercaseServer.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(0, lowercaseFake.updateCalls);
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

void test_exact_body_slot_save_preserves_every_optional_field() {
    WebServer server(80);
    FakeRuntime fake;
    fake.profileOwned = true;
    const char body[] = "slot=2&profile=ROAD&name=NIGHT+DRIVE&color=4660&alertPersist=4&priorityArrowOnly=true";
    WifiAutoPushApiService::handleApiSlotSaveBody(
        server, makeRuntime(fake), reinterpret_cast<const uint8_t*>(body), sizeof(body) - 1u,
        alwaysAllow, nullptr);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(1, fake.updateCalls);
    TEST_ASSERT_EQUAL_INT(2, fake.update.slot);
    TEST_ASSERT_TRUE(fake.update.hasName);
    TEST_ASSERT_EQUAL_STRING("NIGHT DRIVE", fake.update.name.c_str());
    TEST_ASSERT_TRUE(fake.update.hasColor);
    TEST_ASSERT_EQUAL_UINT16(4660, fake.update.color);
    TEST_ASSERT_TRUE(fake.update.hasPriorityArrowOnly);
    TEST_ASSERT_TRUE(fake.update.priorityArrowOnly);
}

void test_exact_body_rejects_duplicate_or_missing_requested_optional_without_mutation() {
    for (const char* body : {
             "slot=1&profile=ROAD&name=One&n%61me=Two",
             "slot=1&profile=ROAD&priorityArrowOnly",
             "slot=1&profile=ROAD&priorityArrowOnyl=true",
         }) {
        WebServer server(80);
        FakeRuntime fake;
        fake.profileOwned = true;
        WifiAutoPushApiService::handleApiSlotSaveBody(
            server, makeRuntime(fake), reinterpret_cast<const uint8_t*>(body), std::strlen(body),
            alwaysAllow, nullptr);
        TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
        TEST_ASSERT_EQUAL_INT(0, fake.updateCalls);
    }
}

void test_exact_body_activation_rejects_unknown_field_without_mutation() {
    WebServer server(80);
    FakeRuntime fake;
    const char body[] = "slot=2&enable=true&enabel=false";
    WifiAutoPushApiService::handleApiActivateBody(
        server, makeRuntime(fake), reinterpret_cast<const uint8_t*>(body), sizeof(body) - 1u,
        alwaysAllow, nullptr);
    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(0, fake.activationCalls);
}

void test_exact_slot_forms_reject_noncanonical_numeric_and_relationship_values() {
    const char* invalidUrlEncoded[] = {
        "slot=1&profile=ROAD&mode=4",
        "slot=1&profile=ROAD&mode=2&color=0",
        "slot=1&profile=ROAD&mode=2&color=-1",
        "slot=1&profile=ROAD&mode=2&color=65536",
        "slot=1&profile=ROAD&mode=2&alertPersist=-1",
        "slot=1&profile=ROAD&mode=2&alertPersist=6",
        "slot=1&profile=ROAD&mode=2&volumeConfigured=false&volume=1&muteVol=2",
        "slot=1&profile=ROAD&mode=2&name=Commute",
    };
    for (const char* body : invalidUrlEncoded) {
        WebServer server(80);
        FakeRuntime fake;
        WifiAutoPushApiService::handleApiSlotSaveBody(
            server, makeRuntime(fake), reinterpret_cast<const uint8_t*>(body),
            std::strlen(body), alwaysAllow, nullptr);
        TEST_ASSERT_EQUAL_INT_MESSAGE(400, server.lastStatusCode, body);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, fake.updateCalls, body);
    }

    struct Field { const char* name; const char* value; };
    const auto assertMultipartRejected = [](std::initializer_list<Field> fields) {
        constexpr char boundary[] = "exact-invalid";
        String body;
        for (const Field& field : fields) {
            body += "--exact-invalid\r\nContent-Disposition: form-data; name=\"";
            body += field.name;
            body += "\"\r\n\r\n";
            body += field.value;
            body += "\r\n";
        }
        body += "--exact-invalid--\r\n";
        WebServer server(80);
        FakeRuntime fake;
        WifiAutoPushApiService::handleApiSlotSaveBody(
            server, makeRuntime(fake), reinterpret_cast<const uint8_t*>(body.c_str()),
            body.length(), alwaysAllow, nullptr, boundary, sizeof(boundary) - 1u);
        TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
        TEST_ASSERT_EQUAL_INT(0, fake.updateCalls);
    };
    assertMultipartRejected({{"slot", "1"}, {"profile", "ROAD"}, {"mode", "4"}});
    assertMultipartRejected({{"slot", "1"}, {"profile", "ROAD"}, {"mode", "2"},
                             {"color", "65536"}});
    assertMultipartRejected({{"slot", "1"}, {"profile", "ROAD"}, {"mode", "2"},
                             {"alertPersist", "6"}});
    assertMultipartRejected({{"slot", "1"}, {"profile", "ROAD"}, {"mode", "2"},
                             {"volumeConfigured", "false"}, {"volume", "1"},
                             {"muteVol", "2"}});
}

void test_shipped_ui_multipart_slot_and_activation_forms_remain_compatible() {
    constexpr char boundary[] = "legacy-v203";
    const char slotBody[] =
        "--legacy-v203\r\nContent-Disposition: form-data; name=\"slot\"\r\n\r\n2\r\n"
        "--legacy-v203\r\nContent-Disposition: form-data; name=\"profile\"\r\n\r\nROAD\r\n"
        "--legacy-v203\r\nContent-Disposition: form-data; name=\"name\"\r\n\r\nNIGHT DRIVE\r\n"
        "--legacy-v203--\r\n";
    WebServer slotServer(80);
    FakeRuntime slotFake;
    slotFake.profileOwned = true;
    WifiAutoPushApiService::handleApiSlotSaveBody(
        slotServer, makeRuntime(slotFake), reinterpret_cast<const uint8_t*>(slotBody),
        sizeof(slotBody) - 1u, alwaysAllow, nullptr, boundary, sizeof(boundary) - 1u);
    TEST_ASSERT_EQUAL_INT(200, slotServer.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(1, slotFake.updateCalls);
    TEST_ASSERT_EQUAL_INT(2, slotFake.update.slot);
    TEST_ASSERT_EQUAL_STRING("NIGHT DRIVE", slotFake.update.name.c_str());

    const char activateBody[] =
        "--legacy-v203\r\nContent-Disposition: form-data; name=\"slot\"\r\n\r\n1\r\n"
        "--legacy-v203\r\nContent-Disposition: form-data; name=\"enable\"\r\n\r\nfalse\r\n"
        "--legacy-v203--\r\n";
    WebServer activateServer(80);
    FakeRuntime activateFake;
    WifiAutoPushApiService::handleApiActivateBody(
        activateServer, makeRuntime(activateFake), reinterpret_cast<const uint8_t*>(activateBody),
        sizeof(activateBody) - 1u, alwaysAllow, nullptr, boundary, sizeof(boundary) - 1u);
    TEST_ASSERT_EQUAL_INT(200, activateServer.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(1, activateFake.activationCalls);
    TEST_ASSERT_EQUAL_INT(1, activateFake.activation.slot);
    TEST_ASSERT_FALSE(activateFake.activation.enable);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_slots_api_uses_explicit_volume_contract_and_never_emits_255);
    RUN_TEST(test_slot_save_rejects_one_sided_volume_pair);
    RUN_TEST(test_slot_save_can_explicitly_disable_volume_pair);
    RUN_TEST(test_status_api_preserves_terminal_result);
    RUN_TEST(test_profile_owned_slots_api_omits_legacy_detector_fields);
    RUN_TEST(test_profile_owned_slot_save_accepts_only_assignment_and_slot_overlays);
    RUN_TEST(test_slot_save_rejects_non_roundtrippable_display_name_without_mutation);
    RUN_TEST(test_profile_owned_slot_save_rejects_detector_overrides_atomically);
    RUN_TEST(test_slot_save_rejects_nonexistent_profile_without_mutating_settings);
    RUN_TEST(test_slot_save_reports_busy_for_temporarily_unreadable_profile);
    RUN_TEST(test_slot_save_requires_persistence_but_accepts_noop_success);
    RUN_TEST(test_activate_reports_persistence_failure_instead_of_success);
    RUN_TEST(test_exact_body_slot_save_preserves_every_optional_field);
    RUN_TEST(test_exact_body_rejects_duplicate_or_missing_requested_optional_without_mutation);
    RUN_TEST(test_exact_body_activation_rejects_unknown_field_without_mutation);
    RUN_TEST(test_exact_slot_forms_reject_noncanonical_numeric_and_relationship_values);
    RUN_TEST(test_shipped_ui_multipart_slot_and_activation_forms_remain_compatible);
    return UNITY_END();
}

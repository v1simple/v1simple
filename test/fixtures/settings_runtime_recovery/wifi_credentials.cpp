#define main original_settings_suite_main
#include "test/test_settings_persistence/test_settings_persistence.cpp"
#undef main

#include <fstream>
#include "src/modules/wifi/wifi_saved_network_mutation_policy.h"

using WifiClientApiService::SlotActionStatus;

// Compile the real saved-network methods and settings persistence owner.
// Only unrelated radio lifecycle actions are recorded by this adapter.
class WiFiManager {
public:
    explicit WiFiManager(SettingsManager& settings) : settings_(settings) {}
    SettingsManager& settings_;
    unsigned radioChanges = 0;
    unsigned connectionTests = 0;
    struct StationEffects {
        unsigned& changes;
        unsigned& tests;
        bool testSavedNetwork(int, size_t) { ++tests; return true; }
        WifiSavedNetworkMutationPolicy::Decision savedNetworkMutation(size_t) const { return {}; }
        void savedNetworkMutationCommitted(int, size_t, WifiSavedNetworkMutationPolicy::Decision, const char*) {
            ++changes;
        }
    } station_{radioChanges, connectionTests};
    int stationEnvironment() const { return 0; }
    std::vector<WifiClientApiService::SavedNetworkSlotPayload> getSavedNetworkSlots() const;
    int selectSlotForNetworkConnect(const String&) const;
    SlotActionStatus upsertSavedNetwork(const WifiClientApiService::SavedNetworkUpsertPayload&, size_t&);
    SlotActionStatus deleteSavedNetwork(const WifiClientApiService::SavedNetworkSlotRequest&);
    SlotActionStatus testSavedNetwork(const WifiClientApiService::SavedNetworkSlotRequest&);
    bool savedNetworkSlotMatches(const WifiClientApiService::SavedNetworkSlotRequest&) const;
    WifiClientApiService::PriorityUpdateStatus updateSavedNetworkPriorities(
        const std::vector<WifiClientApiService::SavedNetworkPriorityUpdate>&);
};

#include "wifi_credentials.inc"

namespace {
std::filesystem::path editorSlotPath;

void injectPasswordReadFailure(size_t slot, int fault) {
    if (fault == 0) {
        mock_preferences::set_fail_read_only_begin_for_namespace(WIFI_CLIENT_NS, 1);
    } else {
        const auto stage = fault == 1 ? mock_nvs::ReadStage::Presence
                         : fault == 2 || fault == 4 ? mock_nvs::ReadStage::StringSize
                                                  : mock_nvs::ReadStage::StringValue;
        mock_nvs::set_nth_read_result(WIFI_CLIENT_NS, kNvsWifiStaSlotPassword[slot],
                                     fault >= 4 ? ESP_ERR_NVS_NOT_FOUND : ESP_FAIL, stage, 1);
    }
}

WifiClientApiService::SavedNetworkUpsertPayload labelEdit(size_t slot) {
    WifiClientApiService::SavedNetworkUpsertPayload request;
    request.hasIndex = true;
    request.index = slot;
    request.hasExpectedSsid = true;
    request.expectedSsid = "LookupNet";
    request.ssid = "LookupNet";
    request.hasLabel = true;
    request.label = "Renamed";
    request.hasPriority = true;
    request.priority = 2;
    return request;
}

using SavedNetworkHandler = decltype(&WifiClientApiService::handleApiNetworksSave);

void postSavedNetwork(WebServer& server, WiFiManager& wifi, SavedNetworkHandler handler, const char* body) {
    WifiClientApiService::Runtime runtime;
    runtime.maintenanceBootActive = true;
    runtime.upsertSavedNetwork = [](const WifiClientApiService::SavedNetworkUpsertPayload& request,
                                    size_t& index, void* ctx) {
        return static_cast<WiFiManager*>(ctx)->upsertSavedNetwork(request, index);
    };
    runtime.upsertSavedNetworkCtx = &wifi;
    runtime.deleteSavedNetwork = [](const WifiClientApiService::SavedNetworkSlotRequest& request, void* ctx) {
        return static_cast<WiFiManager*>(ctx)->deleteSavedNetwork(request);
    };
    runtime.deleteSavedNetworkCtx = &wifi;
    runtime.testSavedNetwork = [](const WifiClientApiService::SavedNetworkSlotRequest& request, void* ctx) {
        return static_cast<WiFiManager*>(ctx)->testSavedNetwork(request);
    };
    runtime.testSavedNetworkCtx = &wifi;
    runtime.updateSavedNetworkPriorities = [](const std::vector<WifiClientApiService::SavedNetworkPriorityUpdate>& updates,
                                              void* ctx) {
        return static_cast<WiFiManager*>(ctx)->updateSavedNetworkPriorities(updates);
    };
    runtime.updateSavedNetworkPrioritiesCtx = &wifi;
    server.setArg("plain", body);
    bool preAdmitted = true;
    WifiMaintenanceWritePolicy::dispatchStorageResolved(server, preAdmitted,
        [&]() { return wifi.settings_.resolveStorageTransactionsForMutation(); },
        [&]() { handler(server, runtime, nullptr, nullptr, nullptr, nullptr); });
}
} // namespace

void test_omitted_password_failure_refuses_edit_and_retry_preserves_credentials() {
    for (size_t slot : {size_t{0}, size_t{1}}) {
        for (int fault = 0; fault < 6; ++fault) {
            resetRuntimeState();
            fs::FS filesystem(g_tempRoot / ("edit_" + std::to_string(slot) + "_" + std::to_string(fault)));
            SettingsManager manager(storage, profiles);
            seedWifiPasswordWithStaleSd(manager, filesystem, slot);
            WiFiManager wifi(manager);
            const V1Settings before = manager.get();
            const String passwordBefore = mock_preferences::getString(WIFI_CLIENT_NS, kNvsWifiStaSlotPassword[slot]);
            const auto sdBefore = readFileToString(filesystem, WIFI_CLIENT_SD_SECRET_PATH);
            size_t writes = 0;
            mock_preferences::afterWrite() = [&](const char*) { ++writes; };
            injectPasswordReadFailure(slot, fault);
            size_t selected = kWifiStaSlotCount;
            const auto saved = wifi.upsertSavedNetwork(labelEdit(slot), selected);
            mock_preferences::afterWrite() = {};
            TEST_ASSERT_TRUE(saved == SlotActionStatus::Failed);
            TEST_ASSERT_EQUAL_UINT(kWifiStaSlotCount, selected);
            TEST_ASSERT_EQUAL_UINT(0, writes);
            TEST_ASSERT_EQUAL_UINT(0, wifi.radioChanges);
            assertSettingsEqual(before, manager.get());
            TEST_ASSERT_EQUAL_STRING(passwordBefore.c_str(),
                mock_preferences::getString(WIFI_CLIENT_NS, kNvsWifiStaSlotPassword[slot]).c_str());
            TEST_ASSERT_EQUAL_STRING(sdBefore.c_str(), readFileToString(filesystem, WIFI_CLIENT_SD_SECRET_PATH).c_str());
            TEST_ASSERT_EQUAL_STRING("<current-password>", manager.getWifiStaSlotPassword(slot).c_str());

            TEST_ASSERT_TRUE(wifi.upsertSavedNetwork(labelEdit(slot), selected) == SlotActionStatus::Success);
            TEST_ASSERT_EQUAL_UINT(slot, selected);
            TEST_ASSERT_EQUAL_STRING("Renamed", manager.get().wifiStaSlots[slot].label.c_str());
            TEST_ASSERT_EQUAL_UINT8(2, manager.get().wifiStaSlots[slot].priority);
            TEST_ASSERT_EQUAL_STRING("<current-password>", manager.getWifiStaSlotPassword(slot).c_str());
            SettingsManager rebooted(storage, profiles);
            rebooted.load();
            TEST_ASSERT_EQUAL_STRING("<current-password>", rebooted.getWifiStaSlotPassword(slot).c_str());
        }
    }
}

void test_unavailable_password_listing_keeps_blank_editor_preservation() {
    for (int fault = 0; fault < 6; ++fault) {
        resetRuntimeState();
        fs::FS filesystem(g_tempRoot / ("listing_" + std::to_string(fault)));
        SettingsManager manager(storage, profiles);
        seedWifiPasswordWithStaleSd(manager, filesystem, 0);
        WiFiManager wifi(manager);
        const V1Settings before = manager.get();
        const auto sdBefore = readFileToString(filesystem, WIFI_CLIENT_SD_SECRET_PATH);
        size_t writes = 0;
        mock_preferences::afterWrite() = [&](const char*) { ++writes; };
        injectPasswordReadFailure(0, fault);
        const auto slots = wifi.getSavedNetworkSlots();
        mock_preferences::afterWrite() = {};
        TEST_ASSERT_EQUAL_UINT(kWifiStaSlotCount, slots.size());
        TEST_ASSERT_TRUE(slots[0].configured);
        TEST_ASSERT_TRUE(slots[0].hasPassword);
        TEST_ASSERT_FALSE(slots[1].hasPassword);
        TEST_ASSERT_EQUAL_UINT(0, writes);
        assertSettingsEqual(before, manager.get());
        TEST_ASSERT_EQUAL_STRING(sdBefore.c_str(), readFileToString(filesystem, WIFI_CLIENT_SD_SECRET_PATH).c_str());
        TEST_ASSERT_EQUAL_STRING("<current-password>", manager.getWifiStaSlotPassword(0).c_str());
        if (fault == 0) {
            JsonDocument payload;
            payload["index"] = slots[0].index;
            payload["ssid"] = slots[0].ssid;
            payload["priority"] = slots[0].priority;
            payload["hasPassword"] = slots[0].hasPassword;
            std::ofstream output(editorSlotPath);
            serializeJson(payload, output);
            TEST_ASSERT_TRUE(output.good());
        }
    }
}

void test_healthy_omitted_and_explicit_empty_passwords_keep_their_meaning() {
    for (const char* password : {"", "<current-password>"}) {
        resetRuntimeState();
        fs::FS filesystem(g_tempRoot / (password[0] ? "secured" : "open"));
        SettingsManager manager(storage, profiles);
        seedWifiPasswordWithStaleSd(manager, filesystem, 0, password);
        WiFiManager wifi(manager);
        TEST_ASSERT_EQUAL(password[0] != '\0', wifi.getSavedNetworkSlots()[0].hasPassword);
        size_t selected = kWifiStaSlotCount;
        auto request = labelEdit(0);
        TEST_ASSERT_TRUE(wifi.upsertSavedNetwork(request, selected) == SlotActionStatus::Success);
        TEST_ASSERT_EQUAL_STRING(password, manager.getWifiStaSlotPassword(0).c_str());
        request.hasPassword = true;
        request.password = "";
        TEST_ASSERT_TRUE(wifi.upsertSavedNetwork(request, selected) == SlotActionStatus::Success);
        TEST_ASSERT_EQUAL_STRING("", manager.getWifiStaSlotPassword(0).c_str());
        TEST_ASSERT_TRUE(mock_preferences::namespaceHasKey(WIFI_CLIENT_NS, kNvsWifiStaSlotPassword[0]));
        TEST_ASSERT_FALSE(wifi.getSavedNetworkSlots()[0].hasPassword);
    }
}

void test_implicit_priority_uses_current_saved_order_and_appends_after_legacy_ties() {
    resetRuntimeState();
    fs::FS filesystem(g_tempRoot / "priority_order");
    SettingsManager manager(storage, profiles);
    seedWifiPasswordWithStaleSd(manager, filesystem, 0);
    TEST_ASSERT_TRUE(manager.setWifiStaSlotCredentials(0, "LookupNet", "first", "First", 7));
    TEST_ASSERT_TRUE(manager.setWifiStaSlotCredentials(2, "Second", "second", "Second", 7));
    WiFiManager wifi(manager);
    size_t selected = kWifiStaSlotCount;
    auto edit = labelEdit(0);
    edit.hasPriority = false;
    TEST_ASSERT_TRUE(wifi.upsertSavedNetwork(edit, selected) == SlotActionStatus::Success);
    TEST_ASSERT_EQUAL_UINT(0, selected);

    WifiClientApiService::SavedNetworkUpsertPayload add;
    add.ssid = "New network";
    add.hasPassword = true;
    add.password = "<fixture-password>";
    TEST_ASSERT_TRUE(wifi.upsertSavedNetwork(add, selected) == SlotActionStatus::Success);
    TEST_ASSERT_EQUAL_UINT(1, selected);
    SettingsManager rebooted(storage, profiles);
    rebooted.load();
    TEST_ASSERT_EQUAL_UINT8(7, rebooted.get().wifiStaSlots[0].priority);
    TEST_ASSERT_EQUAL_UINT8(8, rebooted.get().wifiStaSlots[1].priority);
    TEST_ASSERT_EQUAL_UINT8(7, rebooted.get().wifiStaSlots[2].priority);
    TEST_ASSERT_EQUAL_STRING("Renamed", rebooted.get().wifiStaSlots[0].label.c_str());
    TEST_ASSERT_EQUAL_STRING("first", rebooted.getWifiStaSlotPassword(0).c_str());
}

void test_stale_identity_and_full_priority_range_cannot_write() {
    resetRuntimeState();
    fs::FS filesystem(g_tempRoot / "stale_or_full_priority");
    SettingsManager manager(storage, profiles);
    seedWifiPasswordWithStaleSd(manager, filesystem, 0);
    TEST_ASSERT_TRUE(manager.setWifiStaSlotCredentials(0, "Replacement", "replacement", "Current", 255));
    WiFiManager wifi(manager);
    auto edit = labelEdit(0);
    WifiClientApiService::SavedNetworkUpsertPayload add;
    add.ssid = "New network";
    add.hasPassword = true;
    add.password = "new";
    size_t writes = 0;
    mock_preferences::afterWrite() = [&](const char*) { ++writes; };
    size_t selected = kWifiStaSlotCount;
    const auto staleSaved = wifi.upsertSavedNetwork(edit, selected);
    const auto implicitSaved = wifi.upsertSavedNetwork(add, selected);
    mock_preferences::afterWrite() = {};
    TEST_ASSERT_TRUE(staleSaved == SlotActionStatus::Conflict);
    TEST_ASSERT_TRUE(implicitSaved == SlotActionStatus::Failed);
    TEST_ASSERT_EQUAL_UINT(kWifiStaSlotCount, selected);
    TEST_ASSERT_EQUAL_UINT(0, writes);
    TEST_ASSERT_EQUAL_UINT(0, wifi.radioChanges);

    add.hasPriority = true;
    add.priority = 4;
    TEST_ASSERT_TRUE(wifi.upsertSavedNetwork(add, selected) == SlotActionStatus::Success);
    TEST_ASSERT_EQUAL_UINT(1, selected);
    SettingsManager rebooted(storage, profiles);
    rebooted.load();
    TEST_ASSERT_EQUAL_STRING("Replacement", rebooted.get().wifiStaSlots[0].ssid.c_str());
    TEST_ASSERT_EQUAL_STRING("replacement", rebooted.getWifiStaSlotPassword(0).c_str());
    TEST_ASSERT_EQUAL_UINT8(255, rebooted.get().wifiStaSlots[0].priority);
    TEST_ASSERT_EQUAL_UINT8(4, rebooted.get().wifiStaSlots[1].priority);
}

void test_stale_delete_and_test_leave_replacement_durable() {
    using namespace WifiClientApiService;
    fs::FS filesystem(g_tempRoot);
    storage.setFilesystem(&filesystem, true);
    TEST_ASSERT_TRUE(profiles.begin(storage));
    SettingsManager manager(storage, profiles);
    TEST_ASSERT_TRUE(manager.setWifiStaSlotCredentials(0, "Old network", "old", "Old", 0));
    WiFiManager wifi(manager);

    WebServer replace;
    postSavedNetwork(replace, wifi, handleApiNetworksSave,
        R"({"index":0,"expectedSsid":"Old network","ssid":" Replacement ","password":"replacement"})");
    TEST_ASSERT_EQUAL_INT(200, replace.lastStatusCode);
    const auto before = manager.get();
    unsigned writes = 0;
    mock_preferences::afterWrite() = [&](const char*) { ++writes; };
    for (const auto action : {handleApiNetworksDelete, handleApiNetworksTest}) {
        WebServer legacy;
        postSavedNetwork(legacy, wifi, action, R"({"index":0})");
        TEST_ASSERT_EQUAL_INT(400, legacy.lastStatusCode);
        for (const char* body : {R"({"index":0,"expectedSsid":"Old network"})",
                                R"({"index":0,"expectedSsid":"Replacement"})"}) {
            WebServer stale;
            postSavedNetwork(stale, wifi, action, body);
            TEST_ASSERT_EQUAL_INT(409, stale.lastStatusCode);
            TEST_ASSERT_TRUE(stale.lastBody.indexOf("network_slot_changed") >= 0);
        }
    }
    mock_preferences::afterWrite() = {};
    TEST_ASSERT_EQUAL_UINT(0, writes);
    TEST_ASSERT_EQUAL_UINT(1, wifi.radioChanges);
    TEST_ASSERT_EQUAL_UINT(0, wifi.connectionTests);
    assertSettingsEqual(before, manager.get());

    SettingsManager rebooted(storage, profiles);
    rebooted.load();
    TEST_ASSERT_EQUAL_STRING(" Replacement ", rebooted.get().wifiStaSlots[0].ssid.c_str());
    TEST_ASSERT_EQUAL_STRING("replacement", rebooted.getWifiStaSlotPassword(0).c_str());

    WebServer test;
    postSavedNetwork(test, wifi, handleApiNetworksTest, R"({"index":0,"expectedSsid":" Replacement "})");
    TEST_ASSERT_EQUAL_INT(200, test.lastStatusCode);
    TEST_ASSERT_EQUAL_UINT(1, wifi.connectionTests);
    WebServer remove;
    postSavedNetwork(remove, wifi, handleApiNetworksDelete, R"({"index":0,"expectedSsid":" Replacement "})");
    TEST_ASSERT_EQUAL_INT(200, remove.lastStatusCode);
    TEST_ASSERT_FALSE(manager.get().wifiStaSlots[0].isConfigured());
    TEST_ASSERT_EQUAL_UINT(2, wifi.radioChanges);
}

void test_saved_network_create_conflict_preserves_acknowledged_password() {
    using namespace WifiClientApiService;
    fs::FS filesystem(g_tempRoot);
    storage.setFilesystem(&filesystem, true);
    TEST_ASSERT_TRUE(profiles.begin(storage));
    SettingsManager manager(storage, profiles);
    WiFiManager wifi(manager);
    WebServer create;
    postSavedNetwork(create, wifi, handleApiNetworksSave, R"({"ssid":"Shared","password":"fixture"})");
    TEST_ASSERT_EQUAL_INT(200, create.lastStatusCode);
    unsigned writes = 0;
    mock_preferences::afterWrite() = [&](const char*) { ++writes; };
    WebServer legacy;
    postSavedNetwork(legacy, wifi, handleApiNetworksSave, R"({"index":0,"ssid":"Stale","password":""})");
    TEST_ASSERT_EQUAL_INT(400, legacy.lastStatusCode);
    WebServer duplicate;
    postSavedNetwork(duplicate, wifi, handleApiNetworksSave, R"({"ssid":"Shared","password":""})");
    mock_preferences::afterWrite() = {};
    TEST_ASSERT_EQUAL_INT(409, duplicate.lastStatusCode);
    TEST_ASSERT_TRUE(duplicate.lastBody.indexOf("network_slot_changed") >= 0);
    TEST_ASSERT_EQUAL_UINT(0, writes);
    TEST_ASSERT_EQUAL_UINT(1, wifi.radioChanges);
    TEST_ASSERT_EQUAL_STRING("fixture", manager.getWifiStaSlotPassword(0).c_str());

    WebServer intentional;
    postSavedNetwork(intentional, wifi, handleApiNetworksSave,
        R"({"index":0,"expectedSsid":"Shared","ssid":"Shared","password":""})");
    TEST_ASSERT_EQUAL_INT(200, intentional.lastStatusCode);
    TEST_ASSERT_EQUAL_STRING("", manager.getWifiStaSlotPassword(0).c_str());
}

void test_reorder_requires_every_current_slot_and_its_prior_order() {
    using namespace WifiClientApiService;
    fs::FS filesystem(g_tempRoot);
    storage.setFilesystem(&filesystem, true);
    TEST_ASSERT_TRUE(profiles.begin(storage));
    SettingsManager manager(storage, profiles);
    TEST_ASSERT_TRUE(manager.setWifiStaSlotCredentials(0, "One", "one", "One", 0));
    TEST_ASSERT_TRUE(manager.setWifiStaSlotCredentials(1, "Replacement", "two", "Two", 2));
    TEST_ASSERT_TRUE(manager.setWifiStaSlotCredentials(2, "Three", "three", "Three", 255));
    WiFiManager wifi(manager);
    const auto before = manager.get();
    unsigned writes = 0;
    mock_preferences::afterWrite() = [&](const char*) { ++writes; };
    constexpr const char* incomplete =
        R"({"updates":[{"index":2,"priority":1,"expectedSsid":"Three","expectedPriority":255}]})";
    constexpr const char* replaced =
        R"({"updates":[{"index":0,"priority":0,"expectedSsid":"One","expectedPriority":0},)"
        R"({"index":2,"priority":1,"expectedSsid":"Three","expectedPriority":255},)"
        R"({"index":1,"priority":2,"expectedSsid":"Old","expectedPriority":2}]})";
    constexpr const char* reordered =
        R"({"updates":[{"index":0,"priority":0,"expectedSsid":"One","expectedPriority":0},)"
        R"({"index":2,"priority":1,"expectedSsid":"Three","expectedPriority":3},)"
        R"({"index":1,"priority":2,"expectedSsid":"Replacement","expectedPriority":2}]})";
    for (const char* body : {incomplete, replaced, reordered}) {
        WebServer stale;
        postSavedNetwork(stale, wifi, handleApiNetworksPriorities, body);
        TEST_ASSERT_EQUAL_INT(409, stale.lastStatusCode);
        TEST_ASSERT_TRUE(stale.lastBody.indexOf("priority_conflict") >= 0);
    }
    mock_preferences::afterWrite() = {};
    TEST_ASSERT_EQUAL_UINT(0, writes);
    assertSettingsEqual(before, manager.get());

    WebServer valid;
    postSavedNetwork(valid, wifi, handleApiNetworksPriorities,
        R"({"updates":[{"index":0,"priority":0,"expectedSsid":"One","expectedPriority":0},)"
        R"({"index":2,"priority":1,"expectedSsid":"Three","expectedPriority":255},)"
        R"({"index":1,"priority":2,"expectedSsid":"Replacement","expectedPriority":2}]})");
    TEST_ASSERT_EQUAL_INT(200, valid.lastStatusCode);
    SettingsManager rebooted(storage, profiles);
    rebooted.load();
    TEST_ASSERT_EQUAL_UINT8(0, rebooted.get().wifiStaSlots[0].priority);
    TEST_ASSERT_EQUAL_UINT8(2, rebooted.get().wifiStaSlots[1].priority);
    TEST_ASSERT_EQUAL_UINT8(1, rebooted.get().wifiStaSlots[2].priority);
    TEST_ASSERT_EQUAL_STRING("two", rebooted.getWifiStaSlotPassword(1).c_str());
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    editorSlotPath = argv[1];
    UNITY_BEGIN();
    RUN_TEST(test_omitted_password_failure_refuses_edit_and_retry_preserves_credentials);
    RUN_TEST(test_unavailable_password_listing_keeps_blank_editor_preservation);
    RUN_TEST(test_healthy_omitted_and_explicit_empty_passwords_keep_their_meaning);
    RUN_TEST(test_implicit_priority_uses_current_saved_order_and_appends_after_legacy_ties);
    RUN_TEST(test_stale_identity_and_full_priority_range_cannot_write);
    RUN_TEST(test_stale_delete_and_test_leave_replacement_durable);
    RUN_TEST(test_saved_network_create_conflict_preserves_acknowledged_password);
    RUN_TEST(test_reorder_requires_every_current_slot_and_its_prior_order);
    return UNITY_END();
}

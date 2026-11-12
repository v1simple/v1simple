#define main original_settings_suite_main
#include "test/test_settings_persistence/test_settings_persistence.cpp"
#undef main

#include <fstream>
#include "src/modules/wifi/wifi_saved_network_mutation_policy.h"

// Compile the real saved-network methods and settings persistence owner.
// Only unrelated radio lifecycle actions are recorded by this adapter.
class WiFiManager {
public:
    explicit WiFiManager(SettingsManager& settings) : settings_(settings) {}
    SettingsManager& settings_;
    unsigned radioChanges = 0;
    struct StationEffects {
        unsigned& changes;
        WifiSavedNetworkMutationPolicy::Decision savedNetworkMutation(size_t) const { return {}; }
        void savedNetworkMutationCommitted(int, size_t, WifiSavedNetworkMutationPolicy::Decision, const char*) {
            ++changes;
        }
    } station_{radioChanges};
    int stationEnvironment() const { return 0; }
    std::vector<WifiClientApiService::SavedNetworkSlotPayload> getSavedNetworkSlots() const;
    int selectSlotForNetworkConnect(const String&) const;
    bool upsertSavedNetwork(const WifiClientApiService::SavedNetworkUpsertPayload&, size_t&);
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
    request.ssid = "LookupNet";
    request.hasLabel = true;
    request.label = "Renamed";
    request.hasPriority = true;
    request.priority = 2;
    return request;
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
            const bool saved = wifi.upsertSavedNetwork(labelEdit(slot), selected);
            mock_preferences::afterWrite() = {};
            TEST_ASSERT_FALSE(saved);
            TEST_ASSERT_EQUAL_UINT(kWifiStaSlotCount, selected);
            TEST_ASSERT_EQUAL_UINT(0, writes);
            TEST_ASSERT_EQUAL_UINT(0, wifi.radioChanges);
            assertSettingsEqual(before, manager.get());
            TEST_ASSERT_EQUAL_STRING(passwordBefore.c_str(),
                mock_preferences::getString(WIFI_CLIENT_NS, kNvsWifiStaSlotPassword[slot]).c_str());
            TEST_ASSERT_EQUAL_STRING(sdBefore.c_str(), readFileToString(filesystem, WIFI_CLIENT_SD_SECRET_PATH).c_str());
            TEST_ASSERT_EQUAL_STRING("<current-password>", manager.getWifiStaSlotPassword(slot).c_str());

            TEST_ASSERT_TRUE(wifi.upsertSavedNetwork(labelEdit(slot), selected));
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
        TEST_ASSERT_TRUE(wifi.upsertSavedNetwork(request, selected));
        TEST_ASSERT_EQUAL_STRING(password, manager.getWifiStaSlotPassword(0).c_str());
        request.hasPassword = true;
        request.password = "";
        TEST_ASSERT_TRUE(wifi.upsertSavedNetwork(request, selected));
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
    edit.hasExpectedSsid = true;
    edit.expectedSsid = "LookupNet";
    TEST_ASSERT_TRUE(wifi.upsertSavedNetwork(edit, selected));
    TEST_ASSERT_EQUAL_UINT(0, selected);

    WifiClientApiService::SavedNetworkUpsertPayload add;
    add.ssid = "New network";
    add.hasPassword = true;
    add.password = "<fixture-password>";
    TEST_ASSERT_TRUE(wifi.upsertSavedNetwork(add, selected));
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
    edit.hasExpectedSsid = true;
    edit.expectedSsid = "LookupNet";
    WifiClientApiService::SavedNetworkUpsertPayload add;
    add.ssid = "New network";
    add.hasPassword = true;
    add.password = "new";
    size_t writes = 0;
    mock_preferences::afterWrite() = [&](const char*) { ++writes; };
    size_t selected = kWifiStaSlotCount;
    const bool staleSaved = wifi.upsertSavedNetwork(edit, selected);
    const bool implicitSaved = wifi.upsertSavedNetwork(add, selected);
    mock_preferences::afterWrite() = {};
    TEST_ASSERT_FALSE(staleSaved);
    TEST_ASSERT_FALSE(implicitSaved);
    TEST_ASSERT_EQUAL_UINT(kWifiStaSlotCount, selected);
    TEST_ASSERT_EQUAL_UINT(0, writes);
    TEST_ASSERT_EQUAL_UINT(0, wifi.radioChanges);

    add.hasPriority = true;
    add.priority = 4;
    TEST_ASSERT_TRUE(wifi.upsertSavedNetwork(add, selected));
    TEST_ASSERT_EQUAL_UINT(1, selected);
    SettingsManager rebooted(storage, profiles);
    rebooted.load();
    TEST_ASSERT_EQUAL_STRING("Replacement", rebooted.get().wifiStaSlots[0].ssid.c_str());
    TEST_ASSERT_EQUAL_STRING("replacement", rebooted.getWifiStaSlotPassword(0).c_str());
    TEST_ASSERT_EQUAL_UINT8(255, rebooted.get().wifiStaSlots[0].priority);
    TEST_ASSERT_EQUAL_UINT8(4, rebooted.get().wifiStaSlots[1].priority);
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
    return UNITY_END();
}

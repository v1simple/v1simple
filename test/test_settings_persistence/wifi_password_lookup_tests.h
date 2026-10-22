#pragma once

namespace {

// Exercise a supported history: a newer password is committed while the SD
// card is absent, then the card containing the older password is reattached.
void seedWifiPasswordWithStaleSd(SettingsManager& manager, fs::FS& filesystem,
                                 size_t slot, const char* password = "<current-password>") {
    storage.setFilesystem(&filesystem, true);
    TEST_ASSERT_TRUE(profiles.begin(storage));
    TEST_ASSERT_TRUE(manager.setWifiStaSlotCredentials(slot, "LookupNet", "old-password", "Saved", 0));
    storage.setFilesystem(&filesystem, false);
    TEST_ASSERT_TRUE(manager.setWifiStaSlotCredentials(slot, "LookupNet", password, "Saved", 0));
    storage.setFilesystem(&filesystem, true);
    TEST_ASSERT_EQUAL_STRING(password, manager.getWifiStaSlotPassword(slot).c_str());
    TEST_ASSERT_EQUAL_STRING("old-password",
        decodeObfuscatedFromStorage(loadWifiClientSecretFromSD(storage, "LookupNet", slot)).c_str());
}

void moveWifiPasswordToLegacyKey(size_t slot) {
    Preferences prefs;
    TEST_ASSERT_TRUE(prefs.begin(WIFI_CLIENT_NS, false));
    const String value = prefs.getString(kNvsWifiStaSlotPassword[slot]);
    prefs.putString(kNvsWifiPassword, value);
    TEST_ASSERT_TRUE(prefs.remove(kNvsWifiStaSlotPassword[slot]));
    prefs.end();
}

enum class WifiPasswordReadFault { Open, Presence, StringSize, StringValue };

void assertWifiPasswordReadFailurePreservesAuthority(size_t slot, bool legacy, WifiPasswordReadFault fault,
                                                      esp_err_t readError = ESP_FAIL) {
    resetRuntimeState();
    static unsigned fixture = 0;
    fs::FS filesystem(g_tempRoot / ("password_lookup_" + std::to_string(++fixture)));
    SettingsManager manager(storage, profiles);
    seedWifiPasswordWithStaleSd(manager, filesystem, slot);
    if (legacy) moveWifiPasswordToLegacyKey(slot);
    const char* key = legacy ? kNvsWifiPassword : kNvsWifiStaSlotPassword[slot];
    const String before = mock_preferences::getString(WIFI_CLIENT_NS, key);
    const std::string sdBefore = readFileToString(filesystem, WIFI_CLIENT_SD_SECRET_PATH);
    size_t writes = 0;
    mock_preferences::afterWrite() = [&](const char*) { ++writes; };
    if (fault == WifiPasswordReadFault::Open) {
        mock_preferences::set_fail_read_only_begin_for_namespace(WIFI_CLIENT_NS, 1);
    } else {
        const auto stage = fault == WifiPasswordReadFault::Presence ? mock_nvs::ReadStage::Presence
                         : fault == WifiPasswordReadFault::StringSize ? mock_nvs::ReadStage::StringSize
                                                                    : mock_nvs::ReadStage::StringValue;
        mock_nvs::set_nth_read_result(WIFI_CLIENT_NS, key, readError, stage, 1);
    }

    bool readFailed = false;
    const String unavailable = manager.getWifiStaSlotPassword(slot, &readFailed);
    mock_preferences::afterWrite() = {};
    TEST_ASSERT_EQUAL_STRING("", unavailable.c_str());
    TEST_ASSERT_TRUE(readFailed);
    TEST_ASSERT_EQUAL_UINT(0, writes);
    TEST_ASSERT_EQUAL_STRING(before.c_str(), mock_preferences::getString(WIFI_CLIENT_NS, key).c_str());
    TEST_ASSERT_EQUAL_STRING(sdBefore.c_str(), readFileToString(filesystem, WIFI_CLIENT_SD_SECRET_PATH).c_str());
    TEST_ASSERT_EQUAL_STRING("<current-password>", manager.getWifiStaSlotPassword(slot, &readFailed).c_str());
    TEST_ASSERT_FALSE(readFailed);
    SettingsManager rebooted(storage, profiles);
    rebooted.load();
    TEST_ASSERT_EQUAL_STRING("<current-password>", rebooted.getWifiStaSlotPassword(slot).c_str());
}

} // namespace

void test_wifi_password_lookup_open_failure_does_not_restore_stale_sd() {
    for (size_t slot : {size_t{0}, size_t{1}})
        assertWifiPasswordReadFailurePreservesAuthority(slot, false, WifiPasswordReadFault::Open);
}

void test_wifi_password_lookup_key_and_string_read_failures_do_not_restore_stale_sd() {
    for (size_t slot : {size_t{0}, size_t{1}})
        for (auto fault : {WifiPasswordReadFault::Presence, WifiPasswordReadFault::StringSize,
                           WifiPasswordReadFault::StringValue})
            assertWifiPasswordReadFailurePreservesAuthority(slot, false, fault);
}

void test_wifi_password_lookup_legacy_read_failures_do_not_restore_stale_sd() {
    for (auto fault : {WifiPasswordReadFault::Presence, WifiPasswordReadFault::StringSize,
                       WifiPasswordReadFault::StringValue})
        assertWifiPasswordReadFailurePreservesAuthority(0, true, fault);
}

void test_wifi_password_lookup_disappearing_present_value_is_unavailable() {
    for (bool legacy : {false, true})
        for (auto fault : {WifiPasswordReadFault::StringSize, WifiPasswordReadFault::StringValue})
            assertWifiPasswordReadFailurePreservesAuthority(0, legacy, fault, ESP_ERR_NVS_NOT_FOUND);
}

void test_wifi_password_lookup_proven_absence_recovers_sd_and_heals_nvs() {
    for (bool missingNamespace : {false, true}) {
        resetRuntimeState();
        fs::FS filesystem(g_tempRoot / (missingNamespace ? "missing_namespace" : "missing_key"));
        SettingsManager manager(storage, profiles);
        seedWifiPasswordWithStaleSd(manager, filesystem, 0);
        if (missingNamespace) {
            mock_preferences::store().erase(WIFI_CLIENT_NS);
            mock_nvs::set_next_namespace_open_result(WIFI_CLIENT_NS, ESP_ERR_NVS_NOT_FOUND);
        } else {
            Preferences prefs;
            TEST_ASSERT_TRUE(prefs.begin(WIFI_CLIENT_NS, false));
            TEST_ASSERT_TRUE(prefs.remove(kNvsWifiStaSlotPassword[0]));
            prefs.end();
        }
        bool readFailed = true;
        TEST_ASSERT_EQUAL_STRING("old-password", manager.getWifiStaSlotPassword(0, &readFailed).c_str());
        TEST_ASSERT_FALSE(readFailed);
        storage.setFilesystem(&filesystem, false);
        TEST_ASSERT_EQUAL_STRING("old-password", manager.getWifiStaSlotPassword(0).c_str());
        TEST_ASSERT_TRUE(mock_preferences::namespaceHasKey(WIFI_CLIENT_NS, kNvsWifiStaSlotPassword[0]));
    }
}

void test_wifi_password_lookup_empty_and_legacy_values_remain_authoritative() {
    for (bool legacy : {false, true}) {
        for (const char* password : {"", "<current-password>"}) {
            resetRuntimeState();
            fs::FS filesystem(g_tempRoot / (std::string(legacy ? "legacy_" : "slot_") +
                                            (password[0] ? "secured" : "open")));
            SettingsManager manager(storage, profiles);
            seedWifiPasswordWithStaleSd(manager, filesystem, 0, password);
            if (legacy) moveWifiPasswordToLegacyKey(0);
            else {
                // A present primary slot must not depend on an irrelevant
                // legacy key, even if querying that legacy key would fail.
                mock_nvs::set_read_result(WIFI_CLIENT_NS, kNvsWifiPassword, ESP_FAIL);
            }
            size_t writes = 0;
            mock_preferences::afterWrite() = [&](const char*) { ++writes; };
            bool readFailed = true;
            const String actual = manager.getWifiStaSlotPassword(0, &readFailed);
            mock_preferences::afterWrite() = {};
            TEST_ASSERT_FALSE(readFailed);
            TEST_ASSERT_EQUAL_STRING(password, actual.c_str());
            TEST_ASSERT_EQUAL_UINT(0, writes);
            TEST_ASSERT_EQUAL(legacy, !mock_preferences::namespaceHasKey(WIFI_CLIENT_NS, kNvsWifiStaSlotPassword[0]));
        }
    }
}

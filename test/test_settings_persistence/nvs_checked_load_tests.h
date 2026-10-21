#pragma once

// Exercise complete load/recovery/save lifecycles for every persisted field,
// including empty strings and zero/false values that can resemble defaults.
static void assertNvsCopiesEqual(const mock_preferences::Store& expected) {
    for (const char* ns : {SETTINGS_NS_A, SETTINGS_NS_B, SETTINGS_NS_LEGACY, SETTINGS_NS_META}) {
        const auto before = expected.find(ns);
        const auto after = mock_preferences::store().find(ns);
        TEST_ASSERT_EQUAL(before != expected.end(), after != mock_preferences::store().end());
        if (before == expected.end()) continue;
        TEST_ASSERT_EQUAL_UINT(before->second.size(), after->second.size());
        for (const auto& item : before->second) {
            TEST_ASSERT_TRUE(after->second.count(item.first) != 0);
            TEST_ASSERT_EQUAL_INT(item.second.type, after->second.at(item.first).type);
            TEST_ASSERT_TRUE(item.second.value == after->second.at(item.first).value);
        }
    }
}

static void seedNvsReadLifecycle(fs::FS& filesystem, bool withSd, SettingsManager& original) {
    storage.setFilesystem(&filesystem, withSd);
    TEST_ASSERT_TRUE(profiles.begin(storage));
    V1Profile road("Road");
    road.schemaVersion = V1_PROFILE_SCHEMA_VERSION;
    TEST_ASSERT_TRUE(profiles.saveProfile(road).success);
    original.begin();
    V1Settings& value = original.mutableSettings();
    makeCurrentV21Source(value);
    value.slot0_default.profileName = "Road";
    value.slot1_highway.profileName = "Road";
    value.slot2_comfort.profileName = "Road";
    value.apSSID = "Fixture AP";
    value.apPassword = "fixture-password";
    value.proxyName = "Fixture Proxy";
    value.brightness = 81;
    value.obdMinRssi = -71;
    value.gpsBaud = 38400;
    value.colorBogey = 0x3412;
    value.wifiClientEnabled = true;
    for (size_t i = 0; i < kWifiStaSlotCount; ++i) {
        value.wifiStaSlots[i].ssid = String("Fixture network ") + String(static_cast<int>(i));
        value.wifiStaSlots[i].label = String("Saved ") + String(static_cast<int>(i));
        value.wifiStaSlots[i].priority = i;
        value.wifiStaSlots[i].lastConnectedAtSec = 100 + i;
    }
    value.refreshWifiClientAliasFromSlots();
    TEST_ASSERT_TRUE(original.save());
}

static bool compatibilityBarKey(const std::string& key) {
    for (const char* candidate : {kNvsColorBarSeg1, kNvsColorBarSeg2, kNvsColorBarSeg3, kNvsColorBarSeg4,
                                  kNvsColorBarSeg5, kNvsColorBarSeg6, kNvsColorBarSeg7, kNvsColorBarSeg8}) {
        if (key == candidate) return true;
    }
    return false;
}

static void exerciseNvsFieldReadLifecycle(const std::string& key, mock_nvs::ReadStage stage, bool withSd,
                                          bool legacyBars = false) {
    resetRuntimeState();
    std::filesystem::remove_all(g_tempRoot);
    fs::FS filesystem(g_tempRoot);
    SettingsManager original(storage, profiles);
    seedNvsReadLifecycle(filesystem, withSd, original);
    const String active = activeNamespaceOrEmpty();
    if (legacyBars) {
        Preferences prefs;
        TEST_ASSERT_TRUE(prefs.begin(active.c_str(), false));
        TEST_ASSERT_EQUAL_UINT(sizeof(int32_t), prefs.putInt(kNvsSettingsVer, 11));
        prefs.end();
    }
    // Healthy read is the expected migrated representation for legacy themes.
    SettingsManager healthy(storage, profiles);
    healthy.load();
    const V1Settings expected = healthy.get();
    const auto before = mock_preferences::store();
    const std::string sdBefore = withSd ? readFileToString(filesystem, SETTINGS_BACKUP_PATH) : "";
    mock_nvs::set_read_result(active.c_str(), key.c_str(), -77, stage);
    SettingsManager rebooted(storage, profiles);
    rebooted.begin();
    TEST_ASSERT_TRUE_MESSAGE(rebooted.recoveryPending(), key.c_str());
    assertSettingsEqual(V1Settings(), rebooted.get());
    rebooted.migrateAutoPushProfilesToV2();
    rebooted.validateProfileReferences(profiles);
    DisplaySettingsUpdate update;
    update.hasBrightness = true;
    update.brightness = 111;
    TEST_ASSERT_FALSE_MESSAGE(rebooted.applyDisplaySettingsUpdate(update).success, key.c_str());
    TEST_ASSERT_FALSE(rebooted.save());
    TEST_ASSERT_FALSE(rebooted.backupToSD());
    rebooted.serviceDeferredPersist(2000);
    rebooted.serviceDeferredBackup(2000);
    assertNvsCopiesEqual(before);
    if (withSd) TEST_ASSERT_EQUAL_STRING(sdBefore.c_str(), readFileToString(filesystem, SETTINGS_BACKUP_PATH).c_str());
    TEST_ASSERT_TRUE(mock_nvs::handles().empty());

    mock_nvs::readResults().clear();
    rebooted.serviceDeferredPersist(4000);
    TEST_ASSERT_FALSE(rebooted.recoveryPending());
    assertSettingsEqual(expected, rebooted.get());
    TEST_ASSERT_TRUE(rebooted.applyDisplaySettingsUpdate(update).success);
    SettingsManager secondReboot(storage, profiles);
    secondReboot.begin();
    V1Settings after = expected;
    after.brightness = 111;
    assertSettingsEqual(after, secondReboot.get());
    TEST_ASSERT_TRUE(mock_nvs::handles().empty());
    if (withSd) {
        JsonDocument backup;
        TEST_ASSERT_TRUE(parseBackupFile(&filesystem, SETTINGS_BACKUP_PATH, backup, false));
        TEST_ASSERT_EQUAL_STRING("Road", backup["slot0ProfileName"].as<const char*>());
        TEST_ASSERT_EQUAL_STRING("Road", backup["slot1ProfileName"].as<const char*>());
        TEST_ASSERT_EQUAL_STRING("Road", backup["slot2ProfileName"].as<const char*>());
        TEST_ASSERT_EQUAL_UINT8(111, backup["brightness"].as<uint8_t>());
    }
}

void test_every_saved_nvs_field_defers_on_read_error_and_survives_retry_save_and_reboot() {
    fs::FS filesystem(g_tempRoot);
    SettingsManager original(storage, profiles);
    seedNvsReadLifecycle(filesystem, false, original);
    const auto saved = mock_preferences::store().at(activeNamespaceOrEmpty().c_str());
    size_t cases = 0;
    for (const auto& item : saved) {
        // v12 reads the six direct colours; v11 compatibility values are covered below.
        if (compatibilityBarKey(item.first)) continue;
        for (bool withSd : {false, true}) {
            if (item.second.type == PT_STR) {
                exerciseNvsFieldReadLifecycle(item.first, mock_nvs::ReadStage::StringSize, withSd);
                exerciseNvsFieldReadLifecycle(item.first, mock_nvs::ReadStage::StringValue, withSd);
                cases += 2;
            } else {
                exerciseNvsFieldReadLifecycle(item.first, mock_nvs::ReadStage::Scalar, withSd);
                ++cases;
            }
        }
    }
    std::printf("NVS field lifecycle cases: %zu\n", cases);
}

void test_nvs_optional_presence_and_legacy_color_reads_are_checked() {
    for (const char* key : {kNvsWifiClientEnabled, kNvsWifiClientSsid, kNvsLastV1Address,
                           kNvsLegacyColorBar1, kNvsColorBarSeg1, kNvsProxyBle,
                           kNvsProxyName, kNvsBrightness, kNvsAutoPush}) {
        for (bool withSd : {false, true}) {
            exerciseNvsFieldReadLifecycle(key, mock_nvs::ReadStage::Presence, withSd);
        }
    }
    for (const char* key : {kNvsColorBarSeg1, kNvsColorBarSeg2, kNvsColorBarSeg3, kNvsColorBarSeg4,
                           kNvsColorBarSeg5, kNvsColorBarSeg6, kNvsColorBarSeg7, kNvsColorBarSeg8}) {
        for (bool withSd : {false, true}) {
            exerciseNvsFieldReadLifecycle(key, mock_nvs::ReadStage::Scalar, withSd, true);
        }
    }
    // The restore marker is normally absent but is still a required checked read.
    exerciseNvsFieldReadLifecycle(kNvsRestorePending, mock_nvs::ReadStage::Scalar, true);
}

void test_nvs_missing_optional_fields_keep_legacy_default_semantics() {
    fs::FS filesystem(g_tempRoot);
    SettingsManager original(storage, profiles);
    seedNvsReadLifecycle(filesystem, false, original);
    const auto saved = mock_preferences::store();
    const String active = activeNamespaceOrEmpty();
    for (const auto& item : saved.at(active.c_str())) {
        if (item.first == kNvsValid || item.first == kNvsSettingsGeneration ||
            item.first == kNvsCommittedGeneration || item.first == kNvsSettingsVer) continue;
        mock_preferences::store() = saved;
        mock_preferences::store().at(active.c_str()).erase(item.first);
        SettingsManager rebooted(storage, profiles);
        rebooted.load();
        TEST_ASSERT_FALSE_MESSAGE(rebooted.recoveryPending(), item.first.c_str());
    }
    // Proven absence chooses defaults, including an intentionally empty profile.
    mock_preferences::store() = saved;
    auto& copy = mock_preferences::store().at(active.c_str());
    copy.erase(kNvsSlot2Profile);
    copy.erase(kNvsWifiStaSlotPriority[2]);
    copy.erase(kNvsRestoreCommitWatermark);
    copy.erase(kNvsProfileDeleteCommitWatermark);
    SettingsManager rebooted(storage, profiles);
    rebooted.begin();
    TEST_ASSERT_FALSE(rebooted.recoveryPending());
    TEST_ASSERT_EQUAL_STRING("", rebooted.get().slot2_comfort.profileName.c_str());
    TEST_ASSERT_EQUAL_UINT8(2, rebooted.get().wifiStaSlots[2].priority);
    TEST_ASSERT_TRUE(rebooted.setStealthEnabled(true).success);
    SettingsManager next(storage, profiles);
    next.begin();
    TEST_ASSERT_EQUAL_STRING("Road", next.get().slot0_default.profileName.c_str());
    TEST_ASSERT_EQUAL_STRING("", next.get().slot2_comfort.profileName.c_str());
}

void test_nvs_wrong_types_never_publish_a_partial_snapshot() {
    fs::FS filesystem(g_tempRoot);
    SettingsManager original(storage, profiles);
    seedNvsReadLifecycle(filesystem, false, original);
    const auto saved = mock_preferences::store();
    const String active = activeNamespaceOrEmpty();
    for (const char* key : {kNvsSlot1Profile, kNvsObdMinRssi, kNvsBrightness, kNvsProxyBle,
                           kNvsColorBogey, kNvsActiveSlot, kNvsGpsBaud, kNvsRestoreCommitWatermark}) {
        mock_preferences::store() = saved;
        Preferences prefs;
        TEST_ASSERT_TRUE(prefs.begin(active.c_str(), false));
        const uint8_t blob[] = {1, 2, 3};
        TEST_ASSERT_EQUAL_UINT(sizeof(blob), prefs.putBytes(key, blob, sizeof(blob)));
        prefs.end();
        const auto before = mock_preferences::store();
        SettingsManager manager(storage, profiles);
        manager.load();
        TEST_ASSERT_TRUE_MESSAGE(manager.recoveryPending(), key);
        assertSettingsEqual(V1Settings(), manager.get());
        TEST_ASSERT_FALSE(manager.setStealthEnabled(true).success);
        assertNvsCopiesEqual(before);
    }
}

void test_legacy_selector_read_failure_does_not_choose_a_different_namespace() {
    writeHealthySettingsCopy(SETTINGS_NS_A, 71, SETTINGS_VERSION);
    writeHealthySettingsCopy(SETTINGS_NS_B, 93, SETTINGS_VERSION);
    Preferences prefs;
    TEST_ASSERT_TRUE(prefs.begin(SETTINGS_NS_META, false));
    TEST_ASSERT_GREATER_THAN(0, prefs.putString(kNvsMetaActive, SETTINGS_NS_A));
    prefs.end();
    const auto before = mock_preferences::store();
    for (auto stage : {mock_nvs::ReadStage::StringSize, mock_nvs::ReadStage::StringValue}) {
        mock_nvs::set_read_result(SETTINGS_NS_META, kNvsMetaActive, -77, stage);
        SettingsManager manager(storage, profiles);
        manager.load();
        TEST_ASSERT_TRUE(manager.recoveryPending());
        TEST_ASSERT_FALSE(manager.setStealthEnabled(true).success);
        assertNvsCopiesEqual(before);
        mock_nvs::readResults().clear();
    }
    SettingsManager recovered(storage, profiles);
    recovered.begin();
    TEST_ASSERT_EQUAL_UINT8(71, recovered.get().brightness);
    TEST_ASSERT_TRUE(recovered.setStealthEnabled(true).success);
    SettingsManager rebooted(storage, profiles);
    rebooted.begin();
    TEST_ASSERT_EQUAL_UINT8(71, rebooted.get().brightness);
}

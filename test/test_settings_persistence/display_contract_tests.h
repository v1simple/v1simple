// Independent installed-contract vectors. These deliberately do not iterate the
// production descriptors: an omitted or miswired row must fail at its consumer.
namespace {
struct DisplayColorExpectation {
    const char* http;
    const char* nvs;
    const char* backup;
    uint16_t V1Settings::* member;
    uint16_t defaultValue;
    bool critical;
};
constexpr DisplayColorExpectation kDisplayColorExpectations[] = {
    {"bogey", "colorBogey", "colorBogey", &V1Settings::colorBogey, 0xF800, true},
    {"freq", "colorFreq", "colorFrequency", &V1Settings::colorFrequency, 0xF800, true},
    {"arrowFront", "colorArrF", "colorArrowFront", &V1Settings::colorArrowFront, 0xF800, true},
    {"arrowSide", "colorArrS", "colorArrowSide", &V1Settings::colorArrowSide, 0xF800, true},
    {"arrowRear", "colorArrR", "colorArrowRear", &V1Settings::colorArrowRear, 0xF800, true},
    {"bandL", "colorBandL", "colorBandL", &V1Settings::colorBandL, 0x001F, true},
    {"bandKa", "colorBandKa", "colorBandKa", &V1Settings::colorBandKa, 0xF800, true},
    {"bandK", "colorBandK", "colorBandK", &V1Settings::colorBandK, 0x001F, true},
    {"bandX", "colorBandX", "colorBandX", &V1Settings::colorBandX, 0x07E0, true},
    {"bandPhoto", "colorBandP", "colorBandPhoto", &V1Settings::colorBandPhoto, 0x780F, true},
    {"wifiConnected", "colorWiFiC", "colorWiFiConnected", &V1Settings::colorWiFiConnected, 0x07E0, false},
    {"bleConnected", "colorBleC", "colorBleConnected", &V1Settings::colorBleConnected, 0x07E0, false},
    {"bleDisconnected", "colorBleD", "colorBleDisconnected", &V1Settings::colorBleDisconnected, 0x001F, false},
    {"muted", "colorMuted", "colorMuted", &V1Settings::colorMuted, 0x4A49, false},
    {"persisted", "colorPersist", "colorPersisted", &V1Settings::colorPersisted, 0x4208, false},
    {"volumeMain", "colorVolMain", "colorVolumeMain", &V1Settings::colorVolumeMain, 0xF800, false},
    {"volumeMute", "colorVolMute", "colorVolumeMute", &V1Settings::colorVolumeMute, 0x7BEF, false},
    {"rssiV1", "colorRssiV1", "colorRssiV1", &V1Settings::colorRssiV1, 0x07E0, false},
    {"rssiProxy", "colorRssiPrx", "colorRssiProxy", &V1Settings::colorRssiProxy, 0x001F, false},
    {"obd", "colorObd", "colorObd", &V1Settings::colorObd, 0x001F, true},
    {"alpConnected", "colorAlpCon", "colorAlpConnected", &V1Settings::colorAlpConnected, 0x07E0, true},
    {"alpDli", "colorAlpDli", "colorAlpDli", &V1Settings::colorAlpDli, 0xFD20, true},
    {"alpLidActive", "colorAlpLid", "colorAlpLidActive", &V1Settings::colorAlpLidActive, 0x001F, true},
    {"alpAlert", "colorAlpAlt", "colorAlpAlert", &V1Settings::colorAlpAlert, 0xF800, true},
};
struct DisplayFlagExpectation {
    const char* key;
    const char* nvs;
    bool V1Settings::* member;
    bool critical;
};
constexpr DisplayFlagExpectation kDisplayFlagExpectations[] = {
    {"freqUseBandColor", "freqBandCol", &V1Settings::freqUseBandColor, false},
    {"hideWifiIcon", "hideWifi", &V1Settings::hideWifiIcon, true},
    {"hideProfileIndicator", "hideProfile", &V1Settings::hideProfileIndicator, true},
    {"hideBatteryIcon", "hideBatt", &V1Settings::hideBatteryIcon, true},
    {"showBatteryPercent", "battPct", &V1Settings::showBatteryPercent, true},
    {"hideBleIcon", "hideBle", &V1Settings::hideBleIcon, true},
    {"hideVolumeIndicator", "hideVol", &V1Settings::hideVolumeIndicator, true},
    {"hideRssiIndicator", "hideRssi", &V1Settings::hideRssiIndicator, true},
};

WifiDisplayColorsApiService::Runtime displayContractRuntime(SettingsManager& manager) {
    WifiDisplayColorsApiService::Runtime runtime{};
    runtime.getSettings = [](void* ctx) -> const V1Settings& { return static_cast<SettingsManager*>(ctx)->get(); };
    runtime.getSettingsCtx = &manager;
    runtime.applySettingsUpdate = [](const DisplaySettingsUpdate& update, void* ctx) {
        return static_cast<SettingsManager*>(ctx)->applyDisplaySettingsUpdate(
            update, SettingsPersistMode::ImmediateNvsDeferredBackup);
    };
    runtime.applySettingsUpdateCtx = &manager;
    runtime.resetDisplaySettings = [](void* ctx) {
        return static_cast<SettingsManager*>(ctx)->resetDisplaySettings(SettingsPersistMode::ImmediateNvsDeferredBackup);
    };
    runtime.resetDisplaySettingsCtx = &manager;
    return runtime;
}

std::string displayContractProjectFile(const char* path) {
    std::ifstream input(std::filesystem::path(PROJECT_DIR) / path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

void assertDisplayContractDefaults(const V1Settings& actual) {
    for (const auto& color : kDisplayColorExpectations) {
        TEST_ASSERT_EQUAL_UINT16_MESSAGE(color.defaultValue, actual.*(color.member), color.http);
    }
    const uint16_t bars[] = {0x07E0, 0x07E0, 0xFFE0, 0xFFE0, 0xF800, 0xF800};
    TEST_ASSERT_EQUAL_UINT16_ARRAY(bars, actual.colorBars, 6);
}
} // namespace

void test_display_backup_literal_keys_retain_static_storage() {
    fs::FS filesystem(g_tempRoot);
    storage.setFilesystem(&filesystem, false);
    TEST_ASSERT_TRUE(profiles.begin(storage));
    V1Settings source;
    source.autoPushProfileSchemaVersion = V1_PROFILE_SCHEMA_VERSION;
    for (const auto transport : {BackupPayloadBuilder::BackupTransport::HttpDownload,
                                 BackupPayloadBuilder::BackupTransport::SdBackup}) {
        JsonDocument output;
        TEST_ASSERT_TRUE(BackupPayloadBuilder::buildBackupDocument(output, source, profiles, transport, 1234).safeToCommit);
        const auto assertStaticKey = [&](const char* expected) {
            bool found = false;
            for (JsonPairConst field : output.as<JsonObjectConst>()) {
                if (std::strcmp(field.key().c_str(), expected) != 0) continue;
                found = true;
                TEST_ASSERT_TRUE_MESSAGE(field.key().isStatic(), expected);
                break;
            }
            TEST_ASSERT_TRUE_MESSAGE(found, expected);
        };
        // These names were literal-array keys before descriptors. Verify their
        // allocation policy on the real writer, independently of its metadata.
        assertStaticKey("brightness");
        assertStaticKey("colorWiFiIcon");
        for (const auto& color : kDisplayColorExpectations) assertStaticKey(color.backup);
        for (const auto& flag : kDisplayFlagExpectations) assertStaticKey(flag.key);
        // Bar names previously came from a local formatting buffer. Their new
        // permanent literals can also be retained without key allocations.
        for (const char* key : {"colorBar1", "colorBar2", "colorBar3", "colorBar4", "colorBar5", "colorBar6",
                                "colorBarS1", "colorBarS2", "colorBarS3", "colorBarS4", "colorBarS5", "colorBarS6",
                                "colorBarS7", "colorBarS8"}) assertStaticKey(key);
    }
}

void test_display_http_persistence_matches_independent_browser_fixture() {
    fs::FS filesystem(g_tempRoot);
    storage.setFilesystem(&filesystem, true);
    TEST_ASSERT_TRUE(profiles.begin(storage));
    SettingsManager manager(storage, profiles);
    TEST_ASSERT_TRUE(manager.saveDeferredBackup());
    const std::string fixture = displayContractProjectFile("interface/src/test/fixtures/wifi-api.json");
    TEST_ASSERT_FALSE(fixture.empty());
    JsonDocument vectors;
    TEST_ASSERT_FALSE(deserializeJson(vectors, fixture));
    JsonObjectConst expected = vectors["scenarios"]["display_colors_success"]["GET /api/display/settings"][0];
    TEST_ASSERT_FALSE(expected.isNull());
    WebServer save(80);
    for (JsonPairConst field : expected["body"].as<JsonObjectConst>()) {
        const String value = field.value().is<bool>() ? String(field.value().as<bool>() ? "true" : "false")
                                                     : String(field.value().as<int>());
        save.setArg(field.key().c_str(), value);
    }
    WifiDisplayColorsApiService::handleApiSave(save, displayContractRuntime(manager), nullptr, nullptr);
    TEST_ASSERT_EQUAL_INT(200, save.lastStatusCode);
    SettingsManager reboot(storage, profiles);
    reboot.load();
    WebServer get(80);
    WifiDisplayColorsApiService::handleApiGet(get, displayContractRuntime(reboot));
    TEST_ASSERT_EQUAL_INT(expected["status"].as<int>(), get.lastStatusCode);
    TEST_ASSERT_EQUAL_STRING(expected["contentType"].as<const char*>(), get.lastContentType.c_str());
    JsonDocument actual;
    TEST_ASSERT_FALSE(deserializeJson(actual, get.lastBody.c_str()));
    // Independent signed-integer RGB565 interpolation for the fixture's bars:
    // green to yellow yields red=16; yellow to red yields green=32.
    TEST_ASSERT_EQUAL_UINT16(34784, actual["barS3"].as<uint16_t>());
    TEST_ASSERT_EQUAL_UINT16(64512, actual["barS6"].as<uint16_t>());
    TEST_ASSERT_EQUAL_UINT(expected["body"].size(), actual.size());
    for (JsonPairConst field : expected["body"].as<JsonObjectConst>()) {
        TEST_ASSERT_TRUE_MESSAGE(actual[field.key().c_str()] == field.value(), field.key().c_str());
    }
}

void test_display_all_fields_cross_http_nvs_backup_and_reset_boundaries() {
    fs::FS filesystem(g_tempRoot);
    storage.setFilesystem(&filesystem, true);
    TEST_ASSERT_TRUE(profiles.begin(storage));
    SettingsManager manager(storage, profiles);
    manager.mutableSettings().autoPushProfileSchemaVersion = V1_PROFILE_SCHEMA_VERSION;
    TEST_ASSERT_TRUE(manager.saveDeferredBackup());
    WebServer save(80);
    unsigned index = 0;
    for (const auto& color : kDisplayColorExpectations) save.setArg(color.http, String(0x1101 + index++));
    for (const auto& flag : kDisplayFlagExpectations) save.setArg(flag.key, "true");
    save.setArg("brightness", "123");
    save.setArg("wifiIcon", "22222"); // Canonical key wins even if both are present.
    for (unsigned bar = 0; bar < 6; ++bar) {
        save.setArg(("bar" + std::to_string(bar + 1)).c_str(), String(0x2101 + bar));
    }
    WifiDisplayColorsApiService::handleApiSave(save, displayContractRuntime(manager), nullptr, nullptr);
    TEST_ASSERT_EQUAL_INT(200, save.lastStatusCode);
    SettingsManager reboot(storage, profiles);
    reboot.load();
    JsonDocument http;
    WebServer get(80);
    WifiDisplayColorsApiService::handleApiGet(get, displayContractRuntime(reboot));
    TEST_ASSERT_FALSE(deserializeJson(http, get.lastBody.c_str()));
    Preferences saved;
    TEST_ASSERT_TRUE(saved.begin(activeNamespaceOrEmpty().c_str(), true));
    JsonDocument backup;
    TEST_ASSERT_TRUE(BackupPayloadBuilder::buildBackupDocument(backup, reboot.get(), profiles,
        BackupPayloadBuilder::BackupTransport::HttpDownload, 1234).safeToCommit);
    index = 0;
    for (const auto& color : kDisplayColorExpectations) {
        const uint16_t expected = 0x1101 + index++;
        TEST_ASSERT_EQUAL_UINT16_MESSAGE(expected, http[color.http].as<uint16_t>(), color.http);
        TEST_ASSERT_EQUAL_UINT16_MESSAGE(expected, reboot.get().*(color.member), color.http);
        TEST_ASSERT_EQUAL_UINT16_MESSAGE(expected, saved.getUShort(color.nvs), color.nvs);
        TEST_ASSERT_EQUAL_UINT16_MESSAGE(expected, backup[color.backup].as<uint16_t>(), color.backup);
    }
    for (const auto& flag : kDisplayFlagExpectations) {
        TEST_ASSERT_TRUE_MESSAGE(http[flag.key].as<bool>(), flag.key);
        TEST_ASSERT_TRUE_MESSAGE(reboot.get().*(flag.member), flag.key);
        TEST_ASSERT_TRUE_MESSAGE(saved.getBool(flag.nvs), flag.nvs);
        TEST_ASSERT_TRUE_MESSAGE(backup[flag.key].as<bool>(), flag.key);
    }
    TEST_ASSERT_EQUAL_UINT8(123, saved.getUChar("brightness"));
    TEST_ASSERT_EQUAL_UINT8(123, backup["brightness"].as<uint8_t>());
    saved.end();
    for (const auto transport : {BackupPayloadBuilder::BackupTransport::HttpDownload,
                                 BackupPayloadBuilder::BackupTransport::SdBackup}) {
        JsonDocument output;
        TEST_ASSERT_TRUE(BackupPayloadBuilder::buildBackupDocument(output, reboot.get(), profiles, transport, 1234).safeToCommit);
        std::string serialized;
        serializeJson(output, serialized);
        const char* name = transport == BackupPayloadBuilder::BackupTransport::HttpDownload
            ? "display-contract-http.json" : "display-contract-sd.json";
        // Exact synthetic bytes from the pre-refactor producer, including order
        // and SD CRC. Expectations are not generated by the descriptors.
        const std::string expected = displayContractProjectFile(
            (std::string("test/test_settings_persistence/fixtures/") + name).c_str());
        TEST_ASSERT_FALSE(expected.empty());
        TEST_ASSERT_EQUAL_STRING(expected.c_str(), serialized.c_str());
        V1Settings restored;
        applyBackupDisplayFields(output, restored, BackupRestoreScope::Full);
        for (const auto& color : kDisplayColorExpectations) {
            TEST_ASSERT_EQUAL_UINT16(reboot.get().*(color.member), restored.*(color.member));
        }
        for (const auto& flag : kDisplayFlagExpectations) TEST_ASSERT_EQUAL(reboot.get().*(flag.member), restored.*(flag.member));
        TEST_ASSERT_EQUAL_UINT16_ARRAY(reboot.get().colorBars, restored.colorBars, 6);
    }
    WebServer black(80);
    for (const auto& color : kDisplayColorExpectations) black.setArg(color.http, "0");
    black.setArg("wifiIcon", "65535"); // Present canonical zero still wins over legacy.
    WifiDisplayColorsApiService::handleApiSave(black, displayContractRuntime(reboot), nullptr, nullptr);
    TEST_ASSERT_EQUAL_INT(200, black.lastStatusCode);
    index = 0;
    for (const auto& color : kDisplayColorExpectations) {
        TEST_ASSERT_EQUAL_UINT16(0x1101 + index++, reboot.get().*(color.member));
    }
    WebServer reset(80);
    WifiDisplayColorsApiService::handleApiReset(reset, displayContractRuntime(reboot), nullptr, nullptr);
    TEST_ASSERT_EQUAL_INT(200, reset.lastStatusCode);
    assertDisplayContractDefaults(reboot.get());
    TEST_ASSERT_EQUAL_UINT8(123, reboot.get().brightness);
    TEST_ASSERT_FALSE(reboot.get().freqUseBandColor);
    const auto repeatedReset = reboot.resetDisplaySettings(SettingsPersistMode::Deferred);
    TEST_ASSERT_TRUE(repeatedReset.success);
    TEST_ASSERT_TRUE(repeatedReset.changed);
    TEST_ASSERT_TRUE(reboot.deferredPersistPending());
    for (const auto& flag : kDisplayFlagExpectations) {
        if (flag.critical) TEST_ASSERT_TRUE_MESSAGE(reboot.get().*(flag.member), flag.key);
    }
}

void test_display_missing_black_and_legacy_critical_policies_remain_distinct() {
    V1Settings defaults;
    assertDisplayContractDefaults(defaults);
    TEST_ASSERT_EQUAL_UINT8(200, defaults.brightness);
    for (const auto& flag : kDisplayFlagExpectations) TEST_ASSERT_FALSE(defaults.*(flag.member));
    SettingsManager missing(storage, profiles);
    missing.load();
    assertDisplayContractDefaults(missing.get());
    Preferences active;
    TEST_ASSERT_TRUE(active.begin(SETTINGS_NS_A, false));
    TEST_ASSERT_TRUE(active.clear());
    TEST_ASSERT_GREATER_THAN(0, active.putInt("nvsValid", SETTINGS_VERSION));
    TEST_ASSERT_GREATER_THAN(0, active.putInt("settingsVer", SETTINGS_VERSION));
    for (const auto& color : kDisplayColorExpectations) TEST_ASSERT_GREATER_THAN(0, active.putUShort(color.nvs, 0));
    TEST_ASSERT_GREATER_THAN(0, active.putUChar("brightness", 0));
    active.end();
    Preferences meta;
    TEST_ASSERT_TRUE(meta.begin(SETTINGS_NS_META, false));
    TEST_ASSERT_GREATER_THAN(0, meta.putString("active", SETTINGS_NS_A));
    meta.end();
    SettingsManager blackNvs(storage, profiles);
    blackNvs.load();
    assertDisplayContractDefaults(blackNvs.get());
    TEST_ASSERT_EQUAL_UINT8(1, blackNvs.get().brightness);
    JsonDocument legacy;
    for (const auto& color : kDisplayColorExpectations) legacy[color.backup] = 0;
    for (const auto& flag : kDisplayFlagExpectations) legacy[flag.key] = "true";
    legacy["brightness"] = 0;
    legacy["colorBar1"] = 0;
    for (const auto scope : {BackupRestoreScope::Full, BackupRestoreScope::CriticalRecovery}) {
        V1Settings target;
        for (const auto& color : kDisplayColorExpectations) target.*(color.member) = 0x1234;
        target.colorBars[0] = 0x2345;
        applyBackupDisplayFields(legacy, target, scope);
        for (const auto& color : kDisplayColorExpectations) {
            TEST_ASSERT_EQUAL_UINT16_MESSAGE(scope == BackupRestoreScope::Full || color.critical ? color.defaultValue : 0x1234,
                                             target.*(color.member), color.http);
        }
        for (const auto& flag : kDisplayFlagExpectations) {
            TEST_ASSERT_EQUAL_MESSAGE(scope == BackupRestoreScope::Full || flag.critical, target.*(flag.member), flag.key);
        }
        TEST_ASSERT_EQUAL_UINT8(1, target.brightness);
        TEST_ASSERT_EQUAL_UINT16(scope == BackupRestoreScope::Full ? 0x07E0 : 0x2345, target.colorBars[0]);
    }
    JsonDocument unsuitable;
    unsuitable["colorBogey"] = "12";
    unsuitable["colorWiFiConnected"] = "invalid";
    unsuitable["colorWiFiIcon"] = 0x4567;
    V1Settings target;
    target.colorBogey = 0x1234;
    target.colorFrequency = 0x2345;
    applyBackupDisplayFields(unsuitable, target, BackupRestoreScope::Full);
    TEST_ASSERT_EQUAL_UINT16(0x1234, target.colorBogey);
    TEST_ASSERT_EQUAL_UINT16(0x2345, target.colorFrequency);
    TEST_ASSERT_EQUAL_UINT16(0x4567, target.colorWiFiConnected);
}

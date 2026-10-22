#pragma once

namespace {

void exerciseDeleteReadyReadFailure(bool fail, bool freshBootFirst) {
    fs::FS primary(g_tempRoot / "delete_read_retry");
    V1DeviceStore devices;
    TEST_ASSERT_TRUE(devices.begin(&primary));
    TEST_ASSERT_TRUE(devices.upsertDevice("AA:BB:CC:DD:EE:FF"));
    TEST_ASSERT_TRUE(devices.upsertDevice("11:22:33:44:55:66"));
    seedRuntimeFallback("AA:BB:CC:DD:EE:FF");
    fs::mock_set_fs_write_budget(20);
    const auto first = settings.deleteV1DeviceTransactional("AA:BB:CC:DD:EE:FF", devices);
    TEST_ASSERT_EQUAL_INT(V1DeviceMutationStatus::DurablePending, first.status);
    fs::mock_reset_fs_write_budget();
    const auto before = mock_preferences::store().at(kSettingsV1RuntimeNamespace);
    const std::string catalogBefore = readFileToString(primary, "/v1devices.json");
    if (fail) mock_preferences::set_scalar_read_fault(kSettingsV1RuntimeNamespace,
        kNvsV1DeleteReady, PT_U8, ESP_FAIL);
    if (freshBootFirst) {
        V1DeviceStore rebooted;
        TEST_ASSERT_TRUE(rebooted.begin(&primary));
        SettingsManager freshSettings(storage, profiles);
        freshSettings.begin();
        TEST_ASSERT_EQUAL(!fail, freshSettings.resolvePendingV1DeviceDelete(rebooted));
    } else {
        const auto second = settings.deleteV1DeviceTransactional("11:22:33:44:55:66", devices);
        TEST_ASSERT_EQUAL_INT(fail ? V1DeviceMutationStatus::Unavailable : V1DeviceMutationStatus::FullyMirrored,
                             second.status);
    }
    if (fail) {
        const auto& after = mock_preferences::store().at(kSettingsV1RuntimeNamespace);
        TEST_ASSERT_EQUAL_UINT(before.size(), after.size());
        for (const auto& entry : before) {
            TEST_ASSERT_TRUE(after.count(entry.first) == 1);
            TEST_ASSERT_EQUAL_INT(entry.second.type, after.at(entry.first).type);
            TEST_ASSERT_TRUE(entry.second.value == after.at(entry.first).value);
        }
        TEST_ASSERT_EQUAL_STRING(catalogBefore.c_str(), readFileToString(primary, "/v1devices.json").c_str());
    }
    // Fresh settings/catalog owners must finish the acknowledged first request.
    // A healthy later request may then delete the second address without
    // replacing the first operation's durable authority.
    V1DeviceStore finalDevices;
    TEST_ASSERT_TRUE(finalDevices.begin(&primary));
    SettingsManager finalSettings(storage, profiles);
    finalSettings.begin();
    TEST_ASSERT_TRUE(finalSettings.resolvePendingV1DeviceDelete(finalDevices));
    bool firstPresent = true;
    TEST_ASSERT_TRUE(finalDevices.containsDeviceChecked("AA:BB:CC:DD:EE:FF", firstPresent));
    TEST_ASSERT_FALSE(firstPresent);
    TEST_ASSERT_TRUE(finalSettings.deleteV1DeviceTransactional("11:22:33:44:55:66", finalDevices).fullyMirrored());
    TEST_ASSERT_FALSE(mock_preferences::namespaceHasKey(kSettingsV1RuntimeNamespace, kNvsV1DeleteReady));
    V1DeviceStore lastBoot;
    TEST_ASSERT_TRUE(lastBoot.begin(&primary));
    TEST_ASSERT_TRUE(lastBoot.listDevices().empty());
}

} // namespace

void test_device_delete_ready_read_failure_preserves_prior_acknowledged_intent() {
    exerciseDeleteReadyReadFailure(true, false);
}
void test_device_delete_ready_read_failure_on_fresh_boot_retries_without_loss() {
    exerciseDeleteReadyReadFailure(true, true);
}
void test_device_delete_healthy_lookup_finishes_prior_intent_before_next_delete() {
    exerciseDeleteReadyReadFailure(false, false);
}
void test_device_delete_healthy_fresh_boot_finishes_pending_intent() {
    exerciseDeleteReadyReadFailure(false, true);
}

void test_device_delete_presence_lookup_error_retains_intent() {
    fs::FS primary(g_tempRoot / "delete_presence_error");
    V1DeviceStore devices;
    TEST_ASSERT_TRUE(devices.begin(&primary));
    TEST_ASSERT_TRUE(devices.upsertDevice("AA:BB:CC:DD:EE:FF"));
    fs::mock_set_fs_write_budget(20);
    TEST_ASSERT_EQUAL_INT(V1DeviceMutationStatus::DurablePending,
        settings.deleteV1DeviceTransactional("AA:BB:CC:DD:EE:FF", devices).status);
    fs::mock_reset_fs_write_budget();
    mock_nvs::set_read_result(kSettingsV1RuntimeNamespace, kNvsV1DeleteReady, ESP_FAIL,
                               mock_nvs::ReadStage::Presence);
    TEST_ASSERT_FALSE(settings.resolvePendingV1DeviceDelete(devices));
    TEST_ASSERT_TRUE(mock_preferences::namespaceHasKey(kSettingsV1RuntimeNamespace, kNvsV1DeleteReady));
    mock_nvs::reset();
    TEST_ASSERT_TRUE(settings.resolvePendingV1DeviceDelete(devices));
    TEST_ASSERT_TRUE(devices.listDevices().empty());
}

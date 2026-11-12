// Exercises the actual settings/credential and profile owners over separate
// host SD and LittleFS stores, including retries by fresh boot owners.

struct TransactionParticipantFixture {
    fs::FS sd{g_tempRoot / "sd"};
    fs::FS little{g_tempRoot / "little"};
    fs::FS otherCard{g_tempRoot / "other"};

    TransactionParticipantFixture() {
        for (const char* directory : {"sd", "little", "other"}) {
            std::filesystem::create_directories(g_tempRoot / directory);
        }
    }

    void mount(fs::FS& primary, bool withSd, bool withLittle = true) {
        storage.setFilesystem(&primary, withSd);
        storage.setLittleFS(withLittle ? &little : nullptr);
    }
};

JsonDocument credentialRestore(const char* ssid, const char* password = nullptr) {
    JsonDocument doc;
    doc["_type"] = "v1simple_backup";
    JsonObject slot = doc["wifiStaSlots"].to<JsonArray>().add<JsonObject>();
    slot["index"] = 0;
    slot["ssid"] = ssid;
    if (password) slot["passwordObf"] = encodeObfuscatedForStorage(password);
    return doc;
}

void exercise_missing_restore_card(bool mirrored) {
    TransactionParticipantFixture media;
    media.mount(media.sd, true, mirrored);
    TEST_ASSERT_TRUE(profiles.begin(storage));
    SettingsManager manager(storage, profiles);
    TEST_ASSERT_TRUE(manager.setWifiStaSlotCredentials(0, "Road Network", "original-password", "Saved", 0));
    const std::string originalSdSecret = readFileToString(media.sd, WIFI_CLIENT_SD_SECRET_PATH);
    manager.utInterruptRestoreAfterCredentials(true);
    TEST_ASSERT_FALSE(manager.applyBackupDocument(credentialRestore("Road Network", "candidate-password"), true).success);
    TEST_ASSERT_TRUE(media.sd.exists(RESTORE_TRANSACTION_PATH));
    TEST_ASSERT_EQUAL(mirrored, media.little.exists(RESTORE_TRANSACTION_PATH));
    const std::string pending = readFileToString(media.sd, RESTORE_TRANSACTION_PATH);
    const std::string stagedSdSecret = readFileToString(media.sd, WIFI_CLIENT_SD_SECRET_PATH);

    media.mount(media.little, false);
    V1ProfileManager offlineProfiles;
    TEST_ASSERT_TRUE(offlineProfiles.begin(storage));
    for (int attempt = 0; attempt < 2; ++attempt) {
        SettingsManager offline(storage, offlineProfiles);
        offline.load();
        TEST_ASSERT_FALSE(offline.checkAndRestoreFromSD());
        bool unavailable = false;
        TEST_ASSERT_EQUAL_STRING("", offline.getWifiStaSlotPassword(0, &unavailable).c_str());
        TEST_ASSERT_TRUE(unavailable);
        TEST_ASSERT_FALSE(offline.setWifiStaSlotCredentials(0, "Road Network", "would-be-newer", "Saved", 0));
        TEST_ASSERT_EQUAL(mirrored, media.little.exists(RESTORE_TRANSACTION_PATH));
        TEST_ASSERT_FALSE(media.little.exists(WIFI_CLIENT_SD_SECRET_PATH));
    }

    media.mount(media.otherCard, true);
    V1ProfileManager otherProfiles;
    TEST_ASSERT_TRUE(otherProfiles.begin(storage));
    SettingsManager other(storage, otherProfiles);
    other.load();
    TEST_ASSERT_FALSE(other.resolveStorageTransactionsForMutation());
    TEST_ASSERT_FALSE(media.otherCard.exists(WIFI_CLIENT_SD_SECRET_PATH));
    // Even a matching journal file on another volume cannot prove its card
    // participated in the original write. A different witness also fails.
    writeFileFromString(media.otherCard, RESTORE_TRANSACTION_PATH, pending.c_str());
    TEST_ASSERT_FALSE(other.resolveStorageTransactionsForMutation());
    JournalOwnerRecord reserved;
    TEST_ASSERT_TRUE(readJournalOwner(restoreJournalLocation.ownerKey, reserved));
    ++reserved.cardWitness;
    reserved.phase = JournalPhase::Preparing;
    TEST_ASSERT_TRUE(writeCardWitness(storage, restoreJournalLocation.cardPath, reserved));
    TEST_ASSERT_FALSE(other.resolveStorageTransactionsForMutation());
    TEST_ASSERT_TRUE(media.otherCard.exists(RESTORE_TRANSACTION_PATH));
    TEST_ASSERT_EQUAL_STRING(stagedSdSecret.c_str(), readFileToString(media.sd, WIFI_CLIENT_SD_SECRET_PATH).c_str());

    media.mount(media.sd, true);
    V1ProfileManager rejoinedProfiles;
    TEST_ASSERT_TRUE(rejoinedProfiles.begin(storage));
    SettingsManager rejoined(storage, rejoinedProfiles);
    rejoined.load();
    // Force a reset after rollback is durable but before any journal deletion.
    fs::mock_fail_next_remove(RESTORE_TRANSACTION_PATH);
    TEST_ASSERT_FALSE(rejoined.resolveStorageTransactionsForMutation());
    TEST_ASSERT_TRUE(media.sd.exists(RESTORE_TRANSACTION_PATH));
    TEST_ASSERT_EQUAL_STRING(originalSdSecret.c_str(), readFileToString(media.sd, WIFI_CLIENT_SD_SECRET_PATH).c_str());
    TEST_ASSERT_TRUE(readJournalOwner(restoreJournalLocation.ownerKey, reserved));
    TEST_ASSERT_EQUAL(JournalPhase::RolledBack, reserved.phase);

    media.mount(media.little, false);
    SettingsManager detachedAfterRollback(storage, offlineProfiles);
    detachedAfterRollback.load();
    TEST_ASSERT_TRUE(detachedAfterRollback.setWifiStaSlotCredentials(0, "Road Network", "acknowledged-newer", "Saved", 0));
    TEST_ASSERT_FALSE(media.little.exists(RESTORE_TRANSACTION_PATH));

    media.mount(media.sd, true);
    SettingsManager finalBoot(storage, rejoinedProfiles);
    finalBoot.load();
    TEST_ASSERT_TRUE(finalBoot.resolveStorageTransactionsForMutation());
    TEST_ASSERT_EQUAL_STRING("acknowledged-newer", finalBoot.getWifiStaSlotPassword(0).c_str());
    TEST_ASSERT_FALSE(media.sd.exists(RESTORE_TRANSACTION_PATH));
}

void test_mirrored_restore_waits_for_original_card_and_retires_before_cleanup() {
    exercise_missing_restore_card(true);
}

void test_sd_only_restore_keeps_recovery_authority_when_card_is_absent() {
    exercise_missing_restore_card(false);
}

void exercise_detached_profile_transaction(bool deleting, bool legacy) {
    TransactionParticipantFixture media;
    media.mount(media.sd, true);
    TEST_ASSERT_TRUE(profiles.begin(storage));
    V1Profile original("Road");
    original.description = "original";
    TEST_ASSERT_TRUE(profiles.saveProfile(original).success);
    SettingsManager manager(storage, profiles);
    TEST_ASSERT_TRUE(manager.saveDeferredBackup());

    const JournalLocation& journal = deleting ? deleteJournalLocation : restoreJournalLocation;
    if (deleting) {
        manager.utInterruptProfileDeleteAfterProfile(true);
        TEST_ASSERT_FALSE(manager.deleteProfileAndReferences("Road").success());
    } else {
        JsonDocument doc;
        doc["_type"] = "v1simple_backup";
        JsonObject profile = doc["profiles"].to<JsonArray>().add<JsonObject>();
        profile["name"] = "Road";
        profile["description"] = "interrupted replacement";
        JsonArray bytes = profile["bytes"].to<JsonArray>();
        for (int i = 0; i < 6; ++i) bytes.add(0);
        manager.utInterruptRestoreAfterProfiles(true);
        TEST_ASSERT_FALSE(manager.applyBackupDocument(doc, true).success);
    }
    if (legacy) {
        JsonDocument doc;
        TEST_ASSERT_TRUE(loadJsonFile(media.sd, journal.path, doc));
        doc["_version"] = 3;
        doc.remove("storage");
        doc.remove("cardWitness");
        doc.remove("device");
        stampJournalCrc(doc);
        writeJsonFile(media.sd, journal.path, doc);
        writeJsonFile(media.little, journal.path, doc);
        mock_preferences::ensureNamespace(SETTINGS_NS_META).erase(journal.ownerKey);
        TEST_ASSERT_TRUE(media.sd.remove(journal.cardPath));
    }
    const std::string pending = readFileToString(media.sd, journal.path);

    media.mount(media.little, false);
    V1ProfileManager offlineProfiles;
    TEST_ASSERT_TRUE(offlineProfiles.begin(storage));
    SettingsManager offline(storage, offlineProfiles);
    offline.load();
    TEST_ASSERT_FALSE(offline.resolveStorageTransactionsForMutation());
    TEST_ASSERT_TRUE(media.little.exists(journal.path));
    TEST_ASSERT_TRUE(media.sd.exists(journal.path));

    media.mount(media.sd, true, false);
    if (!legacy) {
        SettingsManager missingMirror(storage, profiles);
        missingMirror.load();
        TEST_ASSERT_FALSE(missingMirror.resolveStorageTransactionsForMutation());
    }
    media.mount(media.sd, true);
    if (legacy) {
        // Older detached rollback could remove the internal witness and then
        // acknowledge later edits. A sole SD journal has no replay authority.
        TEST_ASSERT_TRUE(media.little.remove(journal.path));
        SettingsManager ambiguous(storage, profiles);
        ambiguous.load();
        TEST_ASSERT_FALSE(ambiguous.resolveStorageTransactionsForMutation());
        TEST_ASSERT_TRUE(media.sd.exists(journal.path));
        writeFileFromString(media.little, journal.path, pending.c_str());
    }
    V1ProfileManager rejoinedProfiles;
    TEST_ASSERT_TRUE(rejoinedProfiles.begin(storage));
    SettingsManager rejoined(storage, rejoinedProfiles);
    rejoined.load();
    TEST_ASSERT_TRUE(rejoined.resolveStorageTransactionsForMutation());
    V1Profile saved;
    TEST_ASSERT_TRUE(rejoinedProfiles.loadProfile("Road", saved));
    TEST_ASSERT_EQUAL_STRING("original", saved.description.c_str());
    saved.description = "acknowledged later edit";
    TEST_ASSERT_TRUE(rejoinedProfiles.saveProfile(saved).success);
    writeFileFromString(media.sd, journal.path, pending.c_str());

    SettingsManager finalBoot(storage, rejoinedProfiles);
    finalBoot.load();
    TEST_ASSERT_TRUE(finalBoot.resolveStorageTransactionsForMutation());
    TEST_ASSERT_TRUE(rejoinedProfiles.loadProfile("Road", saved));
    TEST_ASSERT_EQUAL_STRING("acknowledged later edit", saved.description.c_str());
    TEST_ASSERT_FALSE(media.sd.exists(journal.path));
}

void test_pending_catalog_restore_requires_its_mirrors_before_later_profile_edits() {
    exercise_detached_profile_transaction(false, false);
}

void test_pending_profile_delete_requires_its_mirrors_before_later_profile_edits() {
    exercise_detached_profile_transaction(true, false);
}

void test_legacy_unidentified_journals_wait_for_original_primary() {
    exercise_detached_profile_transaction(true, true);
}

void test_restore_preparation_reset_retires_unapplied_journal_without_replay() {
    TransactionParticipantFixture media;
    media.mount(media.sd, true);
    TEST_ASSERT_TRUE(profiles.begin(storage));
    SettingsManager manager(storage, profiles);
    TEST_ASSERT_TRUE(manager.setWifiStaSlotCredentials(0, "Road Network", "original-password", "Saved", 0));

    mock_preferences::set_fail_write_for_key_on_call(restoreJournalLocation.ownerKey, 2);
    TEST_ASSERT_FALSE(manager.applyBackupDocument(credentialRestore("Road Network", "candidate-password"), true).success);
    TEST_ASSERT_TRUE(media.sd.exists(RESTORE_TRANSACTION_PATH));
    TEST_ASSERT_TRUE(media.little.exists(RESTORE_TRANSACTION_PATH));
    JournalOwnerRecord prepared;
    TEST_ASSERT_TRUE(readJournalOwner(restoreJournalLocation.ownerKey, prepared));
    TEST_ASSERT_EQUAL(JournalPhase::Preparing, prepared.phase);
    TEST_ASSERT_EQUAL_STRING("original-password", manager.getWifiStaSlotPassword(0).c_str());

    // Any replay would overwrite this newer authoritative NVS value. The
    // preparation receipt proves this rollback payload has no effects to undo.
    TEST_ASSERT_TRUE(storeWifiClientPasswordObfToNvs(encodeObfuscatedForStorage("untouched-by-preparation"), 0));
    SettingsManager rebooted(storage, profiles);
    rebooted.load();
    TEST_ASSERT_TRUE(rebooted.resolveStorageTransactionsForMutation());
    TEST_ASSERT_EQUAL_STRING("untouched-by-preparation", rebooted.getWifiStaSlotPassword(0).c_str());
    TEST_ASSERT_FALSE(media.sd.exists(RESTORE_TRANSACTION_PATH));
    TEST_ASSERT_FALSE(media.little.exists(RESTORE_TRANSACTION_PATH));
}

void test_modern_restore_without_local_authority_is_preserved_after_nvs_loss() {
    TransactionParticipantFixture media;
    media.mount(media.sd, true, false);
    TEST_ASSERT_TRUE(profiles.begin(storage));
    SettingsManager manager(storage, profiles);
    TEST_ASSERT_TRUE(manager.setWifiStaSlotCredentials(0, "Road Network", "original-password", "Saved", 0));
    manager.utInterruptRestoreAfterCredentials(true);
    TEST_ASSERT_FALSE(manager.applyBackupDocument(credentialRestore("Road Network", "candidate-password"), true).success);
    const std::string savedJournal = readFileToString(media.sd, RESTORE_TRANSACTION_PATH);
    const std::string savedSecret = readFileToString(media.sd, WIFI_CLIENT_SD_SECRET_PATH);
    mock_preferences::ensureNamespace(SETTINGS_NS_META).erase(restoreJournalLocation.ownerKey);

    SettingsManager rebooted(storage, profiles);
    rebooted.load();
    TEST_ASSERT_FALSE(rebooted.resolveStorageTransactionsForMutation());
    TEST_ASSERT_EQUAL_STRING(savedJournal.c_str(), readFileToString(media.sd, RESTORE_TRANSACTION_PATH).c_str());
    TEST_ASSERT_EQUAL_STRING(savedSecret.c_str(), readFileToString(media.sd, WIFI_CLIENT_SD_SECRET_PATH).c_str());
}

void test_foreign_restore_journal_is_preserved_even_when_its_token_is_obsolete_locally() {
    TransactionParticipantFixture media;
    media.mount(media.sd, true, false);
    TEST_ASSERT_TRUE(profiles.begin(storage));
    SettingsManager manager(storage, profiles);
    manager.utLeaveRestoreJournalAfterCommit(true);
    TEST_ASSERT_TRUE(manager.applyBackupDocument(credentialRestore("Road Network", "saved-password"), true).success);
    JsonDocument foreign;
    TEST_ASSERT_TRUE(loadJsonFile(media.sd, RESTORE_TRANSACTION_PATH, foreign));
    foreign["device"] = journalDevice() + 1;
    stampJournalCrc(foreign);
    writeJsonFile(media.sd, RESTORE_TRANSACTION_PATH, foreign);
    mock_preferences::ensureNamespace(SETTINGS_NS_META).erase(restoreJournalLocation.ownerKey);

    SettingsManager rebooted(storage, profiles);
    rebooted.load();
    TEST_ASSERT_FALSE(rebooted.resolveStorageTransactionsForMutation());
    TEST_ASSERT_TRUE(media.sd.exists(RESTORE_TRANSACTION_PATH));
}

void test_open_network_rollback_waits_for_proven_key_absence_and_retries() {
    TransactionParticipantFixture media;
    media.mount(media.little, false);
    TEST_ASSERT_TRUE(profiles.begin(storage));
    SettingsManager manager(storage, profiles);
    TEST_ASSERT_TRUE(manager.applyBackupDocument(credentialRestore("Open Network"), true).success);
    const char* key = kNvsWifiStaSlotPassword[0];
    TEST_ASSERT_NULL(mock_preferences::find(WIFI_CLIENT_NS, key));
    manager.utInterruptRestoreAfterCredentials(true);
    TEST_ASSERT_FALSE(manager.applyBackupDocument(credentialRestore("Secured Network", "candidate-password"), true).success);

    SettingsManager rebooted(storage, profiles);
    rebooted.load();
    mock_nvs::set_read_result(WIFI_CLIENT_NS, key, ESP_FAIL, mock_nvs::ReadStage::Presence);
    TEST_ASSERT_FALSE(rebooted.checkAndRestoreFromSD());
    TEST_ASSERT_TRUE(media.little.exists(RESTORE_TRANSACTION_PATH));
    bool unavailable = false;
    TEST_ASSERT_EQUAL_STRING("", rebooted.getWifiStaSlotPassword(0, &unavailable).c_str());
    TEST_ASSERT_TRUE(unavailable);
    TEST_ASSERT_NOT_NULL(mock_preferences::find(WIFI_CLIENT_NS, key));

    mock_nvs::set_read_result(WIFI_CLIENT_NS, key, ESP_OK, mock_nvs::ReadStage::Presence);
    mock_preferences::set_scalar_read_fault(WIFI_CLIENT_NS, key, PT_STR, ESP_FAIL, 2);
    TEST_ASSERT_TRUE(rebooted.resolveStorageTransactionsForMutation());
    TEST_ASSERT_FALSE(media.little.exists(RESTORE_TRANSACTION_PATH));
    TEST_ASSERT_NULL(mock_preferences::find(WIFI_CLIENT_NS, key));
    TEST_ASSERT_EQUAL_STRING("Open Network", rebooted.get().wifiStaSlots[0].ssid.c_str());
    TEST_ASSERT_EQUAL_STRING("", rebooted.getWifiStaSlotPassword(0, &unavailable).c_str());
    TEST_ASSERT_FALSE(unavailable);
}

void test_restore_snapshot_does_not_convert_disappearing_present_password_to_empty() {
    TransactionParticipantFixture media;
    media.mount(media.little, false);
    TEST_ASSERT_TRUE(profiles.begin(storage));
    SettingsManager manager(storage, profiles);
    TEST_ASSERT_TRUE(manager.setWifiStaSlotCredentials(0, "Road Network", "original-password", "Saved", 0));
    const char* key = kNvsWifiStaSlotPassword[0];
    mock_nvs::set_nth_read_result(WIFI_CLIENT_NS, key, ESP_ERR_NVS_NOT_FOUND, mock_nvs::ReadStage::StringSize, 1);
    TEST_ASSERT_FALSE(manager.applyBackupDocument(credentialRestore("New Network", "candidate-password"), true).success);
    TEST_ASSERT_FALSE(media.little.exists(RESTORE_TRANSACTION_PATH));
    TEST_ASSERT_EQUAL_STRING("Road Network", manager.get().wifiStaSlots[0].ssid.c_str());
    TEST_ASSERT_EQUAL_STRING("original-password", manager.getWifiStaSlotPassword(0).c_str());
}

void test_ordinary_wifi_rollback_keeps_journal_until_absence_is_proven() {
    TransactionParticipantFixture media;
    media.mount(media.little, false);
    TEST_ASSERT_TRUE(profiles.begin(storage));
    SettingsManager manager(storage, profiles);
    TEST_ASSERT_TRUE(manager.saveDeferredBackup());
    manager.utInterruptWifiCredentialBeforeSettingsCommit(true);
    TEST_ASSERT_FALSE(manager.setWifiStaSlotCredentials(0, "New Network", "candidate-password", "Saved", 0));
    const char* key = kNvsWifiStaSlotPassword[0];
    TEST_ASSERT_NOT_NULL(mock_preferences::find(WIFI_CLIENT_NS, key));

    SettingsManager rebooted(storage, profiles);
    rebooted.load();
    mock_nvs::set_read_result(WIFI_CLIENT_NS, key, ESP_FAIL, mock_nvs::ReadStage::Presence);
    TEST_ASSERT_FALSE(rebooted.resolveStorageTransactionsForMutation());
    TEST_ASSERT_TRUE(mock_preferences::namespaceHasKey(WIFI_CLIENT_NS, kNvsWifiTxnReady));
    TEST_ASSERT_NOT_NULL(mock_preferences::find(WIFI_CLIENT_NS, key));

    mock_nvs::set_read_result(WIFI_CLIENT_NS, key, ESP_OK, mock_nvs::ReadStage::Presence);
    TEST_ASSERT_TRUE(rebooted.resolveStorageTransactionsForMutation());
    TEST_ASSERT_FALSE(mock_preferences::namespaceHasKey(WIFI_CLIENT_NS, kNvsWifiTxnReady));
    TEST_ASSERT_NULL(mock_preferences::find(WIFI_CLIENT_NS, key));
    TEST_ASSERT_FALSE(rebooted.get().wifiStaSlots[0].isConfigured());
}

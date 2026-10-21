#pragma once

// Each pending journal is newer than an already committed mirror. A cached
// zero size must not discard the rollback that the interrupted operation needs.
void exercise_journal_checked_size(bool deletion) {
    for (bool inject : {false, true}) {
        for (bool pendingOnSecondary : {false, true}) {
            resetRuntimeState();
            fs::mock_reset_fs_open_state();
            const auto root = g_tempRoot / (std::to_string(inject) + std::to_string(pendingOnSecondary));
            fs::FS primary(root / "primary");
            fs::FS secondary(root / "secondary");
            storage.setFilesystem(&primary, true);
            storage.setLittleFS(&secondary);
            TEST_ASSERT_TRUE(profiles.begin(storage));
            SettingsManager manager(storage, profiles);
            makeCurrentV21Source(manager.mutableSettings());
            const char* path = deletion ? PROFILE_DELETE_TRANSACTION_PATH : RESTORE_TRANSACTION_PATH;
            std::string stale;
            if (deletion) {
                V1Profile original("Road");
                original.description = "original";
                TEST_ASSERT_TRUE(profiles.saveProfile(original).success);
                TEST_ASSERT_TRUE(manager.saveDeferredBackup());
                manager.utLeaveProfileDeleteJournalAfterCommit(true);
                TEST_ASSERT_TRUE(manager.deleteProfileAndReferences("Road").success());
                stale = readFileToString(secondary, path);
                V1Profile recreated("Road");
                recreated.description = "recreated-before-interruption";
                recreated.settings.bytes[0] = 111;
                TEST_ASSERT_TRUE(profiles.saveProfile(recreated).success);
                manager.utInterruptProfileDeleteAfterProfile(true);
                TEST_ASSERT_FALSE(manager.deleteProfileAndReferences("Road").success());
            } else {
                TEST_ASSERT_TRUE(manager.setWifiStaSlotCredentials(0, "OldNet", "old-password", "Old", 0));
                JsonDocument committed;
                JsonObject slot = committed["wifiStaSlots"].to<JsonArray>().add<JsonObject>();
                slot["index"] = 0;
                slot["ssid"] = "CommittedNet";
                slot["passwordObf"] = encodeObfuscatedForStorage("committed-password");
                manager.utLeaveRestoreJournalAfterCommit(true);
                TEST_ASSERT_TRUE(manager.applyBackupDocument(committed, true).success);
                stale = readFileToString(secondary, path);
                slot["ssid"] = "InterruptedNet";
                slot["passwordObf"] = encodeObfuscatedForStorage("interrupted-password");
                manager.utInterruptRestoreAfterCredentials(true);
                TEST_ASSERT_FALSE(manager.applyBackupDocument(committed, true).success);
            }
            TEST_ASSERT_FALSE(stale.empty());
            fs::FS& oldMirror = pendingOnSecondary ? primary : secondary;
            writeFileFromString(oldMirror, path, stale.c_str());
            if (inject) fs::mock_zero_file_size_for_full_path(
                (root / (pendingOnSecondary ? "secondary" : "primary") / (path + 1)).c_str());
            V1ProfileManager recoveredProfiles;
            TEST_ASSERT_TRUE(recoveredProfiles.begin(storage));
            SettingsManager recovered(storage, recoveredProfiles);
            recovered.load();
            recovered.checkAndRestoreFromSD();
            if (deletion) {
                V1Profile restored;
                TEST_ASSERT_TRUE(recoveredProfiles.loadProfile("Road", restored));
                TEST_ASSERT_EQUAL_STRING("recreated-before-interruption", restored.description.c_str());
                TEST_ASSERT_EQUAL_UINT8(111, restored.settings.bytes[0]);
            } else {
                TEST_ASSERT_EQUAL_STRING("CommittedNet", recovered.get().wifiStaSlots[0].ssid.c_str());
                TEST_ASSERT_EQUAL_STRING("committed-password", recovered.getWifiStaSlotPassword(0).c_str());
            }
            TEST_ASSERT_FALSE(primary.exists(path));
            TEST_ASSERT_FALSE(secondary.exists(path));
            fs::mock_reset_fs_open_state();
            recovered.mutableSettings().brightness = 77;
            TEST_ASSERT_TRUE(recovered.saveDeferredBackup());
            V1ProfileManager rebootProfiles;
            TEST_ASSERT_TRUE(rebootProfiles.begin(storage));
            SettingsManager rebooted(storage, rebootProfiles);
            rebooted.load();
            rebooted.checkAndRestoreFromSD();
            TEST_ASSERT_EQUAL_UINT8(77, rebooted.get().brightness);
            if (deletion) {
                V1Profile restored;
                TEST_ASSERT_TRUE(rebootProfiles.loadProfile("Road", restored));
                TEST_ASSERT_EQUAL_STRING("recreated-before-interruption", restored.description.c_str());
            } else {
                TEST_ASSERT_EQUAL_STRING("committed-password", rebooted.getWifiStaSlotPassword(0).c_str());
            }
        }
    }
}

void test_cached_restore_journal_size_preserves_pending_primary_and_secondary() {
    exercise_journal_checked_size(false);
}

void test_cached_delete_journal_size_preserves_pending_primary_and_secondary() {
    exercise_journal_checked_size(true);
}

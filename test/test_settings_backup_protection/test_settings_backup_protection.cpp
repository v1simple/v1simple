/**
 * test_settings_backup_protection — Exercises the atomic write, validation,
 * rollback, signature, scoring, and parse-rejection logic in
 * settings_backup.cpp.
 *
 * These are the "backup protection" guardrails that keep the SD backup file
 * intact even when writes fail partway through, renames fail, or the file
 * on disk turns out to be corrupt after promotion.
 */

#include <unity.h>

#include <filesystem>

#include <ArduinoJson.h>

#include "../mocks/Arduino.h"
#include "../mocks/Preferences.h"
#include "../mocks/nvs.h"
#include "../mocks/storage_manager.h"
#include "../../src/settings.h"
#include "../../src/v1_profiles.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

namespace ArduinoJson {

inline void convertFromJson(JsonVariantConst src, ::String& dst) {
    const char* raw = src.as<const char*>();
    dst = ::String(raw ? raw : "");
}

inline bool canConvertFromJson(JsonVariantConst src, const ::String&) {
    return src.is<const char*>();
}

}  // namespace ArduinoJson

#include "../../src/v1_profiles.cpp"
#include "../../src/backup_payload_builder.cpp"
#include "../../src/psram_freertos_alloc.cpp"
#include "../../src/settings.cpp"
#include "../../src/settings_nvs.cpp"
#include "../../src/settings_backup.cpp"
#include "../../src/settings_backup_doc.cpp"
#include "../../src/settings_restore.cpp"

namespace {

std::filesystem::path g_tempRoot;
int g_tempRootIndex = 0;

std::filesystem::path nextTempRoot() {
    return std::filesystem::temp_directory_path() /
           ("settings_backup_protection_" + std::to_string(++g_tempRootIndex));
}

/// Build a minimal valid backup document that passes signature + type checks.
void buildValidBackupDoc(JsonDocument& doc) {
    V1Settings settings;
    settings.apSSID = "V1-Test";
    settings.brightness = 128;
    V1ProfileManager profileManager;
    BackupPayloadBuilder::buildBackupDocument(
        doc, settings, profileManager,
        BackupPayloadBuilder::BackupTransport::SdBackup, 1000);
}

/// Serialize a JsonDocument to a std::string for writing to files.
std::string serializeDoc(const JsonDocument& doc) {
    std::string output;
    serializeJson(doc, output);
    return output;
}

/// Test helper: wrap a JsonDocument into a SerializedSettingsBackupPayload and
/// call writeBackupAtomically.  The old (fs, JsonDocument) overload was removed
/// in the backup perf cleanup; this preserves the same test coverage.
bool writeBackupAtomicallyFromDoc(fs::FS* fs, const JsonDocument& doc) {
    std::string json = serializeDoc(doc);
    SerializedSettingsBackupPayload payload;
    payload.data     = const_cast<char*>(json.c_str());
    payload.length   = json.size();
    payload.capacity = json.size();
    payload.inPsram  = false;
    bool result = writeBackupAtomically(fs, payload);
    // Prevent releaseSerializedSettingsBackupPayload from freeing stack memory
    payload.data = nullptr;
    payload.length = 0;
    payload.capacity = 0;
    return result;
}

/// Write a string directly to a file on the mock FS.
void writeFileContent(fs::FS& fs, const char* path, const std::string& content) {
    File file = fs.open(path, FILE_WRITE);
    TEST_ASSERT_TRUE(static_cast<bool>(file));
    file.write(reinterpret_cast<const uint8_t*>(content.data()), content.size());
    file.flush();
    file.close();
}

void resetRuntimeState() {
    mock_preferences::reset();
    mock_nvs::reset();
    mock_reset_heap_caps();
    fs::mock_reset_fs_rename_state();
    fs::mock_reset_fs_write_budget();
    v1ProfileManager = V1ProfileManager();
    settingsManager = SettingsManager();
    mockMillis = 1000;
    mockMicros = 1000000;
}

}  // namespace

void setUp() {
    g_tempRoot = nextTempRoot();
    std::filesystem::remove_all(g_tempRoot);
    std::filesystem::create_directories(g_tempRoot);
    resetRuntimeState();
}

void tearDown() {
    if (!g_tempRoot.empty()) {
        std::filesystem::remove_all(g_tempRoot);
    }
}

// ════════════════════════════════════════════════════════════════════════════
// hasBackupSignature
// ════════════════════════════════════════════════════════════════════════════

void test_signature_accepts_apSSID() {
    JsonDocument doc;
    doc["apSSID"] = "V1-Test";
    TEST_ASSERT_TRUE(hasBackupSignature(doc));
}

void test_signature_accepts_brightness() {
    JsonDocument doc;
    doc["brightness"] = 128;
    TEST_ASSERT_TRUE(hasBackupSignature(doc));
}

void test_signature_accepts_colorBogey() {
    JsonDocument doc;
    doc["colorBogey"] = 0xFF0000;
    TEST_ASSERT_TRUE(hasBackupSignature(doc));
}

void test_signature_accepts_slot0Name() {
    JsonDocument doc;
    doc["slot0Name"] = "Default";
    TEST_ASSERT_TRUE(hasBackupSignature(doc));
}

void test_signature_rejects_empty_doc() {
    JsonDocument doc;
    TEST_ASSERT_FALSE(hasBackupSignature(doc));
}

void test_signature_rejects_arbitrary_keys() {
    JsonDocument doc;
    doc["foo"] = "bar";
    doc["baz"] = 42;
    TEST_ASSERT_FALSE(hasBackupSignature(doc));
}

// ════════════════════════════════════════════════════════════════════════════
// isSupportedBackupType
// ════════════════════════════════════════════════════════════════════════════

void test_type_accepts_missing_type_field() {
    // Legacy backups have no _type — should be accepted.
    JsonDocument doc;
    doc["brightness"] = 128;
    TEST_ASSERT_TRUE(isSupportedBackupType(doc));
}

void test_type_accepts_sd_backup() {
    JsonDocument doc;
    doc["_type"] = "v1simple_sd_backup";
    TEST_ASSERT_TRUE(isSupportedBackupType(doc));
}

void test_type_accepts_http_backup() {
    JsonDocument doc;
    doc["_type"] = "v1simple_backup";
    TEST_ASSERT_TRUE(isSupportedBackupType(doc));
}

void test_type_rejects_unknown() {
    JsonDocument doc;
    doc["_type"] = "malicious_payload";
    TEST_ASSERT_FALSE(isSupportedBackupType(doc));
}

// ════════════════════════════════════════════════════════════════════════════
// backupCandidateScore / backupCriticalFieldScore / backupDocumentVersion
// ════════════════════════════════════════════════════════════════════════════

void test_score_empty_doc_is_minimal() {
    JsonDocument doc;
    TEST_ASSERT_EQUAL_INT(0, backupCriticalFieldScore(doc));
    // Version defaults to 1 when missing → score = 1*100 + 0 = 100
    TEST_ASSERT_EQUAL_INT(100, backupCandidateScore(doc));
}

void test_score_counts_critical_fields() {
    JsonDocument doc;
    doc["brightness"] = 128;
    doc["proxyBLE"] = true;
    TEST_ASSERT_EQUAL_INT(2, backupCriticalFieldScore(doc));
}

void test_score_prefers_newer_version() {
    JsonDocument older;
    older["_version"] = 5;
    older["brightness"] = 128;

    JsonDocument newer;
    newer["_version"] = 10;
    // newer has no critical fields beyond version

    // newer: 10*100 + 0 = 1000,  older: 5*100 + 1 = 501
    TEST_ASSERT_GREATER_THAN(backupCandidateScore(older),
                             backupCandidateScore(newer));
}

void test_version_falls_back_to_legacy_key() {
    JsonDocument doc;
    doc["version"] = 7;
    TEST_ASSERT_EQUAL_INT(7, backupDocumentVersion(doc));
}

void test_version_prefers_underscore_key() {
    JsonDocument doc;
    doc["_version"] = 9;
    doc["version"] = 3;
    TEST_ASSERT_EQUAL_INT(9, backupDocumentVersion(doc));
}

// ════════════════════════════════════════════════════════════════════════════
// parseBackupFile
// ════════════════════════════════════════════════════════════════════════════

void test_parse_valid_backup() {
    fs::FS fs(g_tempRoot);
    JsonDocument doc;
    buildValidBackupDoc(doc);
    writeFileContent(fs, SETTINGS_BACKUP_PATH, serializeDoc(doc));

    JsonDocument parsed;
    TEST_ASSERT_TRUE(parseBackupFile(&fs, SETTINGS_BACKUP_PATH, parsed, true));
}

void test_parse_rejects_null_fs() {
    JsonDocument parsed;
    TEST_ASSERT_FALSE(parseBackupFile(nullptr, SETTINGS_BACKUP_PATH, parsed, true));
}

void test_parse_rejects_null_path() {
    fs::FS fs(g_tempRoot);
    JsonDocument parsed;
    TEST_ASSERT_FALSE(parseBackupFile(&fs, nullptr, parsed, true));
}

void test_parse_rejects_empty_path() {
    fs::FS fs(g_tempRoot);
    JsonDocument parsed;
    TEST_ASSERT_FALSE(parseBackupFile(&fs, "", parsed, true));
}

void test_parse_rejects_nonexistent_file() {
    fs::FS fs(g_tempRoot);
    JsonDocument parsed;
    TEST_ASSERT_FALSE(parseBackupFile(&fs, "/does_not_exist.json", parsed, true));
}

void test_parse_rejects_empty_file() {
    fs::FS fs(g_tempRoot);
    writeFileContent(fs, SETTINGS_BACKUP_PATH, "");

    JsonDocument parsed;
    TEST_ASSERT_FALSE(parseBackupFile(&fs, SETTINGS_BACKUP_PATH, parsed, true));
}

void test_parse_rejects_invalid_json() {
    fs::FS fs(g_tempRoot);
    writeFileContent(fs, SETTINGS_BACKUP_PATH, "this is not json {{{");

    JsonDocument parsed;
    TEST_ASSERT_FALSE(parseBackupFile(&fs, SETTINGS_BACKUP_PATH, parsed, true));
}

void test_parse_rejects_unsupported_type() {
    fs::FS fs(g_tempRoot);
    JsonDocument doc;
    doc["_type"] = "unknown_payload";
    doc["brightness"] = 128;
    writeFileContent(fs, SETTINGS_BACKUP_PATH, serializeDoc(doc));

    JsonDocument parsed;
    TEST_ASSERT_FALSE(parseBackupFile(&fs, SETTINGS_BACKUP_PATH, parsed, true));
}

void test_parse_rejects_missing_signature() {
    fs::FS fs(g_tempRoot);
    JsonDocument doc;
    doc["_type"] = "v1simple_sd_backup";
    doc["_version"] = SD_BACKUP_VERSION;
    doc["unrelatedKey"] = "no signature fields present";
    writeFileContent(fs, SETTINGS_BACKUP_PATH, serializeDoc(doc));

    JsonDocument parsed;
    TEST_ASSERT_FALSE(parseBackupFile(&fs, SETTINGS_BACKUP_PATH, parsed, true));
}

// ════════════════════════════════════════════════════════════════════════════
// writeBackupAtomically — happy path
// ════════════════════════════════════════════════════════════════════════════

void test_atomic_write_creates_primary_file() {
    fs::FS fs(g_tempRoot);
    JsonDocument doc;
    buildValidBackupDoc(doc);

    TEST_ASSERT_TRUE(writeBackupAtomicallyFromDoc(&fs, doc));
    TEST_ASSERT_TRUE(fs.exists(SETTINGS_BACKUP_PATH));
    // Temp file must be cleaned up
    TEST_ASSERT_FALSE(fs.exists(SETTINGS_BACKUP_TMP_PATH));
}

void test_atomic_write_rotates_previous() {
    fs::FS fs(g_tempRoot);
    JsonDocument doc;
    buildValidBackupDoc(doc);

    // First write — no .prev yet
    TEST_ASSERT_TRUE(writeBackupAtomicallyFromDoc(&fs, doc));
    TEST_ASSERT_FALSE(fs.exists(SETTINGS_BACKUP_PREV_PATH));

    // Second write — first becomes .prev
    TEST_ASSERT_TRUE(writeBackupAtomicallyFromDoc(&fs, doc));
    TEST_ASSERT_TRUE(fs.exists(SETTINGS_BACKUP_PATH));
    TEST_ASSERT_TRUE(fs.exists(SETTINGS_BACKUP_PREV_PATH));
}

void test_atomic_write_rejects_null_fs() {
    JsonDocument doc;
    buildValidBackupDoc(doc);
    TEST_ASSERT_FALSE(writeBackupAtomicallyFromDoc(static_cast<fs::FS*>(nullptr), doc));
}

// ════════════════════════════════════════════════════════════════════════════
// writeBackupAtomically — failure & rollback paths
// ════════════════════════════════════════════════════════════════════════════

void test_atomic_write_rollback_on_rename_to_primary_failure() {
    fs::FS fs(g_tempRoot);
    JsonDocument doc;
    buildValidBackupDoc(doc);

    // First write succeeds — establishes primary
    TEST_ASSERT_TRUE(writeBackupAtomicallyFromDoc(&fs, doc));
    TEST_ASSERT_TRUE(fs.exists(SETTINGS_BACKUP_PATH));

    // Second write: first rename (primary → prev) succeeds,
    // second rename (tmp → primary) fails.
    fs::mock_fail_rename_on_call(fs::g_mock_fs_rename_state.renameCalls + 2);

    TEST_ASSERT_FALSE(writeBackupAtomicallyFromDoc(&fs, doc));

    // Rollback: previous backup should be restored to primary
    TEST_ASSERT_TRUE(fs.exists(SETTINGS_BACKUP_PATH));
    // Temp file must be cleaned up
    TEST_ASSERT_FALSE(fs.exists(SETTINGS_BACKUP_TMP_PATH));
}

void test_atomic_write_aborts_on_rotation_failure() {
    fs::FS fs(g_tempRoot);
    JsonDocument doc;
    buildValidBackupDoc(doc);

    // First write succeeds
    TEST_ASSERT_TRUE(writeBackupAtomicallyFromDoc(&fs, doc));

    // Second write: first rename (primary → prev) fails
    fs::mock_fail_next_rename();

    TEST_ASSERT_FALSE(writeBackupAtomicallyFromDoc(&fs, doc));

    // Original primary should still be intact
    TEST_ASSERT_TRUE(fs.exists(SETTINGS_BACKUP_PATH));
    TEST_ASSERT_FALSE(fs.exists(SETTINGS_BACKUP_TMP_PATH));
}

// ════════════════════════════════════════════════════════════════════════════
// writeBackupAtomically via SerializedSettingsBackupPayload
// ════════════════════════════════════════════════════════════════════════════

void test_atomic_write_serialized_payload_succeeds() {
    fs::FS fs(g_tempRoot);
    V1Settings settings;
    settings.apSSID = "V1-Test";
    settings.brightness = 128;
    V1ProfileManager profileManager;

    SerializedSettingsBackupPayload payload;
    TEST_ASSERT_TRUE(buildSerializedSdBackupPayload(payload, settings, profileManager, 1000));
    TEST_ASSERT_NOT_NULL(payload.data);
    TEST_ASSERT_GREATER_THAN(0u, payload.length);

    TEST_ASSERT_TRUE(writeBackupAtomically(&fs, payload));
    TEST_ASSERT_TRUE(fs.exists(SETTINGS_BACKUP_PATH));

    releaseSerializedSettingsBackupPayload(payload);
    TEST_ASSERT_NULL(payload.data);
}

// ════════════════════════════════════════════════════════════════════════════
// buildSerializedSdBackupPayload
// ════════════════════════════════════════════════════════════════════════════

void test_payload_release_is_idempotent() {
    SerializedSettingsBackupPayload payload;
    // Release on empty payload should be safe
    releaseSerializedSettingsBackupPayload(payload);
    TEST_ASSERT_NULL(payload.data);

    // Double release should be safe
    releaseSerializedSettingsBackupPayload(payload);
    TEST_ASSERT_NULL(payload.data);
}

// ════════════════════════════════════════════════════════════════════════════

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    UNITY_BEGIN();

    // Signature
    RUN_TEST(test_signature_accepts_apSSID);
    RUN_TEST(test_signature_accepts_brightness);
    RUN_TEST(test_signature_accepts_colorBogey);
    RUN_TEST(test_signature_accepts_slot0Name);
    RUN_TEST(test_signature_rejects_empty_doc);
    RUN_TEST(test_signature_rejects_arbitrary_keys);

    // Type support
    RUN_TEST(test_type_accepts_missing_type_field);
    RUN_TEST(test_type_accepts_sd_backup);
    RUN_TEST(test_type_accepts_http_backup);
    RUN_TEST(test_type_rejects_unknown);

    // Scoring
    RUN_TEST(test_score_empty_doc_is_minimal);
    RUN_TEST(test_score_counts_critical_fields);
    RUN_TEST(test_score_prefers_newer_version);
    RUN_TEST(test_version_falls_back_to_legacy_key);
    RUN_TEST(test_version_prefers_underscore_key);

    // parseBackupFile
    RUN_TEST(test_parse_valid_backup);
    RUN_TEST(test_parse_rejects_null_fs);
    RUN_TEST(test_parse_rejects_null_path);
    RUN_TEST(test_parse_rejects_empty_path);
    RUN_TEST(test_parse_rejects_nonexistent_file);
    RUN_TEST(test_parse_rejects_empty_file);
    RUN_TEST(test_parse_rejects_invalid_json);
    RUN_TEST(test_parse_rejects_unsupported_type);
    RUN_TEST(test_parse_rejects_missing_signature);

    // Atomic write — happy path
    RUN_TEST(test_atomic_write_creates_primary_file);
    RUN_TEST(test_atomic_write_rotates_previous);
    RUN_TEST(test_atomic_write_rejects_null_fs);

    // Atomic write — failure & rollback
    RUN_TEST(test_atomic_write_rollback_on_rename_to_primary_failure);
    RUN_TEST(test_atomic_write_aborts_on_rotation_failure);

    // Serialized payload path
    RUN_TEST(test_atomic_write_serialized_payload_succeeds);
    RUN_TEST(test_payload_release_is_idempotent);

    return UNITY_END();
}

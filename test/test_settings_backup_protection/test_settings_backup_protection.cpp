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

V1ProfileManager profiles;
SettingsManager settings(storage, profiles);

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
    settings.autoPushProfileSchemaVersion = V1_PROFILE_SCHEMA_VERSION;
    V1ProfileManager profileManager;
    BackupPayloadBuilder::buildBackupDocument(
        doc, settings, profileManager,
        BackupPayloadBuilder::BackupTransport::SdBackup, 1000);
}

JsonObject appendCurrentProfile(JsonDocument& doc, const char* name) {
    doc["autoPushProfileSchemaVersion"] = V1_PROFILE_SCHEMA_VERSION;
    JsonArray profiles = doc["profiles"].as<JsonArray>();
    if (profiles.isNull()) profiles = doc["profiles"].to<JsonArray>();
    JsonObject profile = profiles.add<JsonObject>();
    profile["name"] = name;
    profile["description"] = "";
    profile["schemaVersion"] = V1_PROFILE_SCHEMA_VERSION;
    appendV1DetectorConfiguration(profile["detector"].to<JsonObject>(),
                                  V1DetectorConfiguration{});
    JsonArray bytes = profile["bytes"].to<JsonArray>();
    for (size_t index = 0; index < V1SettingsJson::kSettingsByteCount; ++index) {
        bytes.add(0xFF);
    }
    return profile;
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

std::string readFileContent(fs::FS& fs, const char* path) {
    File file = fs.open(path, FILE_READ);
    if (!file) return {};
    std::string content;
    while (file.available()) content.push_back(static_cast<char>(file.read()));
    file.close();
    return content;
}

void resetRuntimeState() {
    mock_preferences::reset();
    mock_nvs::reset();
    mock_reset_heap_caps();
    fs::mock_reset_fs_rename_state();
    fs::mock_reset_fs_write_budget();
    profiles = V1ProfileManager();
    settings = SettingsManager(storage, profiles);
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

void test_future_or_unrepresentable_version_cannot_overflow_candidate_score() {
    JsonDocument future;
    future["_version"] = SD_BACKUP_VERSION + 1;
    future["brightness"] = 1;
    TEST_ASSERT_EQUAL_INT(1, backupDocumentVersion(future));
    TEST_ASSERT_EQUAL_INT(101, backupCandidateScore(future));

    JsonDocument maximum;
    maximum["_version"] = 2147483647;
    maximum["brightness"] = 1;
    TEST_ASSERT_EQUAL_INT(1, backupDocumentVersion(maximum));
    TEST_ASSERT_EQUAL_INT(101, backupCandidateScore(maximum));
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

void test_parse_rejects_present_wrong_type_integrity_markers() {
    fs::FS fs(g_tempRoot);
    const char* bodies[] = {
        "{\"_type\":null,\"brightness\":1}",
        "{\"_type\":7,\"brightness\":1}",
        "{\"_type\":\"v1simple_sd_backup\",\"_crc32\":null,\"brightness\":1}",
        "{\"_type\":\"v1simple_sd_backup\",\"_crc32\":\"0\",\"brightness\":1}",
        "{\"_type\":\"v1simple_sd_backup\",\"_crc32\":-1,\"brightness\":1}",
        "{\"_type\":\"v1simple_sd_backup\",\"_crc32\":1.5,\"brightness\":1}",
        "{\"_type\":\"v1simple_sd_backup\",\"_version\":null,\"brightness\":1}",
        "{\"_type\":\"v1simple_sd_backup\",\"_version\":\"20\",\"brightness\":1}",
        "{\"_type\":\"v1simple_sd_backup\",\"_version\":22,\"brightness\":1}",
        "{\"_type\":\"v1simple_sd_backup\",\"_version\":2147483647,\"brightness\":1}",
        "{\"_type\":\"v1simple_sd_backup\",\"version\":21,\"brightness\":1}",
        "{\"_type\":\"v1simple_sd_backup\",\"_version\":21,\"brightness\":1,\"brightnes\":2}",
    };
    for (const char* body : bodies) {
        writeFileContent(fs, SETTINGS_BACKUP_PATH, body);
        PsramJson::Document parsed;
        TEST_ASSERT_FALSE_MESSAGE(parseBackupFile(&fs, SETTINGS_BACKUP_PATH, parsed, false), body);
    }
}

void test_invalid_newer_marker_candidate_cannot_suppress_valid_previous_backup() {
    fs::FS fs(g_tempRoot);
    JsonDocument future;
    future["_type"] = "v1simple_sd_backup";
    future["_version"] = SD_BACKUP_VERSION + 1;
    future["brightness"] = 211;
    future["_crc32"] = BackupPayloadBuilder::computeBackupCrc32(future);
    writeFileContent(fs, SETTINGS_BACKUP_PATH, serializeDoc(future));
    JsonDocument previous;
    buildValidBackupDoc(previous);
    previous["brightness"] = 42;
    previous.remove("_crc32");
    previous["_crc32"] = BackupPayloadBuilder::computeBackupCrc32(previous);
    writeFileContent(fs, SETTINGS_BACKUP_PREV_PATH, serializeDoc(previous));

    PsramJson::Document selected;
    const char* selectedPath = nullptr;
    TEST_ASSERT_TRUE(loadBestBackupDocument(&fs, selected, &selectedPath, false));
    TEST_ASSERT_EQUAL_STRING(SETTINGS_BACKUP_PREV_PATH, selectedPath);
    TEST_ASSERT_EQUAL_INT(42, selected["brightness"].as<int>());
}

void test_invalid_current_schema_candidate_cannot_suppress_valid_previous_backup() {
    fs::FS fs(g_tempRoot);
    JsonDocument invalidCurrent;
    buildValidBackupDoc(invalidCurrent);
    invalidCurrent["slot0ProfielName"] = "Road";
    invalidCurrent.remove("_crc32");
    invalidCurrent["_crc32"] = BackupPayloadBuilder::computeBackupCrc32(invalidCurrent);
    writeFileContent(fs, SETTINGS_BACKUP_PATH, serializeDoc(invalidCurrent));

    JsonDocument previous;
    buildValidBackupDoc(previous);
    previous["brightness"] = 42;
    previous.remove("_crc32");
    previous["_crc32"] = BackupPayloadBuilder::computeBackupCrc32(previous);
    writeFileContent(fs, SETTINGS_BACKUP_PREV_PATH, serializeDoc(previous));

    PsramJson::Document selected;
    const char* selectedPath = nullptr;
    TEST_ASSERT_TRUE(loadBestBackupDocument(&fs, selected, &selectedPath, false));
    TEST_ASSERT_EQUAL_STRING(SETTINGS_BACKUP_PREV_PATH, selectedPath);
    TEST_ASSERT_EQUAL_INT(42, selected["brightness"].as<int>());
}

void test_intrinsically_invalid_current_candidate_selects_valid_previous_backup() {
    fs::FS fs(g_tempRoot);

    JsonDocument previous;
    buildValidBackupDoc(previous);
    previous["brightness"] = 42;
    previous.remove("_crc32");
    previous["_crc32"] = BackupPayloadBuilder::computeBackupCrc32(previous);
    writeFileContent(fs, SETTINGS_BACKUP_PREV_PATH, serializeDoc(previous));

    const auto assertPreviousSelected = [&](JsonDocument& invalidCurrent, const char* label) {
        TEST_ASSERT_FALSE_MESSAGE(validateCurrentBackupDocumentShape(invalidCurrent), label);
        invalidCurrent.remove("_crc32");
        invalidCurrent["_crc32"] = BackupPayloadBuilder::computeBackupCrc32(invalidCurrent);
        writeFileContent(fs, SETTINGS_BACKUP_PATH, serializeDoc(invalidCurrent));

        PsramJson::Document selected;
        const char* selectedPath = nullptr;
        TEST_ASSERT_TRUE(loadBestBackupDocument(&fs, selected, &selectedPath, false));
        TEST_ASSERT_EQUAL_STRING_MESSAGE(SETTINGS_BACKUP_PREV_PATH, selectedPath, label);
        TEST_ASSERT_EQUAL_INT(42, selected["brightness"].as<int>());
    };

    JsonDocument duplicateWifiIndex;
    buildValidBackupDoc(duplicateWifiIndex);
    JsonArray wifiSlots = duplicateWifiIndex["wifiStaSlots"].to<JsonArray>();
    for (int copy = 0; copy < 2; ++copy) {
        JsonObject slot = wifiSlots.add<JsonObject>();
        slot["index"] = 0;
        slot["ssid"] = "RoadNet";
        slot["label"] = "Road";
        slot["priority"] = copy;
        slot["lastConnectedAtSec"] = static_cast<uint32_t>(copy);
    }
    duplicateWifiIndex["wifiClientSSID"] = "RoadNet";
    assertPreviousSelected(duplicateWifiIndex, "duplicate WiFi slot index");

    JsonDocument malformedDetector;
    buildValidBackupDoc(malformedDetector);
    JsonObject malformedProfile = appendCurrentProfile(malformedDetector, "Road");
    malformedProfile["detector"]["volume"]["policy"] = "temporary";
    assertPreviousSelected(malformedDetector, "malformed current detector");

    JsonDocument duplicateProfileName;
    buildValidBackupDoc(duplicateProfileName);
    appendCurrentProfile(duplicateProfileName, "Road");
    appendCurrentProfile(duplicateProfileName, "ROAD");
    assertPreviousSelected(duplicateProfileName, "duplicate canonical profile name");

    JsonDocument danglingAssignment;
    buildValidBackupDoc(danglingAssignment);
    danglingAssignment["autoPushProfileSchemaVersion"] = V1_PROFILE_SCHEMA_VERSION;
    danglingAssignment["slot0ProfileName"] = "Absent";
    assertPreviousSelected(danglingAssignment, "dangling profile assignment");

    JsonDocument decodedNulPassword;
    buildValidBackupDoc(decodedNulPassword);
    // Under the canonical "V1..." XOR key, the first two encoded bytes decode
    // to NUL. Shape-only validation used to let this primary suppress .prev.
    decodedNulPassword["apPassword"] = "hex:5631000000000000";
    assertPreviousSelected(decodedNulPassword, "AP password decodes to embedded NUL");

    JsonDocument decodedControlPassword;
    buildValidBackupDoc(decodedControlPassword);
    // Decodes to 0x01 followed by seven ASCII 'A' bytes. The exact current
    // writer cannot serialize that C0 byte, so it may not qualify a primary.
    decodedControlPassword["apPassword"] = "hex:577006736C127222";
    assertPreviousSelected(decodedControlPassword, "AP password decodes to unsafe control byte");

    JsonDocument decodedInvalidUtf8Password;
    buildValidBackupDoc(decodedInvalidUtf8Password);
    // Decodes to the invalid UTF-8 prefix C3 28 followed by six ASCII bytes.
    decodedInvalidUtf8Password["apPassword"] = "hex:951906736C127222";
    assertPreviousSelected(decodedInvalidUtf8Password, "AP password decodes to invalid UTF-8");
}

void test_current_backup_requires_every_writer_field_with_only_transport_exceptions() {
    V1Settings settings;
    settings.apSSID = "V1-Test";
    settings.apPassword = "recovery-secret";
    settings.autoPushProfileSchemaVersion = V1_PROFILE_SCHEMA_VERSION;
    V1ProfileManager profileManager;

    JsonDocument sd;
    BackupPayloadBuilder::buildBackupDocument(
        sd, settings, profileManager, BackupPayloadBuilder::BackupTransport::SdBackup, 1000);
    TEST_ASSERT_TRUE(validateCurrentBackupDocumentShape(sd));

    std::vector<std::string> requiredKeys;
    for (JsonPairConst pair : sd.as<JsonObjectConst>()) {
        requiredKeys.emplace_back(pair.key().c_str(), pair.key().size());
    }
    for (const std::string& key : requiredKeys) {
        JsonDocument missing;
        missing.set(sd);
        missing.remove(key.c_str());
        TEST_ASSERT_FALSE_MESSAGE(validateCurrentBackupDocumentShape(missing), key.c_str());
    }

    JsonDocument http;
    BackupPayloadBuilder::buildBackupDocument(
        http, settings, profileManager, BackupPayloadBuilder::BackupTransport::HttpDownload, 1000);
    TEST_ASSERT_TRUE(validateCurrentBackupDocumentShape(http));
    TEST_ASSERT_TRUE(http["apPassword"].isUnbound());
    TEST_ASSERT_TRUE(http["_crc32"].isUnbound());

    http["apPassword"] = "hex:00";
    TEST_ASSERT_FALSE(validateCurrentBackupDocumentShape(http));
    http.remove("apPassword");
    http["_crc32"] = 1u;
    TEST_ASSERT_FALSE(validateCurrentBackupDocumentShape(http));
}

void test_current_backup_marker_only_or_partial_document_is_not_applicable() {
    JsonDocument markerOnly;
    markerOnly["_type"] = "v1simple_backup";
    markerOnly["_version"] = SD_BACKUP_VERSION;
    TEST_ASSERT_FALSE(validateCurrentBackupDocumentShape(markerOnly));

    JsonDocument partial;
    partial["_type"] = "v1simple_sd_backup";
    partial["_version"] = SD_BACKUP_VERSION;
    partial["brightness"] = 1;
    TEST_ASSERT_FALSE(validateCurrentBackupDocumentShape(partial));
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
    settings.autoPushProfileSchemaVersion = V1_PROFILE_SCHEMA_VERSION;
    V1ProfileManager profileManager;
    TEST_ASSERT_TRUE(profileManager.begin(&fs));

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
    payload.requestToken = 123;
    // Release on empty payload should be safe
    releaseSerializedSettingsBackupPayload(payload);
    TEST_ASSERT_NULL(payload.data);
    TEST_ASSERT_EQUAL_UINT32(0, payload.requestToken);

    // Double release should be safe
    releaseSerializedSettingsBackupPayload(payload);
    TEST_ASSERT_NULL(payload.data);
}

void test_backup_crc_streams_without_document_or_string_allocation() {
    JsonDocument doc;
    doc["before"] = "value";
    doc["_crc32"] = 123u; // Deliberately not last.
    doc["escaped\"\\\nkey"] = "payload";
    JsonArray nested = doc["nested"].to<JsonArray>();
    nested.add(1);
    nested.add("two");

    JsonDocument legacyCopy;
    legacyCopy.set(doc);
    legacyCopy.remove("_crc32");
    String serialized;
    serializeJson(legacyCopy, serialized);
    const uint32_t expected = computeCrc32(
        reinterpret_cast<const uint8_t*>(serialized.c_str()), serialized.length());

    mock_reset_heap_caps_tracking();
    g_mock_heap_caps_fail_all_allocations = true;
    TEST_ASSERT_EQUAL_HEX32(expected, BackupPayloadBuilder::computeBackupCrc32(doc));
    g_mock_heap_caps_fail_all_allocations = false;
    TEST_ASSERT_EQUAL_UINT32(0u, g_mock_heap_caps_malloc_calls);
    TEST_ASSERT_EQUAL_UINT32(0u, g_mock_heap_caps_realloc_calls);
    TEST_ASSERT_EQUAL_UINT32(123u, doc["_crc32"].as<uint32_t>());
}

void test_best_backup_psram_exhaustion_fails_closed_without_copy_or_file_mutation() {
    fs::FS fs(g_tempRoot);
    JsonDocument source;
    buildValidBackupDoc(source);
    const std::string committed = serializeDoc(source);
    writeFileContent(fs, SETTINGS_BACKUP_PATH, committed);

    PsramJson::Document selected;
    const char* selectedPath = "stale";
    mock_reset_heap_caps_tracking();
    g_mock_heap_caps_fail_all_allocations = true;
    TEST_ASSERT_FALSE(loadBestBackupDocument(&fs, selected, &selectedPath, false));
    g_mock_heap_caps_fail_all_allocations = false;

    TEST_ASSERT_NULL(selectedPath);
    TEST_ASSERT_EQUAL_UINT(0u, selected.size());
    TEST_ASSERT_GREATER_THAN(0u, g_mock_heap_caps_malloc_calls + g_mock_heap_caps_realloc_calls);
    for (uint32_t index = 0; index < g_mock_heap_caps_malloc_calls && index < MOCK_HEAP_CAPS_TRACKED_CALLS; ++index) {
        TEST_ASSERT_EQUAL_UINT32(PsramJson::kCaps, g_mock_heap_caps_malloc_caps_history[index]);
    }
    TEST_ASSERT_EQUAL_STRING(committed.c_str(), readFileContent(fs, SETTINGS_BACKUP_PATH).c_str());
}

void test_fail_once_while_scanning_any_backup_candidate_never_selects_an_older_copy() {
    fs::FS fs(g_tempRoot);
    JsonDocument newest;
    buildValidBackupDoc(newest);
    newest["_version"] = SD_BACKUP_VERSION;
    newest["brightness"] = 211;
    newest.remove("_crc32");
    newest["_crc32"] = BackupPayloadBuilder::computeBackupCrc32(newest);
    JsonDocument older;
    buildValidBackupDoc(older);
    older["_version"] = SD_BACKUP_VERSION - 1;
    older["brightness"] = 42;
    older.remove("_crc32");
    older["_crc32"] = BackupPayloadBuilder::computeBackupCrc32(older);
    const std::string newestBytes = serializeDoc(newest);
    const std::string olderBytes = serializeDoc(older);
    writeFileContent(fs, SETTINGS_BACKUP_PATH, newestBytes);
    writeFileContent(fs, SETTINGS_BACKUP_PREV_PATH, olderBytes);

    bool exercisedFailure = false;
    for (uint32_t failCall = 1; failCall <= 20; ++failCall) {
        mock_reset_heap_caps_tracking();
        g_mock_heap_caps_fail_malloc_on_call = failCall;
        PsramJson::Document selected;
        const char* selectedPath = "stale";
        BackupDocumentLoadStatus status = BackupDocumentLoadStatus::Success;
        const bool loaded = loadBestBackupDocument(&fs, selected, &selectedPath, false, &status);
        const bool failedAllocation = g_mock_heap_caps_fail_malloc_on_call == 0u;
        if (failedAllocation) {
            exercisedFailure = true;
            TEST_ASSERT_FALSE(loaded);
            TEST_ASSERT_NULL(selectedPath);
            TEST_ASSERT_EQUAL_INT(static_cast<int>(BackupDocumentLoadStatus::MemoryUnavailable),
                                  static_cast<int>(status));
        }
        g_mock_heap_caps_fail_malloc_on_call = 0u;
    }
    TEST_ASSERT_TRUE(exercisedFailure);
    TEST_ASSERT_EQUAL_STRING(newestBytes.c_str(), readFileContent(fs, SETTINGS_BACKUP_PATH).c_str());
    TEST_ASSERT_EQUAL_STRING(olderBytes.c_str(), readFileContent(fs, SETTINGS_BACKUP_PREV_PATH).c_str());
}

void test_atomic_backup_verification_oom_preserves_committed_primary() {
    fs::FS fs(g_tempRoot);
    JsonDocument original;
    buildValidBackupDoc(original);
    TEST_ASSERT_TRUE(writeBackupAtomicallyFromDoc(&fs, original));
    const std::string committed = readFileContent(fs, SETTINGS_BACKUP_PATH);

    JsonDocument replacement;
    buildValidBackupDoc(replacement);
    replacement["brightness"] = 17;
    std::string replacementJson = serializeDoc(replacement);
    SerializedSettingsBackupPayload payload;
    payload.data = replacementJson.data();
    payload.length = replacementJson.size();

    g_mock_heap_caps_fail_all_allocations = true;
    TEST_ASSERT_FALSE(writeBackupAtomically(&fs, payload));
    g_mock_heap_caps_fail_all_allocations = false;
    payload.data = nullptr;

    TEST_ASSERT_EQUAL_STRING(committed.c_str(), readFileContent(fs, SETTINGS_BACKUP_PATH).c_str());
    TEST_ASSERT_FALSE(fs.exists(SETTINGS_BACKUP_TMP_PATH));
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
    RUN_TEST(test_future_or_unrepresentable_version_cannot_overflow_candidate_score);

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
    RUN_TEST(test_parse_rejects_present_wrong_type_integrity_markers);
    RUN_TEST(test_invalid_newer_marker_candidate_cannot_suppress_valid_previous_backup);
    RUN_TEST(test_invalid_current_schema_candidate_cannot_suppress_valid_previous_backup);
    RUN_TEST(test_intrinsically_invalid_current_candidate_selects_valid_previous_backup);
    RUN_TEST(test_current_backup_requires_every_writer_field_with_only_transport_exceptions);
    RUN_TEST(test_current_backup_marker_only_or_partial_document_is_not_applicable);

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
    RUN_TEST(test_backup_crc_streams_without_document_or_string_allocation);
    RUN_TEST(test_fail_once_while_scanning_any_backup_candidate_never_selects_an_older_copy);
    RUN_TEST(test_best_backup_psram_exhaustion_fails_closed_without_copy_or_file_mutation);
    RUN_TEST(test_atomic_backup_verification_oom_preserves_committed_primary);

    return UNITY_END();
}

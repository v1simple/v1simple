#include <unity.h>

#include "../../src/modules/encounter/v1_encounter_logger.h"
#include "../../src/modules/encounter/v1_encounter_logger.cpp"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {
AlertData makeAlert(uint8_t v1Index, Direction direction, uint8_t frontRaw, uint8_t rearRaw) {
    AlertData alert;
    alert.band = BAND_KA;
    alert.direction = direction;
    alert.v1Index = v1Index;
    alert.frontRawStrength = frontRaw;
    alert.rearRawStrength = rearRaw;
    alert.frontStrength = 2;
    alert.rearStrength = 1;
    alert.frequency = 34700;
    alert.isValid = true;
    alert.isPriority = true;
    return alert;
}

std::string readProjectFile(const char* relativePath) {
    const std::filesystem::path path = std::filesystem::path(PROJECT_DIR) / relativePath;
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        return {};
    }
    return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
}
} // namespace

void setUp() {}
void tearDown() {}

void test_encounter_logger_uses_compact_location_free_schema() {
    V1EncounterLogger logger;
    logger.setBootId(12, 0x34);
    logger.begin(true);

    AlertData alert = makeAlert(3, DIR_SIDE, 0x91, 0x72);
    logger.onAlertTable(&alert, 1, 100);

    TEST_ASSERT_EQUAL_STRING("/encounters/encounters_12-00000034.csv", logger.csvPath());
    TEST_ASSERT_EQUAL_UINT32(1, logger.testSnapshotCount());
    const char* legacyPrefix = "100,1,1,START,3,1,Ka,34700,SIDE,145,114,2,1,1,0,0,0,";
    TEST_ASSERT_EQUAL_STRING_LEN(legacyPrefix, logger.testGetLastLine(), std::strlen(legacyPrefix));
    TEST_ASSERT_NULL(std::strstr(logger.testGetLastLine(), "gps"));
    TEST_ASSERT_NULL(std::strstr(logger.testGetLastLine(), "speed"));
}

void test_qualification_trace_stamps_session_identity_digest_and_loss_fields() {
    V1EncounterLogger logger;
    logger.setBootId(44, 0x11223344);
    logger.begin(true);
    logger.beginQualificationSession(0xA1B2C3D4, 900, 1);

    const uint8_t payload[] = {0xAA, 0xD0, 0xE0, 0x31, 0x02, 0xAB};
    V1CausalTraceRecord record;
    record.stage = V1CausalStage::Rx;
    record.outcome = V1CausalOutcome::Accepted;
    record.payloadUnit = V1CausalPayloadUnit::Notification;
    record.stageDutMillis = 902;
    record.identity.dutMillis = 901;
    record.identity.bleSessionGeneration = 7;
    record.identity.rxFirstSeq = 12;
    record.identity.rxLastSeq = 12;
    record.identity.characteristic = 0xFFF4;
    record.identity.payloadLength = sizeof(payload);
    record.identity.payloadDigest = v1Fnv1a32(payload, sizeof(payload));
    record.sourceLossCount = 2;
    logger.recordCausalTrace(record, payload, sizeof(payload));

    TEST_ASSERT_EQUAL_STRING("/encounters/causal_trace_44-11223344.csv", logger.causalTraceCsvPath());
    TEST_ASSERT_EQUAL_UINT32(2, logger.testTraceCount()); // SESSION_START + BLE_RX
    const char* line = logger.testGetLastTraceLine();
    TEST_ASSERT_NOT_NULL(std::strstr(line, ",A1B2C3D4,902,901,BLE_RX,ACCEPTED,7,12,12,0,FFF4,NOTIFICATION,6,"));
    TEST_ASSERT_NOT_NULL(std::strstr(line, ",2,0,1,1,1,AAD0E03102AB\n")); // losses, clocks, exact payload

    AlertData alert = makeAlert(1, DIR_FRONT, 0x90, 0x70);
    logger.onAlertTable(&alert, 1, 902);
    TEST_ASSERT_NOT_NULL(std::strstr(logger.testGetLastLine(), ",A1B2C3D4,"));

    logger.endQualificationSession(0xA1B2C3D4, 903, 3);
    TEST_ASSERT_NOT_NULL(std::strstr(logger.testGetLastTraceLine(), ",903,903,SESSION_END,ENDED,"));
    TEST_ASSERT_NOT_NULL(std::strstr(logger.testGetLastTraceLine(), ",3,0,1,1,1,\n"));
}

void test_qualification_trace_retains_distinct_prestart_state_and_alert_sources() {
    V1EncounterLogger logger;
    logger.begin(true);

    V1SemanticRevisionEvidence evidence;
    evidence.stateRevision = 9;
    evidence.alertRevision = 5;
    evidence.alertTableDigest = 0xAABBCCDD;
    evidence.stateSource.dutMillis = 710;
    evidence.stateSource.bleSessionGeneration = 2;
    evidence.stateSource.rxFirstSeq = 11;
    evidence.stateSource.rxLastSeq = 12;
    evidence.stateSource.eventSeq = 40;
    evidence.stateSource.characteristic = 0xFFF4;
    evidence.stateSource.payloadLength = 23;
    evidence.stateSource.payloadDigest = 0x11112222;
    evidence.alertSource.dutMillis = 720;
    evidence.alertSource.bleSessionGeneration = 2;
    evidence.alertSource.rxFirstSeq = 13;
    evidence.alertSource.rxLastSeq = 14;
    evidence.alertSource.eventSeq = 41;
    evidence.alertSource.characteristic = 0xFFF4;
    evidence.alertSource.payloadLength = 24;
    evidence.alertSource.payloadDigest = 0x33334444;

    logger.testBeginQualificationSession(0xBEE69052, 900, evidence, 3);

    TEST_ASSERT_EQUAL_UINT32(3, logger.testTraceCount());
    const V1PersistedCausalTraceRecord* start = logger.testGetTraceRecord(0);
    const V1PersistedCausalTraceRecord* state = logger.testGetTraceRecord(1);
    const V1PersistedCausalTraceRecord* alerts = logger.testGetTraceRecord(2);
    TEST_ASSERT_NOT_NULL(start);
    TEST_ASSERT_NOT_NULL(state);
    TEST_ASSERT_NOT_NULL(alerts);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(V1CausalStage::SessionStart),
                            static_cast<uint8_t>(start->record.stage));

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(V1CausalStage::StateBaseline),
                            static_cast<uint8_t>(state->record.stage));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(V1CausalOutcome::Retained),
                            static_cast<uint8_t>(state->record.outcome));
    TEST_ASSERT_EQUAL_UINT32(900, state->record.stageDutMillis);
    TEST_ASSERT_EQUAL_UINT32(710, state->record.identity.dutMillis);
    TEST_ASSERT_EQUAL_UINT32(40, state->record.identity.eventSeq);
    TEST_ASSERT_EQUAL_UINT32(11, state->record.identity.rxFirstSeq);
    TEST_ASSERT_EQUAL_UINT32(12, state->record.identity.rxLastSeq);
    TEST_ASSERT_EQUAL_UINT32(0x11112222, state->record.identity.payloadDigest);
    TEST_ASSERT_EQUAL_UINT32(9, state->record.stateRevision);
    TEST_ASSERT_EQUAL_UINT32(5, state->record.alertRevision);
    TEST_ASSERT_EQUAL_UINT32(0xAABBCCDD, state->record.alertTableDigest);
    TEST_ASSERT_EQUAL_UINT32(3, state->record.sourceLossCount);

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(V1CausalStage::AlertTableBaseline),
                            static_cast<uint8_t>(alerts->record.stage));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(V1CausalOutcome::Retained),
                            static_cast<uint8_t>(alerts->record.outcome));
    TEST_ASSERT_EQUAL_UINT32(900, alerts->record.stageDutMillis);
    TEST_ASSERT_EQUAL_UINT32(720, alerts->record.identity.dutMillis);
    TEST_ASSERT_EQUAL_UINT32(41, alerts->record.identity.eventSeq);
    TEST_ASSERT_EQUAL_UINT32(13, alerts->record.identity.rxFirstSeq);
    TEST_ASSERT_EQUAL_UINT32(14, alerts->record.identity.rxLastSeq);
    TEST_ASSERT_EQUAL_UINT32(0x33334444, alerts->record.identity.payloadDigest);
    TEST_ASSERT_EQUAL_UINT32(9, alerts->record.stateRevision);
    TEST_ASSERT_EQUAL_UINT32(5, alerts->record.alertRevision);
    TEST_ASSERT_EQUAL_UINT32(0xAABBCCDD, alerts->record.alertTableDigest);
    TEST_ASSERT_EQUAL_UINT32(3, alerts->record.sourceLossCount);
    TEST_ASSERT_NOT_NULL(std::strstr(logger.testGetLastTraceLine(), "ALERT_TABLE_BASELINE,RETAINED"));
}

void test_qualification_trace_does_not_invent_baselines_for_zero_sources() {
    V1EncounterLogger logger;
    logger.begin(true);

    V1SemanticRevisionEvidence evidence;
    evidence.stateRevision = 9;
    evidence.alertRevision = 5;
    evidence.alertTableDigest = 0xAABBCCDD;
    logger.testBeginQualificationSession(0xBEE69052, 900, evidence, 3);

    TEST_ASSERT_EQUAL_UINT32(1, logger.testTraceCount());
    TEST_ASSERT_NOT_NULL(std::strstr(logger.testGetLastTraceLine(), "SESSION_START,STARTED"));
}

void test_encounter_logger_ignores_raw_jitter_and_tracks_meaningful_edges() {
    V1EncounterLogger logger;
    logger.begin(true);

    AlertData alert = makeAlert(1, DIR_FRONT, 0x90, 0x70);
    logger.onAlertTable(&alert, 1, 100);

    alert.frontRawStrength = 0x91;
    logger.onAlertTable(&alert, 1, 200);
    TEST_ASSERT_EQUAL_UINT32(1, logger.testSnapshotCount());

    logger.onAlertTable(&alert, 1, 350);
    TEST_ASSERT_EQUAL_UINT32(1, logger.testSnapshotCount());

    logger.onAlertTable(&alert, 1, 5099);
    TEST_ASSERT_EQUAL_UINT32(1, logger.testSnapshotCount());

    logger.onAlertTable(&alert, 1, 5100);
    TEST_ASSERT_EQUAL_UINT32(2, logger.testSnapshotCount());
    TEST_ASSERT_NOT_NULL(std::strstr(logger.testGetLastLine(), ",SAMPLE,"));
    TEST_ASSERT_NOT_NULL(std::strstr(logger.testGetLastLine(), ",145,112,2,1,"));

    alert.frontStrength = 3;
    logger.onAlertTable(&alert, 1, 5200);
    TEST_ASSERT_EQUAL_UINT32(2, logger.testSnapshotCount());

    logger.onAlertTable(&alert, 1, 5350);
    TEST_ASSERT_EQUAL_UINT32(3, logger.testSnapshotCount());
    TEST_ASSERT_NOT_NULL(std::strstr(logger.testGetLastLine(), ",145,112,3,1,"));

    alert.direction = DIR_REAR;
    logger.onAlertTable(&alert, 1, 5360);
    TEST_ASSERT_EQUAL_UINT32(4, logger.testSnapshotCount());
    TEST_ASSERT_NOT_NULL(std::strstr(logger.testGetLastLine(), ",REAR,"));

    alert.frequency = 34701;
    logger.onAlertTable(&alert, 1, 5400);
    TEST_ASSERT_EQUAL_UINT32(4, logger.testSnapshotCount());

    logger.onAlertTable(&alert, 1, 5610);
    TEST_ASSERT_EQUAL_UINT32(5, logger.testSnapshotCount());
    TEST_ASSERT_NOT_NULL(std::strstr(logger.testGetLastLine(), ",34701,REAR,"));

    logger.onAlertTable(nullptr, 0, 5620);
    TEST_ASSERT_EQUAL_UINT32(6, logger.testSnapshotCount());
    TEST_ASSERT_NOT_NULL(std::strstr(logger.testGetLastLine(), ",END,"));

    logger.onAlertTable(nullptr, 0, 5630);
    TEST_ASSERT_EQUAL_UINT32(6, logger.testSnapshotCount());
}

void test_encounter_logger_keeps_v1_rows_separate_at_same_frequency() {
    V1EncounterLogger logger;
    logger.begin(true);

    AlertData alerts[2] = {makeAlert(1, DIR_FRONT, 0x90, 0x70), makeAlert(2, DIR_REAR, 0xA0, 0x80)};
    logger.onAlertTable(alerts, 2, 100);

    TEST_ASSERT_EQUAL_UINT32(1, logger.testSnapshotCount());
    TEST_ASSERT_EQUAL_UINT32(2, logger.testLineCount());
    TEST_ASSERT_NOT_NULL(std::strstr(logger.testGetLastLine(), ",2,2,Ka,34700,REAR,"));
}

void test_encounter_logger_begin_warms_storage_at_boot() {
    // Native tests use the UNIT_TEST sink, so pin the production-only warm-up
    // textually: begin() must pre-create the encounters file (directory +
    // create + header via ensureFileReady) under the SD lock, during setup and
    // before enabled_ flips true, so successful warm-up keeps the first alert
    // from paying FAT-allocation cost on the shared SD path mid-encounter.
    // Warm-up failure must stay non-fatal (lazy retry on first append).
    const std::string source = readProjectFile("src/modules/encounter/v1_encounter_logger.cpp");
    TEST_ASSERT_FALSE(source.empty());

    const size_t beginStart = source.find("void V1EncounterLogger::begin(");
    TEST_ASSERT_NOT_EQUAL(std::string::npos, beginStart);
    const size_t enabledTrue = source.find("enabled_ = true;", beginStart);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, enabledTrue);
    const std::string beginBody = source.substr(beginStart, enabledTrue - beginStart);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, beginBody.find("StorageManager::SDLockBlocking"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, beginBody.find("ensureFileReady()"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, beginBody.find("storage warm-up deferred to first alert"));
}

void test_qualification_status_exposes_identity_and_effective_display_settings() {
    const std::string qualification = readProjectFile("src/modules/qualification/qualification_serial_module.cpp");
    const std::string qualificationHeader = readProjectFile("src/modules/qualification/qualification_serial_module.h");
    const std::string runtimeWiring = readProjectFile("src/main_runtime_wiring.cpp");
    const std::string buildMetadata = readProjectFile("src/build_metadata.cpp");
    TEST_ASSERT_FALSE(qualification.empty());
    TEST_ASSERT_FALSE(qualificationHeader.empty());
    TEST_ASSERT_FALSE(runtimeWiring.empty());
    TEST_ASSERT_FALSE(buildMetadata.empty());
    TEST_ASSERT_NOT_NULL(std::strstr(qualification.c_str(), "\\\"dutMillis\\\""));
    TEST_ASSERT_NOT_NULL(std::strstr(qualification.c_str(), "\\\"sessionToken\\\""));
    TEST_ASSERT_NOT_NULL(std::strstr(qualification.c_str(), "\\\"causalTracePath\\\""));
    TEST_ASSERT_NOT_NULL(std::strstr(qualification.c_str(), "\\\"runtimeImageId\\\""));
    TEST_ASSERT_NOT_NULL(std::strstr(qualification.c_str(), "\\\"displaySettings\\\""));
    TEST_ASSERT_NOT_NULL(std::strstr(qualification.c_str(), "\\\"brightness\\\""));
    TEST_ASSERT_NOT_NULL(std::strstr(qualification.c_str(), "\\\"colorMutedRgb565\\\""));
    TEST_ASSERT_NOT_NULL(std::strstr(qualificationHeader.c_str(), "displayBrightness"));
    TEST_ASSERT_NOT_NULL(std::strstr(qualificationHeader.c_str(), "displayMutedColorRgb565"));
    TEST_ASSERT_NOT_NULL(std::strstr(
        runtimeWiring.c_str(), "providers.displayBrightness = [](void*) { return settingsManager.get().brightness; }"));
    TEST_ASSERT_NOT_NULL(
        std::strstr(runtimeWiring.c_str(),
                    "providers.displayMutedColorRgb565 = [](void*) { return settingsManager.get().colorMuted; }"));
    TEST_ASSERT_NOT_NULL(std::strstr(buildMetadata.c_str(), "esp_app_get_elf_sha256"));
}

void test_causal_trace_names_preparser_framing_rejections_and_stage_clock() {
    const std::string causalTypes = readProjectFile("src/causal_evidence_types.h");
    const std::string bleQueue = readProjectFile("src/modules/ble/ble_queue_module.cpp");
    const std::string encounter = readProjectFile("src/modules/encounter/v1_encounter_logger.cpp");
    TEST_ASSERT_FALSE(causalTypes.empty());
    TEST_ASSERT_FALSE(bleQueue.empty());
    TEST_ASSERT_FALSE(encounter.empty());
    TEST_ASSERT_NOT_NULL(std::strstr(causalTypes.c_str(), "uint32_t stageDutMillis"));
    TEST_ASSERT_NOT_NULL(std::strstr(bleQueue.c_str(), "V1CausalOutcome::ResyncNoStart"));
    TEST_ASSERT_NOT_NULL(std::strstr(bleQueue.c_str(), "V1CausalOutcome::ResyncZeroLength"));
    TEST_ASSERT_NOT_NULL(std::strstr(bleQueue.c_str(), "V1CausalOutcome::ResyncMissingEnd"));
    TEST_ASSERT_NOT_NULL(std::strstr(bleQueue.c_str(), "V1CausalOutcome::SessionClosedIncomplete"));
    TEST_ASSERT_NOT_NULL(std::strstr(encounter.c_str(), "stage_dut_millis,rx_dut_millis"));
    TEST_ASSERT_NOT_NULL(std::strstr(encounter.c_str(), "terminal_encounter_sample_seq"));
    TEST_ASSERT_NOT_NULL(std::strstr(encounter.c_str(), "RESYNC_DISCARDED_PREFIX"));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_encounter_logger_uses_compact_location_free_schema);
    RUN_TEST(test_encounter_logger_ignores_raw_jitter_and_tracks_meaningful_edges);
    RUN_TEST(test_encounter_logger_keeps_v1_rows_separate_at_same_frequency);
    RUN_TEST(test_qualification_trace_stamps_session_identity_digest_and_loss_fields);
    RUN_TEST(test_qualification_trace_retains_distinct_prestart_state_and_alert_sources);
    RUN_TEST(test_qualification_trace_does_not_invent_baselines_for_zero_sources);
    RUN_TEST(test_encounter_logger_begin_warms_storage_at_boot);
    RUN_TEST(test_qualification_status_exposes_identity_and_effective_display_settings);
    RUN_TEST(test_causal_trace_names_preparser_framing_rejections_and_stage_clock);
    return UNITY_END();
}

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
    TEST_ASSERT_EQUAL_STRING("100,1,1,START,3,1,Ka,34700,SIDE,145,114,2,1,1,0,0,0\n", logger.testGetLastLine());
    TEST_ASSERT_NULL(std::strstr(logger.testGetLastLine(), "gps"));
    TEST_ASSERT_NULL(std::strstr(logger.testGetLastLine(), "speed"));
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

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_encounter_logger_uses_compact_location_free_schema);
    RUN_TEST(test_encounter_logger_ignores_raw_jitter_and_tracks_meaningful_edges);
    RUN_TEST(test_encounter_logger_keeps_v1_rows_separate_at_same_frequency);
    RUN_TEST(test_encounter_logger_begin_warms_storage_at_boot);
    return UNITY_END();
}

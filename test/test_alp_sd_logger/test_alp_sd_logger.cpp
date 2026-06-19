/**
 * AlpSdLogger unit tests.
 *
 * Covers:
 *   - setBootId() filename path generation (both token forms)
 *   - formatCsvRow() output via log methods (column count, separators,
 *     hex encoding, UTC-empty-when-null, newline termination)
 *   - enabled/sdReady guard: log methods are no-ops when logger is idle
 *
 * Hardware dependencies (SD, UART, GPIO) are all behind #ifndef UNIT_TEST
 * guards in alp_sd_logger.cpp and alp_runtime_module.cpp.  appendLine()
 * in UNIT_TEST mode stores the last formatted line in lastLineBuf_ which
 * is exposed via testGetLastLine() / testClearLastLine().
 */

#include <unity.h>
#include <cstring>
#include <cstdlib>
#include <string>

#include "../../src/modules/alp/alp_runtime_module.h"
#include "../../src/modules/alp/alp_sd_logger.h"
#include "../../src/modules/gps/gps_publishers.cpp"
#include "../../src/modules/alp/alp_sd_logger.cpp"
#include "../../src/modules/alp/alp_runtime_module.cpp"
#include "../../src/modules/system/system_event_bus.h"

// ── helpers ──────────────────────────────────────────────────────────

// Count the number of commas in a NUL-terminated string.
static int countCommas(const char* s) {
    int n = 0;
    while (*s) {
        if (*s++ == ',') n++;
    }
    return n;
}

// Return the Nth comma-separated field (0-indexed) from a CSV row string.
static std::string nthField(const char* row, int n) {
    std::string s(row);
    int start = 0;
    for (int i = 0; i < n; i++) {
        size_t pos = s.find(',', start);
        if (pos == std::string::npos) return "";
        start = static_cast<int>(pos) + 1;
    }
    size_t end = s.find(',', start);
    if (end == std::string::npos) end = s.find('\n', start);
    if (end == std::string::npos) end = s.size();
    return s.substr(start, end - start);
}

// ── test state ───────────────────────────────────────────────────────

static AlpSdLogger logger;

void setUp() {
    logger.testClearLastLine();
}

void tearDown() {}

// ── setBootId path format ─────────────────────────────────────────────

void test_setBootId_with_zero_token_produces_legacy_path() {
    logger.begin(false, false, nullptr);  // not enabled; only tests path format
    logger.setBootId(42, 0);
    TEST_ASSERT_EQUAL_STRING("/alp/alp_42.csv", logger.csvPath());
}

void test_setBootId_with_nonzero_token_produces_hyphenated_path() {
    logger.begin(false, false, nullptr);
    logger.setBootId(7, 0xDEADBEEF);
    TEST_ASSERT_EQUAL_STRING("/alp/alp_7-deadbeef.csv", logger.csvPath());
}

void test_setBootId_zero_id_zero_token_produces_alp_0_path() {
    logger.begin(false, false, nullptr);
    logger.setBootId(0, 0);
    TEST_ASSERT_EQUAL_STRING("/alp/alp_0.csv", logger.csvPath());
}

void test_setBootId_large_id_and_token_formats_correctly() {
    logger.begin(false, false, nullptr);
    logger.setBootId(65535, 0x00000001);
    TEST_ASSERT_EQUAL_STRING("/alp/alp_65535-00000001.csv", logger.csvPath());
}

void test_setBootId_resets_path_on_second_call() {
    logger.begin(false, false, nullptr);
    logger.setBootId(1, 0xABCD1234);
    logger.setBootId(2, 0);
    // Second call must overwrite, not concatenate.
    TEST_ASSERT_EQUAL_STRING("/alp/alp_2.csv", logger.csvPath());
}

// ── enabled/sdReady guard ─────────────────────────────────────────────

void test_log_is_noop_when_disabled() {
    logger.begin(false, false, nullptr);
    logger.setBootId(1, 0);
    logger.logStateTransition(1000, AlpState::IDLE, AlpState::LISTENING);
    TEST_ASSERT_EQUAL_STRING("", logger.testGetLastLine());
}

void test_log_is_noop_when_sd_not_ready() {
    logger.begin(true, false, nullptr);
    logger.setBootId(1, 0);
    logger.logStateTransition(1000, AlpState::IDLE, AlpState::LISTENING);
    TEST_ASSERT_EQUAL_STRING("", logger.testGetLastLine());
}

// ── CSV row format ────────────────────────────────────────────────────

void test_logStateTransition_produces_12_column_row() {
    logger.begin(true, true, nullptr);
    logger.setBootId(1, 0);
    logger.logStateTransition(5000, AlpState::IDLE, AlpState::LISTENING, "FRONT");
    const char* line = logger.testGetLastLine();
    // 12 columns → 11 commas
    TEST_ASSERT_EQUAL_INT(11, countCommas(line));
}

void test_logStateTransition_row_ends_with_newline() {
    logger.begin(true, true, nullptr);
    logger.setBootId(1, 0);
    logger.logStateTransition(5000, AlpState::IDLE, AlpState::LISTENING);
    const char* line = logger.testGetLastLine();
    size_t len = strlen(line);
    TEST_ASSERT_GREATER_THAN_INT(0, (int)len);
    TEST_ASSERT_EQUAL_CHAR('\n', line[len - 1]);
}

void test_logStateTransition_millis_is_first_field() {
    logger.begin(true, true, nullptr);
    logger.setBootId(1, 0);
    logger.logStateTransition(12345, AlpState::LISTENING, AlpState::ALERT_ACTIVE);
    TEST_ASSERT_EQUAL_STRING("12345", nthField(logger.testGetLastLine(), 0).c_str());
}

void test_logStateTransition_utc_empty_without_gps() {
    logger.begin(true, true, nullptr);
    logger.setBootId(1, 0);
    logger.logStateTransition(1000, AlpState::IDLE, AlpState::LISTENING);
    // utc is column 1; without a GpsTimePublisher it must be empty
    TEST_ASSERT_EQUAL_STRING("", nthField(logger.testGetLastLine(), 1).c_str());
}

void test_logStateTransition_event_field_is_STATE() {
    logger.begin(true, true, nullptr);
    logger.setBootId(1, 0);
    logger.logStateTransition(1000, AlpState::IDLE, AlpState::LISTENING);
    TEST_ASSERT_EQUAL_STRING("STATE", nthField(logger.testGetLastLine(), 2).c_str());
}

void test_logStateTransition_from_and_to_states_are_columns_3_and_4() {
    logger.begin(true, true, nullptr);
    logger.setBootId(1, 0);
    logger.logStateTransition(1000, AlpState::IDLE, AlpState::LISTENING);
    TEST_ASSERT_EQUAL_STRING(alpStateName(AlpState::IDLE),
                             nthField(logger.testGetLastLine(), 3).c_str());
    TEST_ASSERT_EQUAL_STRING(alpStateName(AlpState::LISTENING),
                             nthField(logger.testGetLastLine(), 4).c_str());
}

void test_logHeartbeatByte1_event_field_is_HB_BYTE1() {
    logger.begin(true, true, nullptr);
    logger.setBootId(1, 0);
    logger.logHeartbeatByte1(2000, 0x02, 0x01, AlpState::LISTENING, "FRONT");
    TEST_ASSERT_EQUAL_STRING("HB_BYTE1", nthField(logger.testGetLastLine(), 2).c_str());
}

void test_logHeartbeatByte1_hex_encodes_bytes_uppercase() {
    logger.begin(true, true, nullptr);
    logger.setBootId(1, 0);
    // prevByte1=0x0A, newByte1=0xFF
    logger.logHeartbeatByte1(3000, 0x0A, 0xFF, AlpState::LISTENING);
    // byte0 is column 5 (prevByte1), byte1 is column 6 (newByte1)
    TEST_ASSERT_EQUAL_STRING("0A", nthField(logger.testGetLastLine(), 5).c_str());
    TEST_ASSERT_EQUAL_STRING("FF", nthField(logger.testGetLastLine(), 6).c_str());
}

void test_logHeartbeatByte1_produces_12_column_row() {
    logger.begin(true, true, nullptr);
    logger.setBootId(1, 0);
    logger.logHeartbeatByte1(2000, 0x03, 0x01, AlpState::LISTENING);
    TEST_ASSERT_EQUAL_INT(11, countCommas(logger.testGetLastLine()));
}

// ── isEnabled() ──────────────────────────────────────────────────────

void test_isEnabled_false_when_not_started() {
    AlpSdLogger fresh;
    TEST_ASSERT_FALSE(fresh.isEnabled());
}

void test_isEnabled_false_when_enabled_but_sd_not_ready() {
    logger.begin(true, false, nullptr);
    TEST_ASSERT_FALSE(logger.isEnabled());
}

void test_isEnabled_true_when_both_enabled_and_sd_ready() {
    logger.begin(true, true, nullptr);
    TEST_ASSERT_TRUE(logger.isEnabled());
}

// ── main ──────────────────────────────────────────────────────────────

int main() {
    UNITY_BEGIN();

    // setBootId path format
    RUN_TEST(test_setBootId_with_zero_token_produces_legacy_path);
    RUN_TEST(test_setBootId_with_nonzero_token_produces_hyphenated_path);
    RUN_TEST(test_setBootId_zero_id_zero_token_produces_alp_0_path);
    RUN_TEST(test_setBootId_large_id_and_token_formats_correctly);
    RUN_TEST(test_setBootId_resets_path_on_second_call);

    // enabled/sdReady guard
    RUN_TEST(test_log_is_noop_when_disabled);
    RUN_TEST(test_log_is_noop_when_sd_not_ready);

    // CSV row format
    RUN_TEST(test_logStateTransition_produces_12_column_row);
    RUN_TEST(test_logStateTransition_row_ends_with_newline);
    RUN_TEST(test_logStateTransition_millis_is_first_field);
    RUN_TEST(test_logStateTransition_utc_empty_without_gps);
    RUN_TEST(test_logStateTransition_event_field_is_STATE);
    RUN_TEST(test_logStateTransition_from_and_to_states_are_columns_3_and_4);
    RUN_TEST(test_logHeartbeatByte1_event_field_is_HB_BYTE1);
    RUN_TEST(test_logHeartbeatByte1_hex_encodes_bytes_uppercase);
    RUN_TEST(test_logHeartbeatByte1_produces_12_column_row);

    // isEnabled()
    RUN_TEST(test_isEnabled_false_when_not_started);
    RUN_TEST(test_isEnabled_false_when_enabled_but_sd_not_ready);
    RUN_TEST(test_isEnabled_true_when_both_enabled_and_sd_ready);

    return UNITY_END();
}

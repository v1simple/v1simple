#include <unity.h>

#include "../../src/modules/obd/obd_elm327_parser.h"
#include "../../src/modules/obd/obd_elm327_parser.cpp"

void setUp() {}
void tearDown() {}

// ── Valid speed responses ──────────────────────────────────────────

void test_parse_speed_with_spaces() {
    const char* resp = "41 0D 3C";
    Elm327ParseResult r = parseElm327Response(resp, strlen(resp));
    TEST_ASSERT_TRUE(r.valid);
    TEST_ASSERT_EQUAL_UINT8(0x41, r.service);
    TEST_ASSERT_EQUAL_UINT8(0x0D, r.pid);
    TEST_ASSERT_EQUAL_UINT8(1, r.dataLen);
    TEST_ASSERT_EQUAL_UINT8(0x3C, r.dataBytes[0]);
    TEST_ASSERT_FALSE(r.noData);
    TEST_ASSERT_FALSE(r.error);
    TEST_ASSERT_FALSE(r.busInit);
}

void test_parse_speed_without_spaces() {
    const char* resp = "410D3C";
    Elm327ParseResult r = parseElm327Response(resp, strlen(resp));
    TEST_ASSERT_TRUE(r.valid);
    TEST_ASSERT_EQUAL_UINT8(0x41, r.service);
    TEST_ASSERT_EQUAL_UINT8(0x0D, r.pid);
    TEST_ASSERT_EQUAL_UINT8(1, r.dataLen);
    TEST_ASSERT_EQUAL_UINT8(0x3C, r.dataBytes[0]);
}

void test_parse_speed_zero() {
    const char* resp = "41 0D 00";
    Elm327ParseResult r = parseElm327Response(resp, strlen(resp));
    TEST_ASSERT_TRUE(r.valid);
    TEST_ASSERT_EQUAL_UINT8(0x00, r.dataBytes[0]);
    float kmh = decodeSpeedKmh(r);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, kmh);
}

void test_parse_speed_max_255() {
    const char* resp = "41 0D FF";
    Elm327ParseResult r = parseElm327Response(resp, strlen(resp));
    TEST_ASSERT_TRUE(r.valid);
    TEST_ASSERT_EQUAL_UINT8(0xFF, r.dataBytes[0]);
    float kmh = decodeSpeedKmh(r);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 255.0f, kmh);
}

void test_decode_speed_60kmh() {
    const char* resp = "41 0D 3C";
    Elm327ParseResult r = parseElm327Response(resp, strlen(resp));
    float kmh = decodeSpeedKmh(r);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 60.0f, kmh);
    float mph = kmhToMph(kmh);
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 37.28f, mph);
}

void test_decode_speed_100kmh() {
    const char* resp = "41 0D 64";
    Elm327ParseResult r = parseElm327Response(resp, strlen(resp));
    float kmh = decodeSpeedKmh(r);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 100.0f, kmh);
}

// ── Response with trailing whitespace / prompt ────────────────────

void test_parse_response_with_trailing_cr_lf() {
    const char* resp = "41 0D 3C\r\n";
    Elm327ParseResult r = parseElm327Response(resp, strlen(resp));
    TEST_ASSERT_TRUE(r.valid);
    TEST_ASSERT_EQUAL_UINT8(0x3C, r.dataBytes[0]);
}

void test_parse_response_with_prompt() {
    const char* resp = "41 0D 3C\r\n>";
    Elm327ParseResult r = parseElm327Response(resp, strlen(resp));
    TEST_ASSERT_TRUE(r.valid);
    TEST_ASSERT_EQUAL_UINT8(0x3C, r.dataBytes[0]);
}

void test_parse_response_with_leading_whitespace() {
    const char* resp = "\r\n41 0D 3C\r\n";
    Elm327ParseResult r = parseElm327Response(resp, strlen(resp));
    TEST_ASSERT_TRUE(r.valid);
    TEST_ASSERT_EQUAL_UINT8(0x3C, r.dataBytes[0]);
}

// ── NO DATA ───────────────────────────────────────────────────────

void test_parse_no_data() {
    const char* resp = "NO DATA";
    Elm327ParseResult r = parseElm327Response(resp, strlen(resp));
    TEST_ASSERT_FALSE(r.valid);
    TEST_ASSERT_TRUE(r.noData);
    TEST_ASSERT_FALSE(r.error);
}

void test_parse_no_data_with_whitespace() {
    const char* resp = "NO DATA\r\n>";
    Elm327ParseResult r = parseElm327Response(resp, strlen(resp));
    TEST_ASSERT_TRUE(r.noData);
    TEST_ASSERT_FALSE(r.valid);
}

// ── Error responses ───────────────────────────────────────────────

void test_parse_question_mark_error() {
    const char* resp = "?";
    Elm327ParseResult r = parseElm327Response(resp, strlen(resp));
    TEST_ASSERT_FALSE(r.valid);
    TEST_ASSERT_TRUE(r.error);
}

void test_parse_unable_to_connect() {
    const char* resp = "UNABLE TO CONNECT";
    Elm327ParseResult r = parseElm327Response(resp, strlen(resp));
    TEST_ASSERT_FALSE(r.valid);
    TEST_ASSERT_TRUE(r.error);
}

void test_parse_can_error() {
    const char* resp = "CAN ERROR";
    Elm327ParseResult r = parseElm327Response(resp, strlen(resp));
    TEST_ASSERT_FALSE(r.valid);
    TEST_ASSERT_TRUE(r.error);
}

void test_parse_buffer_full() {
    const char* resp = "BUFFER FULL";
    Elm327ParseResult r = parseElm327Response(resp, strlen(resp));
    TEST_ASSERT_FALSE(r.valid);
    TEST_ASSERT_TRUE(r.error);
}

void test_parse_bus_error() {
    const char* resp = "BUS ERROR";
    Elm327ParseResult r = parseElm327Response(resp, strlen(resp));
    TEST_ASSERT_FALSE(r.valid);
    TEST_ASSERT_TRUE(r.error);
}

void test_parse_stopped() {
    const char* resp = "STOPPED";
    Elm327ParseResult r = parseElm327Response(resp, strlen(resp));
    TEST_ASSERT_FALSE(r.valid);
    TEST_ASSERT_TRUE(r.error);
}

// ── Bus init / searching ──────────────────────────────────────────

void test_parse_searching() {
    const char* resp = "SEARCHING...";
    Elm327ParseResult r = parseElm327Response(resp, strlen(resp));
    TEST_ASSERT_FALSE(r.valid);
    TEST_ASSERT_TRUE(r.busInit);
    TEST_ASSERT_FALSE(r.error);
}

void test_parse_bus_init() {
    const char* resp = "BUS INIT: ...OK";
    Elm327ParseResult r = parseElm327Response(resp, strlen(resp));
    TEST_ASSERT_FALSE(r.valid);
    TEST_ASSERT_TRUE(r.busInit);
}

// ── Edge cases ────────────────────────────────────────────────────

void test_parse_empty_string() {
    const char* resp = "";
    Elm327ParseResult r = parseElm327Response(resp, 0);
    TEST_ASSERT_FALSE(r.valid);
    TEST_ASSERT_TRUE(r.error);
}

void test_parse_null_pointer() {
    Elm327ParseResult r = parseElm327Response(nullptr, 0);
    TEST_ASSERT_FALSE(r.valid);
    TEST_ASSERT_TRUE(r.error);
}

void test_parse_whitespace_only() {
    const char* resp = "  \r\n  ";
    Elm327ParseResult r = parseElm327Response(resp, strlen(resp));
    TEST_ASSERT_FALSE(r.valid);
    TEST_ASSERT_TRUE(r.error);
}

void test_parse_single_hex_byte_too_short() {
    const char* resp = "41";
    Elm327ParseResult r = parseElm327Response(resp, strlen(resp));
    // Only service byte, no PID — still technically 2 hex digits = 1 byte
    // Need at least service + PID = 2 bytes
    TEST_ASSERT_FALSE(r.valid);
    TEST_ASSERT_TRUE(r.error);
}

void test_parse_service_and_pid_only() {
    // "41 0D" with no data byte — valid parse but no speed data
    const char* resp = "41 0D";
    Elm327ParseResult r = parseElm327Response(resp, strlen(resp));
    TEST_ASSERT_TRUE(r.valid);
    TEST_ASSERT_EQUAL_UINT8(0x41, r.service);
    TEST_ASSERT_EQUAL_UINT8(0x0D, r.pid);
    TEST_ASSERT_EQUAL_UINT8(0, r.dataLen);
    // decodeSpeedKmh should reject — no data byte
    float kmh = decodeSpeedKmh(r);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, -1.0f, kmh);
}

void test_parse_truncated_hex() {
    // Odd number of hex chars
    const char* resp = "41 0D 3";
    Elm327ParseResult r = parseElm327Response(resp, strlen(resp));
    TEST_ASSERT_FALSE(r.valid);
    TEST_ASSERT_TRUE(r.error);
}

void test_parse_invalid_hex_chars() {
    const char* resp = "41 0D ZZ";
    Elm327ParseResult r = parseElm327Response(resp, strlen(resp));
    TEST_ASSERT_FALSE(r.valid);
    TEST_ASSERT_TRUE(r.error);
}

void test_parse_lowercase_hex() {
    const char* resp = "41 0d 3c";
    Elm327ParseResult r = parseElm327Response(resp, strlen(resp));
    TEST_ASSERT_TRUE(r.valid);
    TEST_ASSERT_EQUAL_UINT8(0x0D, r.pid);
    TEST_ASSERT_EQUAL_UINT8(0x3C, r.dataBytes[0]);
}

void test_parse_multi_byte_data() {
    // Service 01 PID 0C (RPM) returns 2 data bytes
    const char* resp = "41 0C 1A F8";
    Elm327ParseResult r = parseElm327Response(resp, strlen(resp));
    TEST_ASSERT_TRUE(r.valid);
    TEST_ASSERT_EQUAL_UINT8(0x41, r.service);
    TEST_ASSERT_EQUAL_UINT8(0x0C, r.pid);
    TEST_ASSERT_EQUAL_UINT8(2, r.dataLen);
    TEST_ASSERT_EQUAL_UINT8(0x1A, r.dataBytes[0]);
    TEST_ASSERT_EQUAL_UINT8(0xF8, r.dataBytes[1]);
}

void test_parse_mode_22_response_sets_did() {
    const char* resp = "62 F4 5C 78";
    Elm327ParseResult r = parseElm327Response(resp, strlen(resp));
    TEST_ASSERT_TRUE(r.valid);
    TEST_ASSERT_EQUAL_UINT8(0x62, r.service);
    TEST_ASSERT_EQUAL_UINT16(0xF45C, r.did);
    TEST_ASSERT_EQUAL_UINT8(1, r.dataLen);
    TEST_ASSERT_EQUAL_UINT8(0x78, r.dataBytes[0]);
}

void test_parse_response_ignores_echo_and_status_lines() {
    const char* resp =
        "ATSH 7E0\r\n"
        "010D\r\n"
        "SEARCHING...\r\n"
        "41 0D 3C\r\n"
        ">";
    Elm327ParseResult r = parseElm327Response(resp, strlen(resp));
    TEST_ASSERT_TRUE(r.valid);
    TEST_ASSERT_EQUAL_UINT8(0x41, r.service);
    TEST_ASSERT_EQUAL_UINT8(0x0D, r.pid);
    TEST_ASSERT_EQUAL_UINT8(0x3C, r.dataBytes[0]);
    TEST_ASSERT_FALSE(r.noData);
    TEST_ASSERT_FALSE(r.error);
}

void test_parse_too_many_lines_reports_error() {
    const char* resp =
        "NO DATA\r\nNO DATA\r\nNO DATA\r\nNO DATA\r\nNO DATA\r\nNO DATA\r\n"
        "NO DATA\r\nNO DATA\r\nNO DATA\r\nNO DATA\r\nNO DATA\r\nNO DATA\r\n"
        "NO DATA\r\n>";
    Elm327ParseResult r = parseElm327Response(resp, strlen(resp));
    TEST_ASSERT_FALSE(r.valid);
    TEST_ASSERT_FALSE(r.noData);
    TEST_ASSERT_TRUE(r.error);
}

void test_parse_vin_multiline_response() {
    const char* resp =
        "0902\r\n"
        "0: 49 02 01 31 46 54\r\n"
        "1: 57 31 45 54 37 44\r\n"
        "2: 46 41 31 32 33 34\r\n"
        "3: 35 36\r\n"
        ">";
    Elm327VinParseResult vin = parseVinResponse(resp, strlen(resp));
    TEST_ASSERT_TRUE(vin.valid);
    TEST_ASSERT_EQUAL_STRING("1FTW1ET7DFA123456", vin.vin);
}

void test_parse_vin_multiline_response_with_echo_and_status_lines() {
    const char* resp =
        "0902\r\n"
        "SEARCHING...\r\n"
        "0: 49 02 01 31 46 54\r\n"
        "1: 57 31 45 54 37 44\r\n"
        "OK\r\n"
        "2: 46 41 31 32 33 34\r\n"
        "3: 35 36\r\n"
        ">";
    Elm327VinParseResult vin = parseVinResponse(resp, strlen(resp));
    TEST_ASSERT_TRUE(vin.valid);
    TEST_ASSERT_EQUAL_STRING("1FTW1ET7DFA123456", vin.vin);
}

void test_parse_vin_no_data() {
    const char* resp = "0902\r\nNO DATA\r\n>";
    Elm327VinParseResult vin = parseVinResponse(resp, strlen(resp));
    TEST_ASSERT_FALSE(vin.valid);
    TEST_ASSERT_TRUE(vin.noData);
    TEST_ASSERT_FALSE(vin.error);
}

void test_decode_u8_offset40_temperature() {
    Elm327ParseResult r;
    r.valid = true;
    r.dataLen = 1;
    r.dataBytes[0] = 0x64;
    int16_t temp = 0;
    TEST_ASSERT_TRUE(decodeTempC_x10(r, Elm327TempDecodeFormat::U8_OFFSET40, temp));
    TEST_ASSERT_EQUAL_INT16(600, temp);
}

void test_decode_u16_div10_offset40_temperature() {
    Elm327ParseResult r;
    r.valid = true;
    r.dataLen = 2;
    r.dataBytes[0] = 0x17;
    r.dataBytes[1] = 0x70;  // 6000 / 10 - 40 = 20.0 C
    int16_t temp = 0;
    TEST_ASSERT_TRUE(decodeTempC_x10(r, Elm327TempDecodeFormat::U16_DIV10_OFFSET40, temp));
    TEST_ASSERT_EQUAL_INT16(200, temp);
}

void test_decode_u16_raw_offset40_temperature() {
    Elm327ParseResult r;
    r.valid = true;
    r.dataLen = 2;
    r.dataBytes[0] = 0x01;
    r.dataBytes[1] = 0x90;  // 400 - 40 = 0.0 C
    int16_t temp = 0;
    TEST_ASSERT_TRUE(decodeTempC_x10(r, Elm327TempDecodeFormat::U16_RAW_OFFSET40, temp));
    TEST_ASSERT_EQUAL_INT16(0, temp);
}

// ── decodeSpeedKmh edge cases ─────────────────────────────────────

void test_decode_speed_wrong_service() {
    Elm327ParseResult r;
    r.valid = true;
    r.service = 0x42;  // Not 0x41
    r.pid = 0x0D;
    r.dataLen = 1;
    r.dataBytes[0] = 60;
    TEST_ASSERT_FLOAT_WITHIN(0.01f, -1.0f, decodeSpeedKmh(r));
}

void test_decode_speed_wrong_pid() {
    Elm327ParseResult r;
    r.valid = true;
    r.service = 0x41;
    r.pid = 0x0C;  // RPM, not speed
    r.dataLen = 1;
    r.dataBytes[0] = 60;
    TEST_ASSERT_FLOAT_WITHIN(0.01f, -1.0f, decodeSpeedKmh(r));
}

void test_decode_speed_invalid_result() {
    Elm327ParseResult r;
    r.valid = false;
    TEST_ASSERT_FLOAT_WITHIN(0.01f, -1.0f, decodeSpeedKmh(r));
}

// ── kmhToMph ──────────────────────────────────────────────────────

void test_kmh_to_mph_conversion() {
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 62.14f, kmhToMph(100.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, kmhToMph(0.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 158.45f, kmhToMph(255.0f));
}

int main() {
    UNITY_BEGIN();

    // Valid speed responses
    RUN_TEST(test_parse_speed_with_spaces);
    RUN_TEST(test_parse_speed_without_spaces);
    RUN_TEST(test_parse_speed_zero);
    RUN_TEST(test_parse_speed_max_255);
    RUN_TEST(test_decode_speed_60kmh);
    RUN_TEST(test_decode_speed_100kmh);

    // Whitespace / prompt handling
    RUN_TEST(test_parse_response_with_trailing_cr_lf);
    RUN_TEST(test_parse_response_with_prompt);
    RUN_TEST(test_parse_response_with_leading_whitespace);

    // NO DATA
    RUN_TEST(test_parse_no_data);
    RUN_TEST(test_parse_no_data_with_whitespace);

    // Error responses
    RUN_TEST(test_parse_question_mark_error);
    RUN_TEST(test_parse_unable_to_connect);
    RUN_TEST(test_parse_can_error);
    RUN_TEST(test_parse_buffer_full);
    RUN_TEST(test_parse_bus_error);
    RUN_TEST(test_parse_stopped);

    // Bus init / searching
    RUN_TEST(test_parse_searching);
    RUN_TEST(test_parse_bus_init);

    // Edge cases
    RUN_TEST(test_parse_empty_string);
    RUN_TEST(test_parse_null_pointer);
    RUN_TEST(test_parse_whitespace_only);
    RUN_TEST(test_parse_single_hex_byte_too_short);
    RUN_TEST(test_parse_service_and_pid_only);
    RUN_TEST(test_parse_truncated_hex);
    RUN_TEST(test_parse_invalid_hex_chars);
    RUN_TEST(test_parse_lowercase_hex);
    RUN_TEST(test_parse_multi_byte_data);
    RUN_TEST(test_parse_mode_22_response_sets_did);
    RUN_TEST(test_parse_response_ignores_echo_and_status_lines);
    RUN_TEST(test_parse_too_many_lines_reports_error);
    RUN_TEST(test_parse_vin_multiline_response);
    RUN_TEST(test_parse_vin_multiline_response_with_echo_and_status_lines);
    RUN_TEST(test_parse_vin_no_data);
    RUN_TEST(test_decode_u8_offset40_temperature);
    RUN_TEST(test_decode_u16_div10_offset40_temperature);
    RUN_TEST(test_decode_u16_raw_offset40_temperature);

    // decodeSpeedKmh edge cases
    RUN_TEST(test_decode_speed_wrong_service);
    RUN_TEST(test_decode_speed_wrong_pid);
    RUN_TEST(test_decode_speed_invalid_result);

    // kmhToMph
    RUN_TEST(test_kmh_to_mph_conversion);

    return UNITY_END();
}

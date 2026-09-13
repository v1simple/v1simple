#include <unity.h>

#include <cstring>

#include "../mocks/mock_heap_caps_state.h"
#include "../mocks/esp_heap_caps.h"
#include "../../src/json_exact_input.h"

void setUp() { mock_reset_heap_caps(); }
void tearDown() {}

void test_accepts_one_value_and_trailing_json_whitespace() {
    const char input[] = " {\"unicode\":\"Road \\u2603\",\"nested\":[true,null,-1.25e+2]} \r\n\t";
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ExactJsonInput::Status::Ok),
                          static_cast<int>(ExactJsonInput::validate(input, sizeof(input) - 1u)));
}

void test_rejects_trailing_second_root_raw_nul_and_malformed_utf8() {
    const char trailing[] = "{\"ok\":true}garbage";
    const char second[] = "{\"ok\":true}{}";
    const char rawNul[] = {'{','\"','o','k','\"',':','t','r','u','e','}',0,'x'};
    const uint8_t overlong[] = {'{','\"','x','\"',':','\"',0xc0,0xaf,'\"','}'};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ExactJsonInput::Status::Invalid),
                          static_cast<int>(ExactJsonInput::validate(trailing, sizeof(trailing) - 1u)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ExactJsonInput::Status::Invalid),
                          static_cast<int>(ExactJsonInput::validate(second, sizeof(second) - 1u)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ExactJsonInput::Status::Invalid),
                          static_cast<int>(ExactJsonInput::validate(rawNul, sizeof(rawNul))));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ExactJsonInput::Status::Invalid),
                          static_cast<int>(ExactJsonInput::validate(overlong, sizeof(overlong))));
}

void test_rejects_decoded_nul_and_duplicate_keys_including_escaped_equivalent() {
    const char nul[] = "{\"name\":\"abc\\u0000def\"}";
    const char duplicate[] = "{\"name\":1,\"name\":2}";
    const char escapedDuplicate[] = "{\"name\":1,\"n\\u0061me\":2}";
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ExactJsonInput::Status::Invalid),
                          static_cast<int>(ExactJsonInput::validate(nul, sizeof(nul) - 1u)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ExactJsonInput::Status::Invalid),
                          static_cast<int>(ExactJsonInput::validate(duplicate, sizeof(duplicate) - 1u)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ExactJsonInput::Status::Invalid),
                          static_cast<int>(ExactJsonInput::validate(escapedDuplicate,
                                                                    sizeof(escapedDuplicate) - 1u)));
}

void test_rejects_unserializable_c0_but_accepts_json_escaped_controls() {
    const char unsafe[] = "{\"slot\":\"A\\u0001B\"}";
    const char safe[] = "{\"description\":\"line1\\nline2\\tend\\b\\f\\r\"}";
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ExactJsonInput::Status::Invalid),
                          static_cast<int>(ExactJsonInput::validate(unsafe, sizeof(unsafe) - 1u)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ExactJsonInput::Status::Ok),
                          static_cast<int>(ExactJsonInput::validate(safe, sizeof(safe) - 1u)));
}

void test_hash_prefilter_never_replaces_exact_decoded_key_comparison() {
    // Distinct same-length keys exercise the exact comparison after the shared
    // object/hash-table bookkeeping path; lexical uniqueness is deterministic,
    // not a hash-only probabilistic decision.
    const char distinct[] = "{\"aaaa\":1,\"aaab\":2,\"aaba\":3,\"abaa\":4}";
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ExactJsonInput::Status::Ok),
                          static_cast<int>(ExactJsonInput::validate(distinct, sizeof(distinct) - 1u)));
}

void test_key_bookkeeping_psram_exhaustion_is_distinct_and_leak_free() {
    const char input[] = "{\"a\":1}";
    g_mock_heap_caps_fail_all_allocations = true;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ExactJsonInput::Status::MemoryUnavailable),
                          static_cast<int>(ExactJsonInput::validate(input, sizeof(input) - 1u)));
    TEST_ASSERT_EQUAL_UINT32(0u, g_mock_heap_caps_outstanding_allocations);
    TEST_ASSERT_EQUAL_UINT32(MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM,
                             g_mock_heap_caps_last_realloc_caps);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_accepts_one_value_and_trailing_json_whitespace);
    RUN_TEST(test_rejects_trailing_second_root_raw_nul_and_malformed_utf8);
    RUN_TEST(test_rejects_decoded_nul_and_duplicate_keys_including_escaped_equivalent);
    RUN_TEST(test_rejects_unserializable_c0_but_accepts_json_escaped_controls);
    RUN_TEST(test_hash_prefilter_never_replaces_exact_decoded_key_comparison);
    RUN_TEST(test_key_bookkeeping_psram_exhaustion_is_distinct_and_leak_free);
    return UNITY_END();
}

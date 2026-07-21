#include <unity.h>

#include <ArduinoJson.h>
#include <cstring>

#include "../../include/v1_settings_json.h"

namespace {

constexpr uint8_t kSentinel[V1SettingsJson::kSettingsByteCount] = {91, 92, 93, 94, 95, 96};

void assertRejectedWithoutMutation(const char* json) {
    JsonDocument doc;
    TEST_ASSERT_FALSE(deserializeJson(doc, json));

    uint8_t output[V1SettingsJson::kSettingsByteCount];
    memcpy(output, kSentinel, sizeof(output));

    const JsonVariantConst rawBytes = doc["bytes"];
    TEST_ASSERT_FALSE(V1SettingsJson::parseRawBytes(rawBytes, output));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(kSentinel, output, V1SettingsJson::kSettingsByteCount);
}

} // namespace

void setUp() {}
void tearDown() {}

void test_parse_raw_bytes_accepts_integer_boundaries() {
    JsonDocument doc;
    TEST_ASSERT_FALSE(deserializeJson(doc, "{\"bytes\":[0,1,127,128,254,255]}"));

    uint8_t output[V1SettingsJson::kSettingsByteCount] = {};
    const JsonVariantConst rawBytes = doc["bytes"];

    TEST_ASSERT_TRUE(V1SettingsJson::parseRawBytes(rawBytes, output));
    const uint8_t expected[V1SettingsJson::kSettingsByteCount] = {0, 1, 127, 128, 254, 255};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, output, V1SettingsJson::kSettingsByteCount);
}

void test_parse_raw_bytes_rejects_non_integer_elements_atomically() {
    assertRejectedWithoutMutation("{\"bytes\":[1,2,3,4,5,\"6\"]}");
    assertRejectedWithoutMutation("{\"bytes\":[1,2,3,4,5,true]}");
    assertRejectedWithoutMutation("{\"bytes\":[1,2,3,4,5,null]}");
    assertRejectedWithoutMutation("{\"bytes\":[1,2,3,4,5,5.5]}");
}

void test_parse_raw_bytes_rejects_out_of_range_elements_atomically() {
    assertRejectedWithoutMutation("{\"bytes\":[1,2,3,4,5,-1]}");
    assertRejectedWithoutMutation("{\"bytes\":[1,2,3,4,5,256]}");
    assertRejectedWithoutMutation("{\"bytes\":[1,2,3,4,5,999999]}");
}

void test_parse_raw_bytes_rejects_wrong_container_and_size_atomically() {
    assertRejectedWithoutMutation("{\"bytes\":null}");
    assertRejectedWithoutMutation("{\"bytes\":\"1,2,3,4,5,6\"}");
    assertRejectedWithoutMutation("{\"bytes\":[1,2,3,4,5]}");
    assertRejectedWithoutMutation("{\"bytes\":[1,2,3,4,5,6,7]}");
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_parse_raw_bytes_accepts_integer_boundaries);
    RUN_TEST(test_parse_raw_bytes_rejects_non_integer_elements_atomically);
    RUN_TEST(test_parse_raw_bytes_rejects_out_of_range_elements_atomically);
    RUN_TEST(test_parse_raw_bytes_rejects_wrong_container_and_size_atomically);
    return UNITY_END();
}

/**
 * Device NVS / Settings Persistence Tests
 *
 * Validates real NVS (Non-Volatile Storage) operations on ESP32-S3:
 *   - Preferences write/read round-trip
 *   - Namespace A/B toggle pattern (matches settings.cpp)
 *   - String, integer, and blob storage
 *   - NVS quota / size limits
 *   - Namespace clear and re-populate
 *   - XOR obfuscation round-trip (WiFi password pattern)
 *
 * NVS issues cause silent data loss or boot loops. These tests exercise
 * the real flash wear-leveling layer that native mocks bypass.
 */

#include <unity.h>
#include <Arduino.h>
#include <Preferences.h>
#include <nvs_flash.h>
#include "../device_test_reset.h"

// Use a test-only namespace to avoid corrupting real settings
static constexpr const char* TEST_NS_A = "v1test_a";
static constexpr const char* TEST_NS_B = "v1test_b";

static Preferences prefs;

void setUp() {
    // Clean test namespaces before each test
    prefs.begin(TEST_NS_A, false);
    prefs.clear();
    prefs.end();
    prefs.begin(TEST_NS_B, false);
    prefs.clear();
    prefs.end();
}

void tearDown() {
    // Clean up after each test
    prefs.begin(TEST_NS_A, false);
    prefs.clear();
    prefs.end();
    prefs.begin(TEST_NS_B, false);
    prefs.clear();
    prefs.end();
}

// ===========================================================================
// BASIC READ/WRITE ROUND-TRIP
// ===========================================================================

void test_nvs_uint8_roundtrip() {
    prefs.begin(TEST_NS_A, false);
    prefs.putUChar("brightness", 200);
    prefs.end();

    prefs.begin(TEST_NS_A, true);  // Read-only
    uint8_t val = prefs.getUChar("brightness", 0);
    prefs.end();

    TEST_ASSERT_EQUAL_UINT8(200, val);
}

void test_nvs_uint16_roundtrip() {
    prefs.begin(TEST_NS_A, false);
    prefs.putUShort("freq", 34567);
    prefs.end();

    prefs.begin(TEST_NS_A, true);
    uint16_t val = prefs.getUShort("freq", 0);
    prefs.end();

    TEST_ASSERT_EQUAL_UINT16(34567, val);
}

void test_nvs_uint32_roundtrip() {
    prefs.begin(TEST_NS_A, false);
    prefs.putULong("timestamp", 1708300000UL);
    prefs.end();

    prefs.begin(TEST_NS_A, true);
    uint32_t val = prefs.getULong("timestamp", 0);
    prefs.end();

    TEST_ASSERT_EQUAL_UINT32(1708300000UL, val);
}

void test_nvs_int8_negative() {
    prefs.begin(TEST_NS_A, false);
    prefs.putChar("offset", -42);
    prefs.end();

    prefs.begin(TEST_NS_A, true);
    int8_t val = prefs.getChar("offset", 0);
    prefs.end();

    TEST_ASSERT_EQUAL_INT8(-42, val);
}

void test_nvs_bool_roundtrip() {
    prefs.begin(TEST_NS_A, false);
    prefs.putBool("enabled", true);
    prefs.putBool("disabled", false);
    prefs.end();

    prefs.begin(TEST_NS_A, true);
    TEST_ASSERT_TRUE(prefs.getBool("enabled", false));
    TEST_ASSERT_FALSE(prefs.getBool("disabled", true));
    prefs.end();
}

// ===========================================================================
// STRING STORAGE
// ===========================================================================

void test_nvs_string_roundtrip() {
    prefs.begin(TEST_NS_A, false);
    prefs.putString("ssid", "MyNetwork_5G");
    prefs.end();

    prefs.begin(TEST_NS_A, true);
    String val = prefs.getString("ssid", "");
    prefs.end();

    TEST_ASSERT_EQUAL_STRING("MyNetwork_5G", val.c_str());
}

void test_nvs_string_empty() {
    prefs.begin(TEST_NS_A, false);
    prefs.putString("empty", "");
    prefs.end();

    prefs.begin(TEST_NS_A, true);
    String val = prefs.getString("empty", "default");
    prefs.end();

    TEST_ASSERT_EQUAL_STRING("", val.c_str());
}

void test_nvs_string_max_length() {
    // NVS string max is ~4000 bytes. WiFi passwords are 63 chars max.
    char longStr[128];
    memset(longStr, 'A', 127);
    longStr[127] = '\0';

    prefs.begin(TEST_NS_A, false);
    size_t written = prefs.putString("long", longStr);
    prefs.end();

    TEST_ASSERT_GREATER_THAN(0, written);

    prefs.begin(TEST_NS_A, true);
    String val = prefs.getString("long", "");
    prefs.end();

    TEST_ASSERT_EQUAL_STRING(longStr, val.c_str());
}

// ===========================================================================
// NAMESPACE A/B TOGGLE (matches settings.cpp pattern)
// ===========================================================================

void test_nvs_namespace_ab_toggle() {
    // Write to namespace A
    prefs.begin(TEST_NS_A, false);
    prefs.putUChar("volume", 5);
    prefs.putBool("muted", false);
    prefs.end();

    // Write different values to namespace B (simulating save to alternate)
    prefs.begin(TEST_NS_B, false);
    prefs.putUChar("volume", 7);
    prefs.putBool("muted", true);
    prefs.end();

    // Read back from A
    prefs.begin(TEST_NS_A, true);
    TEST_ASSERT_EQUAL_UINT8(5, prefs.getUChar("volume", 0));
    TEST_ASSERT_FALSE(prefs.getBool("muted", true));
    prefs.end();

    // Read back from B
    prefs.begin(TEST_NS_B, true);
    TEST_ASSERT_EQUAL_UINT8(7, prefs.getUChar("volume", 0));
    TEST_ASSERT_TRUE(prefs.getBool("muted", false));
    prefs.end();
}

void test_nvs_namespace_clear_and_repopulate() {
    // Write to A
    prefs.begin(TEST_NS_A, false);
    prefs.putUChar("slot", 2);
    prefs.end();

    // Clear A
    prefs.begin(TEST_NS_A, false);
    prefs.clear();
    prefs.end();

    // Read from cleared A — should get default
    prefs.begin(TEST_NS_A, true);
    uint8_t val = prefs.getUChar("slot", 99);
    prefs.end();
    TEST_ASSERT_EQUAL_UINT8(99, val);

    // Re-populate A
    prefs.begin(TEST_NS_A, false);
    prefs.putUChar("slot", 1);
    prefs.end();

    prefs.begin(TEST_NS_A, true);
    val = prefs.getUChar("slot", 99);
    prefs.end();
    TEST_ASSERT_EQUAL_UINT8(1, val);
}

// ===========================================================================
// XOR OBFUSCATION ROUND-TRIP (WiFi password pattern from settings.cpp)
// ===========================================================================

static const uint8_t XOR_KEY[] = {0x3A, 0x7B, 0x1D, 0xF2, 0x9E, 0x4C, 0x8A, 0x65};
static const size_t XOR_KEY_LEN = sizeof(XOR_KEY);

static void xorObfuscate(char* data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        data[i] ^= XOR_KEY[i % XOR_KEY_LEN];
    }
}

void test_nvs_xor_password_roundtrip() {
    const char* password = "MySecurePass!23";
    char buf[64] = {};
    strncpy(buf, password, sizeof(buf) - 1);

    // Obfuscate
    xorObfuscate(buf, strlen(buf));

    // Store obfuscated bytes in NVS
    prefs.begin(TEST_NS_A, false);
    prefs.putBytes("wifipass", buf, strlen(password));
    prefs.end();

    // Read back
    char readBuf[64] = {};
    prefs.begin(TEST_NS_A, true);
    size_t readLen = prefs.getBytes("wifipass", readBuf, sizeof(readBuf));
    prefs.end();

    TEST_ASSERT_EQUAL(strlen(password), readLen);

    // De-obfuscate
    xorObfuscate(readBuf, readLen);

    TEST_ASSERT_EQUAL_STRING(password, readBuf);
}

// ===========================================================================
// DEFAULT VALUES FOR MISSING KEYS
// ===========================================================================

void test_nvs_missing_key_returns_default() {
    prefs.begin(TEST_NS_A, true);

    TEST_ASSERT_EQUAL_UINT8(42, prefs.getUChar("nonexistent", 42));
    TEST_ASSERT_EQUAL_UINT16(1000, prefs.getUShort("nope", 1000));
    TEST_ASSERT_TRUE(prefs.getBool("missing", true));
    TEST_ASSERT_EQUAL_STRING("fallback", prefs.getString("gone", "fallback").c_str());

    prefs.end();
}

// ===========================================================================
// BLOB (BYTES) STORAGE
// ===========================================================================

void test_nvs_blob_roundtrip() {
    uint8_t data[32];
    for (int i = 0; i < 32; i++) data[i] = (uint8_t)(i * 3);

    prefs.begin(TEST_NS_A, false);
    size_t written = prefs.putBytes("blob", data, 32);
    prefs.end();

    TEST_ASSERT_EQUAL(32, written);

    uint8_t readData[32] = {};
    prefs.begin(TEST_NS_A, true);
    size_t readLen = prefs.getBytes("blob", readData, 32);
    prefs.end();

    TEST_ASSERT_EQUAL(32, readLen);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(data, readData, 32);
}

// ===========================================================================
// OVERWRITE EXISTING KEY
// ===========================================================================

void test_nvs_overwrite_replaces_value() {
    prefs.begin(TEST_NS_A, false);
    prefs.putUChar("val", 10);
    prefs.putUChar("val", 20);
    prefs.end();

    prefs.begin(TEST_NS_A, true);
    TEST_ASSERT_EQUAL_UINT8(20, prefs.getUChar("val", 0));
    prefs.end();
}

// ===========================================================================
// MULTI-KEY STRESS
// ===========================================================================

void test_nvs_many_keys_no_corruption() {
    prefs.begin(TEST_NS_A, false);
    for (int i = 0; i < 30; i++) {
        char key[16];
        snprintf(key, sizeof(key), "k%d", i);
        prefs.putUShort(key, (uint16_t)(i * 100));
    }
    prefs.end();

    prefs.begin(TEST_NS_A, true);
    for (int i = 0; i < 30; i++) {
        char key[16];
        snprintf(key, sizeof(key), "k%d", i);
        uint16_t val = prefs.getUShort(key, 0xFFFF);
        TEST_ASSERT_EQUAL_UINT16((uint16_t)(i * 100), val);
    }
    prefs.end();
}

// ===========================================================================
// NVS STATS / FREE SPACE
// ===========================================================================

void test_nvs_has_free_entries() {
    nvs_stats_t stats;
    esp_err_t err = nvs_get_stats("nvs", &stats);
    if (err == ESP_OK) {
        Serial.printf("  [nvs] total=%lu used=%lu free=%lu ns_count=%lu\n",
                      (unsigned long)stats.total_entries,
                      (unsigned long)stats.used_entries,
                      (unsigned long)stats.free_entries,
                      (unsigned long)stats.namespace_count);
        // Should have at least 100 free entries for settings
        TEST_ASSERT_GREATER_THAN_UINT32(100, stats.free_entries);
    } else {
        TEST_IGNORE_MESSAGE("nvs_get_stats not available on this IDF version");
    }
}

// ===========================================================================
// TEST RUNNER
// ===========================================================================

void setup() {
    if (deviceTestSetup("test_device_nvs")) return;
    UNITY_BEGIN();

    // Basic round-trip
    RUN_TEST(test_nvs_uint8_roundtrip);
    RUN_TEST(test_nvs_uint16_roundtrip);
    RUN_TEST(test_nvs_uint32_roundtrip);
    RUN_TEST(test_nvs_int8_negative);
    RUN_TEST(test_nvs_bool_roundtrip);

    // String storage
    RUN_TEST(test_nvs_string_roundtrip);
    RUN_TEST(test_nvs_string_empty);
    RUN_TEST(test_nvs_string_max_length);

    // Namespace A/B toggle
    RUN_TEST(test_nvs_namespace_ab_toggle);
    RUN_TEST(test_nvs_namespace_clear_and_repopulate);

    // XOR obfuscation
    RUN_TEST(test_nvs_xor_password_roundtrip);

    // Default values
    RUN_TEST(test_nvs_missing_key_returns_default);

    // Blob storage
    RUN_TEST(test_nvs_blob_roundtrip);

    // Overwrite
    RUN_TEST(test_nvs_overwrite_replaces_value);

    // Stress
    RUN_TEST(test_nvs_many_keys_no_corruption);

    // NVS stats
    RUN_TEST(test_nvs_has_free_entries);

    UNITY_END();
    deviceTestFinish();
}

void loop() {
    delay(100);  // Keep USB CDC alive after post-test reboot
}

/**
 * WiFi Manager Unit Tests
 * 
 * Tests WiFi state enums, encryption detection, and connection states.
 * These tests catch bugs where:
 * - State enum values don't match expected behavior
 * - Open network detection fails
 * - UI activity timeout calculations are wrong
 */

#include <unity.h>
#ifdef ARDUINO
#include <Arduino.h>
#endif
#include <cstdint>
#include <cstring>

#include "../../src/wifi_manager.h"
#include "../../include/wifi_manager_internals.h"

// ============================================================================
// TEST-LOCAL HELPERS AROUND PRODUCTION TYPES
// ============================================================================

/**
 * Convert raw setup-mode values to string without first loading them into an
 * enum. UBSan correctly rejects invalid enum values, so invalid-input coverage
 * stays on the raw helper.
 */
const char* setupModeToStringRaw(int state) {
    switch (state) {
        case SETUP_MODE_OFF: return "OFF";
        case SETUP_MODE_AP_ON: return "AP_ON";
        case SETUP_MODE_STOPPING: return "STOPPING";
        default: return "UNKNOWN";
    }
}

/**
 * Convert SetupModeState to string.
 */
const char* setupModeToString(SetupModeState state) {
    return setupModeToStringRaw(static_cast<int>(state));
}

/**
 * Convert raw WiFi auth-mode values to human-readable strings without first
 * loading them into an enum.
 */
const char* authModeToStringRaw(int mode) {
    switch (mode) {
        case WIFI_AUTH_OPEN: return "Open";
        case WIFI_AUTH_WEP: return "WEP";
        case WIFI_AUTH_WPA_PSK: return "WPA";
        case WIFI_AUTH_WPA2_PSK: return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
        case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-Enterprise";
        case WIFI_AUTH_WPA3_PSK: return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
        default: return "Unknown";
    }
}

/**
 * Determine if encryption requires password
 */
bool requiresPassword(uint8_t encryptionType) {
    return encryptionType != WIFI_AUTH_OPEN;
}

/**
 * Get signal quality description from RSSI
 */
const char* rssiToQuality(int rssi) {
    if (rssi >= -50) return "Excellent";
    if (rssi >= -60) return "Good";
    if (rssi >= -70) return "Fair";
    if (rssi >= -80) return "Weak";
    return "Poor";
}

// ============================================================================
// SETUP MODE STATE TESTS
// ============================================================================

void test_setup_mode_enum_values() {
    TEST_ASSERT_EQUAL_INT(0, SETUP_MODE_OFF);
    TEST_ASSERT_EQUAL_INT(1, SETUP_MODE_AP_ON);
    TEST_ASSERT_EQUAL_INT(2, SETUP_MODE_STOPPING);
}

void test_setup_mode_strings() {
    TEST_ASSERT_EQUAL_STRING("OFF", setupModeToString(SETUP_MODE_OFF));
    TEST_ASSERT_EQUAL_STRING("AP_ON", setupModeToString(SETUP_MODE_AP_ON));
    TEST_ASSERT_EQUAL_STRING("STOPPING", setupModeToString(SETUP_MODE_STOPPING));
}

void test_setup_mode_unknown_string() {
    TEST_ASSERT_EQUAL_STRING("UNKNOWN", setupModeToStringRaw(99));
}

// ============================================================================
// WIFI CLIENT STATE TESTS
// ============================================================================

void test_wifi_client_state_enum_values() {
    TEST_ASSERT_EQUAL_INT(0, WIFI_CLIENT_DISABLED);
    TEST_ASSERT_EQUAL_INT(1, WIFI_CLIENT_DISCONNECTED);
    TEST_ASSERT_EQUAL_INT(2, WIFI_CLIENT_CONNECTING);
    TEST_ASSERT_EQUAL_INT(3, WIFI_CLIENT_CONNECTED);
    TEST_ASSERT_EQUAL_INT(4, WIFI_CLIENT_FAILED);
}

void test_wifi_client_state_strings() {
    TEST_ASSERT_EQUAL_STRING("disabled", wifiClientStateApiName(WIFI_CLIENT_DISABLED));
    TEST_ASSERT_EQUAL_STRING("disconnected", wifiClientStateApiName(WIFI_CLIENT_DISCONNECTED));
    TEST_ASSERT_EQUAL_STRING("connecting", wifiClientStateApiName(WIFI_CLIENT_CONNECTING));
    TEST_ASSERT_EQUAL_STRING("connected", wifiClientStateApiName(WIFI_CLIENT_CONNECTED));
    TEST_ASSERT_EQUAL_STRING("failed", wifiClientStateApiName(WIFI_CLIENT_FAILED));
}


// ============================================================================
// NETWORK OPEN DETECTION TESTS
// ============================================================================

void test_network_open_auth_open() {
    ScannedNetwork network;
    network.encryptionType = WIFI_AUTH_OPEN;
    TEST_ASSERT_TRUE(network.isOpen());
}

void test_network_not_open_wep() {
    ScannedNetwork network;
    network.encryptionType = WIFI_AUTH_WEP;
    TEST_ASSERT_FALSE(network.isOpen());
}

void test_network_not_open_wpa() {
    ScannedNetwork network;
    network.encryptionType = WIFI_AUTH_WPA_PSK;
    TEST_ASSERT_FALSE(network.isOpen());
}

void test_network_not_open_wpa2() {
    ScannedNetwork network;
    network.encryptionType = WIFI_AUTH_WPA2_PSK;
    TEST_ASSERT_FALSE(network.isOpen());
}

void test_network_not_open_wpa3() {
    ScannedNetwork network;
    network.encryptionType = WIFI_AUTH_WPA3_PSK;
    TEST_ASSERT_FALSE(network.isOpen());
}

void test_network_not_open_enterprise() {
    ScannedNetwork network;
    network.encryptionType = WIFI_AUTH_WPA2_ENTERPRISE;
    TEST_ASSERT_FALSE(network.isOpen());
}

// ============================================================================
// AUTH MODE STRING TESTS
// ============================================================================

void test_auth_mode_strings() {
    TEST_ASSERT_EQUAL_STRING("Open", authModeToStringRaw(WIFI_AUTH_OPEN));
    TEST_ASSERT_EQUAL_STRING("WEP", authModeToStringRaw(WIFI_AUTH_WEP));
    TEST_ASSERT_EQUAL_STRING("WPA", authModeToStringRaw(WIFI_AUTH_WPA_PSK));
    TEST_ASSERT_EQUAL_STRING("WPA2", authModeToStringRaw(WIFI_AUTH_WPA2_PSK));
    TEST_ASSERT_EQUAL_STRING("WPA/WPA2", authModeToStringRaw(WIFI_AUTH_WPA_WPA2_PSK));
    TEST_ASSERT_EQUAL_STRING("WPA2-Enterprise", authModeToStringRaw(WIFI_AUTH_WPA2_ENTERPRISE));
    TEST_ASSERT_EQUAL_STRING("WPA3", authModeToStringRaw(WIFI_AUTH_WPA3_PSK));
    TEST_ASSERT_EQUAL_STRING("WPA2/WPA3", authModeToStringRaw(WIFI_AUTH_WPA2_WPA3_PSK));
}

void test_auth_mode_unknown_string() {
    TEST_ASSERT_EQUAL_STRING("Unknown", authModeToStringRaw(99));
}

// ============================================================================
// REQUIRES PASSWORD TESTS
// ============================================================================

void test_open_no_password() {
    TEST_ASSERT_FALSE(requiresPassword(WIFI_AUTH_OPEN));
}

void test_wep_requires_password() {
    TEST_ASSERT_TRUE(requiresPassword(WIFI_AUTH_WEP));
}

void test_wpa2_requires_password() {
    TEST_ASSERT_TRUE(requiresPassword(WIFI_AUTH_WPA2_PSK));
}

void test_wpa3_requires_password() {
    TEST_ASSERT_TRUE(requiresPassword(WIFI_AUTH_WPA3_PSK));
}

// ============================================================================
// UI ACTIVITY TIMEOUT TESTS
// ============================================================================

void test_ui_active_recent_request() {
    // Request 5 seconds ago, timeout 30 seconds
    TEST_ASSERT_TRUE(wifiUiActiveSince(1000, 6000, 30000));
}

void test_ui_active_just_before_timeout() {
    // Request 29 seconds ago, timeout 30 seconds
    TEST_ASSERT_TRUE(wifiUiActiveSince(1000, 30000, 30000));
}

void test_ui_inactive_after_timeout() {
    // Request 31 seconds ago, timeout 30 seconds
    TEST_ASSERT_FALSE(wifiUiActiveSince(1000, 32000, 30000));
}

void test_ui_active_at_exact_timeout() {
    // Request exactly 30 seconds ago - should be inactive (< not <=)
    TEST_ASSERT_FALSE(wifiUiActiveSince(1, 30001, 30000));
}

void test_ui_active_zero_elapsed() {
    // Request just now (same time)
    TEST_ASSERT_TRUE(wifiUiActiveSince(5000, 5000, 30000));
}

void test_ui_inactive_when_no_activity_recorded() {
    TEST_ASSERT_FALSE(wifiUiActiveSince(0, 1000, 30000));
}

void test_ui_active_millis_wraparound() {
    // millis() wrapped around - should assume active
    TEST_ASSERT_TRUE(wifiUiActiveSince(0xFFFFFFF0, 100, 30000));  // nowMs < lastRequestMs
}

void test_ui_active_custom_timeout() {
    // Custom 60 second timeout
    TEST_ASSERT_TRUE(wifiUiActiveSince(1000, 60000, 60000));
    TEST_ASSERT_FALSE(wifiUiActiveSince(1000, 62000, 60000));
}

// ============================================================================
// RSSI QUALITY TESTS
// ============================================================================

void test_rssi_excellent() {
    TEST_ASSERT_EQUAL_STRING("Excellent", rssiToQuality(-40));
    TEST_ASSERT_EQUAL_STRING("Excellent", rssiToQuality(-50));
}

void test_rssi_good() {
    TEST_ASSERT_EQUAL_STRING("Good", rssiToQuality(-51));
    TEST_ASSERT_EQUAL_STRING("Good", rssiToQuality(-60));
}

void test_rssi_fair() {
    TEST_ASSERT_EQUAL_STRING("Fair", rssiToQuality(-61));
    TEST_ASSERT_EQUAL_STRING("Fair", rssiToQuality(-70));
}

void test_rssi_weak() {
    TEST_ASSERT_EQUAL_STRING("Weak", rssiToQuality(-71));
    TEST_ASSERT_EQUAL_STRING("Weak", rssiToQuality(-80));
}

void test_rssi_poor() {
    TEST_ASSERT_EQUAL_STRING("Poor", rssiToQuality(-81));
    TEST_ASSERT_EQUAL_STRING("Poor", rssiToQuality(-100));
}

// ============================================================================
// ENUM ORDERING TESTS
// ============================================================================

void test_setup_mode_boolean_logic() {
    // SETUP_MODE_OFF should be falsy (0)
    TEST_ASSERT_FALSE(SETUP_MODE_OFF);
    // SETUP_MODE_AP_ON should be truthy (1)
    TEST_ASSERT_TRUE(SETUP_MODE_AP_ON);
}

void test_wifi_client_disabled_is_zero() {
    // DISABLED should be 0 for easy boolean checks
    TEST_ASSERT_EQUAL_INT(0, WIFI_CLIENT_DISABLED);
}

void test_wifi_client_connected_is_distinct() {
    // CONNECTED should be different from DISABLED for state checks
    TEST_ASSERT_NOT_EQUAL(WIFI_CLIENT_DISABLED, WIFI_CLIENT_CONNECTED);
}

// ============================================================================
// TEST RUNNER
// ============================================================================

void setUp(void) {}
void tearDown(void) {}

void runAllTests() {
    // Setup mode state tests
    RUN_TEST(test_setup_mode_enum_values);
    RUN_TEST(test_setup_mode_strings);
    RUN_TEST(test_setup_mode_unknown_string);
    
    // WiFi client state tests
    RUN_TEST(test_wifi_client_state_enum_values);
    RUN_TEST(test_wifi_client_state_strings);
    
    // Network open detection tests
    RUN_TEST(test_network_open_auth_open);
    RUN_TEST(test_network_not_open_wep);
    RUN_TEST(test_network_not_open_wpa);
    RUN_TEST(test_network_not_open_wpa2);
    RUN_TEST(test_network_not_open_wpa3);
    RUN_TEST(test_network_not_open_enterprise);
    
    // Auth mode string tests
    RUN_TEST(test_auth_mode_strings);
    RUN_TEST(test_auth_mode_unknown_string);
    
    // Requires password tests
    RUN_TEST(test_open_no_password);
    RUN_TEST(test_wep_requires_password);
    RUN_TEST(test_wpa2_requires_password);
    RUN_TEST(test_wpa3_requires_password);
    
    // UI activity timeout tests
    RUN_TEST(test_ui_active_recent_request);
    RUN_TEST(test_ui_active_just_before_timeout);
    RUN_TEST(test_ui_inactive_after_timeout);
    RUN_TEST(test_ui_active_at_exact_timeout);
    RUN_TEST(test_ui_active_zero_elapsed);
    RUN_TEST(test_ui_inactive_when_no_activity_recorded);
    RUN_TEST(test_ui_active_millis_wraparound);
    RUN_TEST(test_ui_active_custom_timeout);
    
    // RSSI quality tests
    RUN_TEST(test_rssi_excellent);
    RUN_TEST(test_rssi_good);
    RUN_TEST(test_rssi_fair);
    RUN_TEST(test_rssi_weak);
    RUN_TEST(test_rssi_poor);
    
    // Enum ordering tests
    RUN_TEST(test_setup_mode_boolean_logic);
    RUN_TEST(test_wifi_client_disabled_is_zero);
    RUN_TEST(test_wifi_client_connected_is_distinct);
}

#ifdef ARDUINO
void setup() {
    delay(2000);
    UNITY_BEGIN();
    runAllTests();
    UNITY_END();
}
void loop() {}
#else
int main(int argc, char **argv) {
    UNITY_BEGIN();
    runAllTests();
    return UNITY_END();
}
#endif

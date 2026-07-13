#include <unity.h>
#include <cstring>

#include "../../src/modules/wifi/wifi_settings_api_service.h"
#include "../../src/modules/wifi/wifi_settings_api_service.cpp"  // Pull implementation for UNIT_TEST.

#ifndef ARDUINO
SerialClass Serial;
#endif

unsigned long mockMillis = 0;
unsigned long mockMicros = 0;

static bool responseContains(const WebServer& server, const char* needle) {
    return std::strstr(server.lastBody.c_str(), needle) != nullptr;
}

struct FakeRuntime {
    V1Settings settings;

    int applySettingsUpdateCalls = 0;
    String lastApSsid;
    String lastApPassword;

    int saveDeferredBackupCalls = 0;
};

static void applyDeviceSettingsUpdateForTest(FakeRuntime& rt, const DeviceSettingsUpdate& update) {
    rt.applySettingsUpdateCalls++;
    rt.saveDeferredBackupCalls++;
    if (update.hasApCredentials) {
        rt.lastApSsid = update.apSSID;
        rt.lastApPassword = update.apPassword;
        rt.settings.apSSID = update.apSSID;
        rt.settings.apPassword = update.apPassword;
    }
    if (update.hasProxyBLE) rt.settings.proxyBLE = update.proxyBLE;
    if (update.hasProxyName) rt.settings.proxyName = sanitizeProxyNameValue(update.proxyName);
    if (update.hasAutoPowerOffMinutes) rt.settings.autoPowerOffMinutes = update.autoPowerOffMinutes;
    if (update.hasApTimeoutMinutes) rt.settings.apTimeoutMinutes = update.apTimeoutMinutes;
    if (update.hasAlpEnabled) rt.settings.alpEnabled = update.alpEnabled;
    if (update.hasAlpSdLogEnabled) rt.settings.alpSdLogEnabled = update.alpSdLogEnabled;
    if (update.hasAlpAlertPersistSec) rt.settings.alpAlertPersistSec = update.alpAlertPersistSec;
    if (update.hasAlpDisableV1LaserOnPush) {
        rt.settings.alpDisableV1LaserOnPush = update.alpDisableV1LaserOnPush;
    }
}

static WifiSettingsApiService::Runtime makeRuntime(FakeRuntime& rt) {
    WifiSettingsApiService::Runtime r;
    r.ctx = &rt;
    r.getSettings = [](void* ctx) -> const V1Settings& {
        return static_cast<FakeRuntime*>(ctx)->settings;
    };
    r.applySettingsUpdate = [](const DeviceSettingsUpdate& update, void* ctx) {
        applyDeviceSettingsUpdateForTest(*static_cast<FakeRuntime*>(ctx), update);
    };
    r.checkRateLimit = [](void* /*ctx*/) { return true; };
    return r;
}

void setUp() {
    mockMillis = 1000;
    mockMicros = 1000000;
}

void tearDown() {}

void test_device_settings_get_serializes_expected_payload() {
    WebServer server(80);
    FakeRuntime rt;
    rt.settings.apSSID = "V1-Test";
    rt.settings.apPassword = "custom-pass";
    rt.settings.proxyBLE = false;
    rt.settings.proxyName = "Proxy-Test";
    rt.settings.autoPowerOffMinutes = 12;
    rt.settings.apTimeoutMinutes = 25;
    rt.settings.alpEnabled = true;
    rt.settings.alpSdLogEnabled = true;
    rt.settings.alpAlertPersistSec = 3;
    rt.settings.alpDisableV1LaserOnPush = false;

    WifiSettingsApiService::handleApiDeviceSettingsGet(server, makeRuntime(rt));

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"ap_ssid\":\"V1-Test\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"ap_password\":\"********\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"isDefaultPassword\":false"));
    TEST_ASSERT_TRUE(responseContains(server, "\"proxy_ble\":false"));
    TEST_ASSERT_TRUE(responseContains(server, "\"proxy_name\":\"Proxy-Test\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"autoPowerOffMinutes\":12"));
    TEST_ASSERT_TRUE(responseContains(server, "\"apTimeoutMinutes\":25"));
    TEST_ASSERT_TRUE(responseContains(server, "\"alpEnabled\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"alpSdLogEnabled\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"alpAlertPersistSec\":3"));
    TEST_ASSERT_TRUE(responseContains(server, "\"alpDisableV1LaserOnPush\":false"));
}

void test_device_settings_save_rejects_invalid_ap_credentials() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("ap_ssid", "MyAP");
    server.setArg("ap_password", "short");

    WifiSettingsApiService::handleApiDeviceSettingsSave(server, makeRuntime(rt));

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "AP SSID required and password must be at least 8 characters"));
    TEST_ASSERT_EQUAL_INT(0, rt.applySettingsUpdateCalls);
    TEST_ASSERT_EQUAL_INT(0, rt.saveDeferredBackupCalls);
}

void test_device_settings_save_uses_existing_password_placeholder() {
    WebServer server(80);
    FakeRuntime rt;
    rt.settings.apPassword = "existing123";
    server.setArg("ap_ssid", "RenamedAP");
    server.setArg("ap_password", "********");

    WifiSettingsApiService::handleApiDeviceSettingsSave(server, makeRuntime(rt));

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(1, rt.applySettingsUpdateCalls);
    TEST_ASSERT_EQUAL_STRING("RenamedAP", rt.lastApSsid.c_str());
    TEST_ASSERT_EQUAL_STRING("existing123", rt.lastApPassword.c_str());
    TEST_ASSERT_EQUAL_INT(1, rt.saveDeferredBackupCalls);
}

void test_device_settings_save_updates_device_toggles() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("proxy_ble", "0");
    server.setArg("proxy_name", "Garage Unit");
    server.setArg("autoPowerOffMinutes", "19");
    server.setArg("apTimeoutMinutes", "14");
    server.setArg("alpEnabled", "true");
    server.setArg("alpSdLogEnabled", "true");
    server.setArg("alpAlertPersistSec", "4");
    server.setArg("alpDisableV1LaserOnPush", "0");

    WifiSettingsApiService::handleApiDeviceSettingsSave(server, makeRuntime(rt));

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_FALSE(rt.settings.proxyBLE);
    TEST_ASSERT_EQUAL_STRING("Garage Unit", rt.settings.proxyName.c_str());
    TEST_ASSERT_EQUAL_UINT8(19, rt.settings.autoPowerOffMinutes);
    TEST_ASSERT_EQUAL_UINT8(14, rt.settings.apTimeoutMinutes);
    TEST_ASSERT_TRUE(rt.settings.alpEnabled);
    TEST_ASSERT_TRUE(rt.settings.alpSdLogEnabled);
    TEST_ASSERT_EQUAL_UINT8(4, rt.settings.alpAlertPersistSec);
    TEST_ASSERT_FALSE(rt.settings.alpDisableV1LaserOnPush);
    TEST_ASSERT_EQUAL_INT(1, rt.saveDeferredBackupCalls);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_device_settings_get_serializes_expected_payload);
    RUN_TEST(test_device_settings_save_rejects_invalid_ap_credentials);
    RUN_TEST(test_device_settings_save_uses_existing_password_placeholder);
    RUN_TEST(test_device_settings_save_updates_device_toggles);
    return UNITY_END();
}

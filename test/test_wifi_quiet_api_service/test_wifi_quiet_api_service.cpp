#include <cstring>

#include <unity.h>

#include "../../src/modules/wifi/wifi_quiet_api_service.cpp"

#ifndef ARDUINO
SerialClass Serial;
#endif

namespace {

V1Settings settings;
QuietSettingsUpdate capturedUpdate;
int applyCalls = 0;
int rateLimitCalls = 0;
bool allowRequest = true;

const V1Settings& getSettings(void*) {
    return settings;
}

void applySettingsUpdate(const QuietSettingsUpdate& update, void*) {
    capturedUpdate = update;
    ++applyCalls;
}

bool checkRateLimit(void*) {
    ++rateLimitCalls;
    return allowRequest;
}

WifiQuietApiService::Runtime makeRuntime() {
    WifiQuietApiService::Runtime runtime;
    runtime.getSettings = getSettings;
    runtime.applySettingsUpdate = applySettingsUpdate;
    runtime.checkRateLimit = checkRateLimit;
    return runtime;
}

bool responseContains(const WebServer& server, const char* needle) {
    return std::strstr(server.lastBody.c_str(), needle) != nullptr;
}

} // namespace

void setUp() {
    settings = V1Settings();
    capturedUpdate = QuietSettingsUpdate();
    applyCalls = 0;
    rateLimitCalls = 0;
    allowRequest = true;
}

void tearDown() {}

void test_get_reports_quiet_settings() {
    settings.alertVolumeFadeEnabled = true;
    settings.alertVolumeFadeDelaySec = 7;
    settings.alertVolumeFadeVolume = 2;
    settings.speedMuteEnabled = true;
    settings.speedMuteThresholdMph = 31;
    settings.speedMuteHysteresisMph = 4;
    settings.speedMuteVolume = 1;
    settings.stealthEnabled = true;
    WebServer server(80);

    WifiQuietApiService::handleApiGet(server, makeRuntime());

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"alertVolumeFadeEnabled\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"alertVolumeFadeDelaySec\":7"));
    TEST_ASSERT_TRUE(responseContains(server, "\"alertVolumeFadeVolume\":2"));
    TEST_ASSERT_TRUE(responseContains(server, "\"speedMuteEnabled\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"speedMuteThresholdMph\":31"));
    TEST_ASSERT_TRUE(responseContains(server, "\"speedMuteHysteresisMph\":4"));
    TEST_ASSERT_TRUE(responseContains(server, "\"speedMuteVolume\":1"));
    TEST_ASSERT_TRUE(responseContains(server, "\"stealthEnabled\":true"));
}

void test_save_parses_and_clamps_quiet_settings() {
    WebServer server(80);
    server.setArg("alertVolumeFadeEnabled", "true");
    server.setArg("alertVolumeFadeDelaySec", "0");
    server.setArg("alertVolumeFadeVolume", "99");
    server.setArg("speedMuteEnabled", "1");
    server.setArg("speedMuteThresholdMph", "100");
    server.setArg("speedMuteHysteresisMph", "0");
    server.setArg("speedMuteVolume", "12");
    server.setArg("stealthEnabled", "true");

    WifiQuietApiService::handleApiSave(server, makeRuntime());

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"success\":true"));
    TEST_ASSERT_EQUAL_INT(1, rateLimitCalls);
    TEST_ASSERT_EQUAL_INT(1, applyCalls);
    TEST_ASSERT_TRUE(capturedUpdate.hasAlertVolumeFadeEnabled);
    TEST_ASSERT_TRUE(capturedUpdate.alertVolumeFadeEnabled);
    TEST_ASSERT_TRUE(capturedUpdate.hasAlertVolumeFadeDelaySec);
    TEST_ASSERT_EQUAL_UINT8(1, capturedUpdate.alertVolumeFadeDelaySec);
    TEST_ASSERT_TRUE(capturedUpdate.hasAlertVolumeFadeVolume);
    TEST_ASSERT_EQUAL_UINT8(9, capturedUpdate.alertVolumeFadeVolume);
    TEST_ASSERT_TRUE(capturedUpdate.hasSpeedMuteEnabled);
    TEST_ASSERT_TRUE(capturedUpdate.speedMuteEnabled);
    TEST_ASSERT_TRUE(capturedUpdate.hasSpeedMuteThresholdMph);
    TEST_ASSERT_EQUAL_UINT8(60, capturedUpdate.speedMuteThresholdMph);
    TEST_ASSERT_TRUE(capturedUpdate.hasSpeedMuteHysteresisMph);
    TEST_ASSERT_EQUAL_UINT8(1, capturedUpdate.speedMuteHysteresisMph);
    TEST_ASSERT_TRUE(capturedUpdate.hasSpeedMuteVolume);
    TEST_ASSERT_EQUAL_UINT8(0, capturedUpdate.speedMuteVolume);
    TEST_ASSERT_TRUE(capturedUpdate.hasStealthEnabled);
    TEST_ASSERT_TRUE(capturedUpdate.stealthEnabled);
}

void test_save_stops_when_rate_limited() {
    allowRequest = false;
    WebServer server(80);
    server.setArg("stealthEnabled", "true");

    WifiQuietApiService::handleApiSave(server, makeRuntime());

    TEST_ASSERT_EQUAL_INT(1, rateLimitCalls);
    TEST_ASSERT_EQUAL_INT(0, applyCalls);
    TEST_ASSERT_EQUAL_INT(0, server.sendCount);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_get_reports_quiet_settings);
    RUN_TEST(test_save_parses_and_clamps_quiet_settings);
    RUN_TEST(test_save_stops_when_rate_limited);
    return UNITY_END();
}

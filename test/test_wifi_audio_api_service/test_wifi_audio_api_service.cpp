#include <unity.h>
#include <cstring>

#include "../../src/modules/wifi/wifi_audio_api_service.h"
#include "../../src/modules/wifi/wifi_audio_api_service.cpp"  // Pull implementation for UNIT_TEST.

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
    int setAudioVolumeCalls = 0;
    uint8_t lastAudioVolume = 0;
    int saveDeferredBackupCalls = 0;
    bool allowRequest = true;
};

static void applyAudioSettingsUpdateForTest(FakeRuntime& rt, const AudioSettingsUpdate& update) {
    rt.saveDeferredBackupCalls++;
    if (update.hasVoiceAlertMode) rt.settings.voiceAlertMode = update.voiceAlertMode;
    if (update.hasVoiceDirectionEnabled) rt.settings.voiceDirectionEnabled = update.voiceDirectionEnabled;
    if (update.hasAnnounceBogeyCount) rt.settings.announceBogeyCount = update.announceBogeyCount;
    if (update.hasMuteVoiceIfVolZero) rt.settings.muteVoiceIfVolZero = update.muteVoiceIfVolZero;
    if (update.hasVoiceVolume) rt.settings.voiceVolume = update.voiceVolume;
    if (update.hasAnnounceSecondaryAlerts) {
        rt.settings.announceSecondaryAlerts = update.announceSecondaryAlerts;
    }
    if (update.hasSecondaryLaser) rt.settings.secondaryLaser = update.secondaryLaser;
    if (update.hasSecondaryKa) rt.settings.secondaryKa = update.secondaryKa;
    if (update.hasSecondaryK) rt.settings.secondaryK = update.secondaryK;
    if (update.hasSecondaryX) rt.settings.secondaryX = update.secondaryX;
    if (update.hasAlertVolumeFadeEnabled) {
        rt.settings.alertVolumeFadeEnabled = update.alertVolumeFadeEnabled;
    }
    if (update.hasAlertVolumeFadeDelaySec) {
        rt.settings.alertVolumeFadeDelaySec = update.alertVolumeFadeDelaySec;
    }
    if (update.hasAlertVolumeFadeVolume) {
        rt.settings.alertVolumeFadeVolume = update.alertVolumeFadeVolume;
    }
    if (update.hasSpeedMuteEnabled) rt.settings.speedMuteEnabled = update.speedMuteEnabled;
    if (update.hasSpeedMuteThresholdMph) {
        rt.settings.speedMuteThresholdMph = update.speedMuteThresholdMph;
    }
    if (update.hasSpeedMuteHysteresisMph) {
        rt.settings.speedMuteHysteresisMph = update.speedMuteHysteresisMph;
    }
    if (update.hasSpeedMuteVolume) rt.settings.speedMuteVolume = update.speedMuteVolume;
    if (update.hasSpeedMuteVoice) rt.settings.speedMuteVoice = update.speedMuteVoice;
    if (update.hasStealthEnabled) rt.settings.stealthEnabled = update.stealthEnabled;
}

static WifiAudioApiService::Runtime makeRuntime(FakeRuntime& rt) {
    WifiAudioApiService::Runtime r;
    r.ctx = &rt;
    r.getSettings = [](void* ctx) -> const V1Settings& {
        return static_cast<FakeRuntime*>(ctx)->settings;
    };
    r.applySettingsUpdate = [](const AudioSettingsUpdate& update, void* ctx) {
        applyAudioSettingsUpdateForTest(*static_cast<FakeRuntime*>(ctx), update);
    };
    r.setAudioVolume = [](uint8_t volume, void* ctx) {
        auto* rt = static_cast<FakeRuntime*>(ctx);
        rt->setAudioVolumeCalls++;
        rt->lastAudioVolume = volume;
    };
    r.checkRateLimit = [](void* ctx) {
        return static_cast<FakeRuntime*>(ctx)->allowRequest;
    };
    return r;
}

void setUp() {
    mockMillis = 1000;
    mockMicros = 1000000;
}

void tearDown() {}

void test_get_returns_500_when_runtime_missing() {
    WebServer server(80);
    WifiAudioApiService::Runtime runtime{};

    WifiAudioApiService::handleApiGet(server, runtime);

    TEST_ASSERT_EQUAL_INT(500, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"Settings unavailable\""));
}

void test_get_serializes_audio_payload() {
    WebServer server(80);
    FakeRuntime rt;
    rt.settings.voiceAlertMode = VOICE_MODE_FREQ_ONLY;
    rt.settings.voiceDirectionEnabled = false;
    rt.settings.voiceVolume = 61;
    rt.settings.alertVolumeFadeEnabled = true;
    rt.settings.alertVolumeFadeDelaySec = 4;
    rt.settings.speedMuteEnabled = true;
    rt.settings.speedMuteThresholdMph = 33;
    rt.settings.speedMuteHysteresisMph = 5;
    rt.settings.speedMuteVolume = 2;
    rt.settings.speedMuteVoice = false;
    rt.settings.stealthEnabled = true;

    WifiAudioApiService::handleApiGet(server, makeRuntime(rt));

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"voiceAlertMode\":2"));
    TEST_ASSERT_TRUE(responseContains(server, "\"voiceDirectionEnabled\":false"));
    TEST_ASSERT_TRUE(responseContains(server, "\"voiceVolume\":61"));
    TEST_ASSERT_TRUE(responseContains(server, "\"alertVolumeFadeEnabled\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"speedMuteEnabled\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"speedMuteThresholdMph\":33"));
    TEST_ASSERT_TRUE(responseContains(server, "\"speedMuteHysteresisMph\":5"));
    TEST_ASSERT_TRUE(responseContains(server, "\"speedMuteVolume\":2"));
    TEST_ASSERT_TRUE(responseContains(server, "\"speedMuteVoice\":false"));
    TEST_ASSERT_TRUE(responseContains(server, "\"stealthEnabled\":true"));
}

void test_save_rate_limited_short_circuits() {
    WebServer server(80);
    FakeRuntime rt;
    rt.allowRequest = false;
    server.setArg("voiceVolume", "55");

    WifiAudioApiService::handleApiSave(server, makeRuntime(rt));

    TEST_ASSERT_EQUAL_INT(0, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(0, rt.setAudioVolumeCalls);
    TEST_ASSERT_EQUAL_INT(0, rt.saveDeferredBackupCalls);
}

void test_save_updates_audio_settings_and_calls_side_effects() {
    WebServer server(80);
    FakeRuntime rt;

    server.setArg("voiceAlertMode", "1");
    server.setArg("voiceDirectionEnabled", "false");
    server.setArg("announceBogeyCount", "false");
    server.setArg("muteVoiceIfVolZero", "true");
    server.setArg("voiceVolume", "71");
    server.setArg("announceSecondaryAlerts", "true");
    server.setArg("secondaryLaser", "false");
    server.setArg("secondaryKa", "false");
    server.setArg("secondaryK", "true");
    server.setArg("secondaryX", "true");
    server.setArg("alertVolumeFadeEnabled", "true");
    server.setArg("alertVolumeFadeDelaySec", "8");
    server.setArg("alertVolumeFadeVolume", "3");
    server.setArg("speedMuteEnabled", "true");
    server.setArg("speedMuteThresholdMph", "32");
    server.setArg("speedMuteHysteresisMph", "4");
    server.setArg("speedMuteVolume", "2");
    server.setArg("speedMuteVoice", "false");
    server.setArg("stealthEnabled", "true");

    WifiAudioApiService::handleApiSave(server, makeRuntime(rt));

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"success\":true"));
    TEST_ASSERT_EQUAL_INT(VOICE_MODE_BAND_ONLY, static_cast<int>(rt.settings.voiceAlertMode));
    TEST_ASSERT_FALSE(rt.settings.voiceDirectionEnabled);
    TEST_ASSERT_FALSE(rt.settings.announceBogeyCount);
    TEST_ASSERT_TRUE(rt.settings.muteVoiceIfVolZero);
    TEST_ASSERT_EQUAL_UINT8(71, rt.settings.voiceVolume);
    TEST_ASSERT_TRUE(rt.settings.announceSecondaryAlerts);
    TEST_ASSERT_FALSE(rt.settings.secondaryLaser);
    TEST_ASSERT_FALSE(rt.settings.secondaryKa);
    TEST_ASSERT_TRUE(rt.settings.secondaryK);
    TEST_ASSERT_TRUE(rt.settings.secondaryX);
    TEST_ASSERT_TRUE(rt.settings.alertVolumeFadeEnabled);
    TEST_ASSERT_EQUAL_UINT8(8, rt.settings.alertVolumeFadeDelaySec);
    TEST_ASSERT_EQUAL_UINT8(3, rt.settings.alertVolumeFadeVolume);
    TEST_ASSERT_TRUE(rt.settings.speedMuteEnabled);
    TEST_ASSERT_EQUAL_UINT8(32, rt.settings.speedMuteThresholdMph);
    TEST_ASSERT_EQUAL_UINT8(4, rt.settings.speedMuteHysteresisMph);
    TEST_ASSERT_EQUAL_UINT8(2, rt.settings.speedMuteVolume);
    TEST_ASSERT_FALSE(rt.settings.speedMuteVoice);
    TEST_ASSERT_TRUE(rt.settings.stealthEnabled);
    TEST_ASSERT_EQUAL_INT(1, rt.setAudioVolumeCalls);
    TEST_ASSERT_EQUAL_UINT8(71, rt.lastAudioVolume);
    TEST_ASSERT_EQUAL_INT(1, rt.saveDeferredBackupCalls);
}

void test_save_clamps_numeric_ranges() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("voiceAlertMode", "99");
    server.setArg("voiceVolume", "101");
    server.setArg("alertVolumeFadeDelaySec", "0");
    server.setArg("alertVolumeFadeVolume", "99");
    server.setArg("speedMuteThresholdMph", "99");
    server.setArg("speedMuteHysteresisMph", "0");
    server.setArg("speedMuteVolume", "99");

    WifiAudioApiService::handleApiSave(server, makeRuntime(rt));

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(3, static_cast<int>(rt.settings.voiceAlertMode));
    TEST_ASSERT_EQUAL_UINT8(100, rt.settings.voiceVolume);
    TEST_ASSERT_EQUAL_UINT8(1, rt.settings.alertVolumeFadeDelaySec);
    TEST_ASSERT_EQUAL_UINT8(9, rt.settings.alertVolumeFadeVolume);
    TEST_ASSERT_EQUAL_UINT8(60, rt.settings.speedMuteThresholdMph);
    TEST_ASSERT_EQUAL_UINT8(1, rt.settings.speedMuteHysteresisMph);
    TEST_ASSERT_EQUAL_UINT8(0, rt.settings.speedMuteVolume);
}

void test_save_clamps_fade_volume_zero_to_one() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("alertVolumeFadeVolume", "0");

    WifiAudioApiService::handleApiSave(server, makeRuntime(rt));

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_UINT8(1, rt.settings.alertVolumeFadeVolume);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_get_returns_500_when_runtime_missing);
    RUN_TEST(test_get_serializes_audio_payload);
    RUN_TEST(test_save_rate_limited_short_circuits);
    RUN_TEST(test_save_updates_audio_settings_and_calls_side_effects);
    RUN_TEST(test_save_clamps_numeric_ranges);
    RUN_TEST(test_save_clamps_fade_volume_zero_to_one);
    return UNITY_END();
}

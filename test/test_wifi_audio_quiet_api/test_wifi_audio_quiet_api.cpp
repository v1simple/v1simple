#include <unity.h>

#include "../mocks/Arduino.h"
#include "../mocks/WebServer.h"
#include "../mocks/mock_heap_caps_state.h"
#include "../../src/modules/wifi/wifi_audio_api_service.h"
#include "../../src/modules/wifi/wifi_quiet_api_service.h"
#include "../../src/modules/wifi/wifi_settings_api_service.h"
#include "../../src/modules/wifi/wifi_display_colors_api_service.h"
#include "../../src/modules/wifi/wifi_audio_api_service.cpp"
#include "../../src/modules/wifi/wifi_quiet_api_service.cpp"
#include "../../src/modules/wifi/wifi_settings_api_service.cpp"
#include "../../src/modules/wifi/wifi_display_colors_api_service.cpp"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

namespace {

struct Probe {
    V1Settings settings;
    AudioSettingsUpdate update;
    DeviceSettingsUpdate deviceUpdate;
    DisplaySettingsUpdate displayUpdate;
    int applyCalls = 0;
    int deviceApplyCalls = 0;
    int displayApplyCalls = 0;
    int resetCalls = 0;
    int volumeCalls = 0;
    int brightnessCalls = 0;
    int redrawCalls = 0;
    int previewCalls = 0;
    uint8_t volume = 0;
    bool persistSuccess = true;
};

WifiAudioSettingsRuntime makeRuntime(Probe& probe) {
    WifiAudioSettingsRuntime runtime;
    runtime.ctx = &probe;
    runtime.getSettings = [](void* ctx) -> const V1Settings& { return static_cast<Probe*>(ctx)->settings; };
    runtime.applySettingsUpdate = [](const AudioSettingsUpdate& update, void* ctx) {
        auto* state = static_cast<Probe*>(ctx);
        state->update = update;
        state->applyCalls++;
        return SettingsPersistResult{state->persistSuccess, true, false};
    };
    runtime.setAudioVolume = [](uint8_t volume, void* ctx) {
        auto* state = static_cast<Probe*>(ctx);
        state->volume = volume;
        state->volumeCalls++;
    };
    return runtime;
}

WifiSettingsApiService::Runtime makeDeviceRuntime(Probe& probe) {
    WifiSettingsApiService::Runtime runtime;
    runtime.ctx = &probe;
    runtime.getSettings = [](void* ctx) -> const V1Settings& { return static_cast<Probe*>(ctx)->settings; };
    runtime.applySettingsUpdate = [](const DeviceSettingsUpdate& update, void* ctx) {
        auto* state = static_cast<Probe*>(ctx);
        state->deviceUpdate = update;
        state->deviceApplyCalls++;
        return SettingsPersistResult{state->persistSuccess, true, false};
    };
    return runtime;
}

WifiDisplayColorsApiService::Runtime makeDisplayRuntime(Probe& probe) {
    WifiDisplayColorsApiService::Runtime runtime;
    runtime.getSettings = [](void* ctx) -> const V1Settings& { return static_cast<Probe*>(ctx)->settings; };
    runtime.getSettingsCtx = &probe;
    runtime.applySettingsUpdate = [](const DisplaySettingsUpdate& update, void* ctx) {
        auto* state = static_cast<Probe*>(ctx);
        state->displayUpdate = update;
        state->displayApplyCalls++;
        return SettingsPersistResult{state->persistSuccess, true, false};
    };
    runtime.applySettingsUpdateCtx = &probe;
    runtime.resetDisplaySettings = [](void* ctx) {
        auto* state = static_cast<Probe*>(ctx);
        state->resetCalls++;
        return SettingsPersistResult{state->persistSuccess, true, false};
    };
    runtime.resetDisplaySettingsCtx = &probe;
    runtime.setDisplayBrightness = [](uint8_t, void* ctx) { static_cast<Probe*>(ctx)->brightnessCalls++; };
    runtime.setDisplayBrightnessCtx = &probe;
    runtime.forceDisplayRedraw = [](void* ctx) { static_cast<Probe*>(ctx)->redrawCalls++; };
    runtime.forceDisplayRedrawCtx = &probe;
    runtime.requestColorPreviewHoldMs = [](uint32_t, void* ctx) { static_cast<Probe*>(ctx)->previewCalls++; };
    runtime.requestColorPreviewHoldMsCtx = &probe;
    return runtime;
}

bool contains(const String& body, const char* text) {
    return body.indexOf(text) >= 0;
}

} // namespace

void setUp() { mock_reset_heap_caps(); }
void tearDown() {}

void test_quiet_get_preserves_its_exact_field_set() {
    Probe probe;
    WebServer server(80);

    WifiQuietApiService::handleApiGet(server, makeRuntime(probe));

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_STRING(
        "{\"alertVolumeFadeEnabled\":false,\"alertVolumeFadeDelaySec\":2,\"alertVolumeFadeVolume\":1,"
        "\"speedMuteEnabled\":false,\"speedMuteThresholdMph\":25,\"speedMuteHysteresisMph\":3,"
        "\"speedMuteVolume\":0,\"stealthEnabled\":false}",
        server.lastBody.c_str());
    TEST_ASSERT_FALSE(contains(server.lastBody, "speedMuteVoice"));
    TEST_ASSERT_FALSE(contains(server.lastBody, "voiceVolume"));
}

void test_audio_get_preserves_quiet_superset_order_and_fields() {
    Probe probe;
    WebServer server(80);

    WifiAudioApiService::handleApiGet(server, makeRuntime(probe));

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(contains(server.lastBody,
                              "\"secondaryX\":false,\"alertVolumeFadeEnabled\":false,"
                              "\"alertVolumeFadeDelaySec\":2,\"alertVolumeFadeVolume\":1"));
    TEST_ASSERT_TRUE(contains(server.lastBody,
                              "\"speedMuteVolume\":0,\"speedMuteVoice\":true,\"stealthEnabled\":false"));
}

void test_quiet_post_uses_shared_update_but_ignores_audio_only_fields() {
    Probe probe;
    WebServer server(80);
    server.setArg("alertVolumeFadeDelaySec", "99");
    server.setArg("speedMuteVolume", "11");
    server.setArg("stealthEnabled", "1");
    server.setArg("speedMuteVoice", "false");
    server.setArg("voiceVolume", "44");

    WifiQuietApiService::handleApiSave(server, makeRuntime(probe));

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_STRING("{\"success\":true}", server.lastBody.c_str());
    TEST_ASSERT_EQUAL_INT(1, probe.applyCalls);
    TEST_ASSERT_TRUE(probe.update.hasAlertVolumeFadeDelaySec);
    TEST_ASSERT_EQUAL_UINT8(10, probe.update.alertVolumeFadeDelaySec);
    TEST_ASSERT_TRUE(probe.update.hasSpeedMuteVolume);
    TEST_ASSERT_EQUAL_UINT8(0, probe.update.speedMuteVolume);
    TEST_ASSERT_TRUE(probe.update.hasStealthEnabled);
    TEST_ASSERT_TRUE(probe.update.stealthEnabled);
    TEST_ASSERT_FALSE(probe.update.hasSpeedMuteVoice);
    TEST_ASSERT_FALSE(probe.update.hasVoiceVolume);
    TEST_ASSERT_EQUAL_INT(0, probe.volumeCalls);
}

void test_audio_post_applies_shared_and_audio_only_fields_once() {
    Probe probe;
    WebServer server(80);
    server.setArg("speedMuteThresholdMph", "2");
    server.setArg("speedMuteVoice", "false");
    server.setArg("voiceVolume", "120");

    WifiAudioApiService::handleApiSave(server, makeRuntime(probe));

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(1, probe.applyCalls);
    TEST_ASSERT_TRUE(probe.update.hasSpeedMuteThresholdMph);
    TEST_ASSERT_EQUAL_UINT8(5, probe.update.speedMuteThresholdMph);
    TEST_ASSERT_TRUE(probe.update.hasSpeedMuteVoice);
    TEST_ASSERT_FALSE(probe.update.speedMuteVoice);
    TEST_ASSERT_TRUE(probe.update.hasVoiceVolume);
    TEST_ASSERT_EQUAL_UINT8(100, probe.update.voiceVolume);
    TEST_ASSERT_EQUAL_INT(1, probe.volumeCalls);
    TEST_ASSERT_EQUAL_UINT8(100, probe.volume);
}

void test_audio_post_reports_persist_failure_without_live_volume_change() {
    Probe probe;
    probe.persistSuccess = false;
    WebServer server(80);
    server.setArg("voiceVolume", "44");

    WifiAudioApiService::handleApiSave(server, makeRuntime(probe));

    TEST_ASSERT_EQUAL_INT(500, server.lastStatusCode);
    TEST_ASSERT_TRUE(contains(server.lastBody, "settings_persist_failed"));
    TEST_ASSERT_EQUAL_INT(1, probe.applyCalls);
    TEST_ASSERT_EQUAL_INT(0, probe.volumeCalls);
}

void test_quiet_post_reports_persist_failure() {
    Probe probe;
    probe.persistSuccess = false;
    WebServer server(80);
    server.setArg("stealthEnabled", "true");

    WifiQuietApiService::handleApiSave(server, makeRuntime(probe));

    TEST_ASSERT_EQUAL_INT(500, server.lastStatusCode);
    TEST_ASSERT_TRUE(contains(server.lastBody, "settings_persist_failed"));
    TEST_ASSERT_EQUAL_INT(1, probe.applyCalls);
}

void test_device_settings_post_reports_persist_failure() {
    Probe probe;
    probe.persistSuccess = false;
    WebServer server(80);
    server.setArg("proxy_ble", "false");

    WifiSettingsApiService::handleApiDeviceSettingsSave(server, makeDeviceRuntime(probe));

    TEST_ASSERT_EQUAL_INT(500, server.lastStatusCode);
    TEST_ASSERT_TRUE(contains(server.lastBody, "settings_persist_failed"));
    TEST_ASSERT_EQUAL_INT(1, probe.deviceApplyCalls);
    TEST_ASSERT_TRUE(probe.deviceUpdate.hasProxyBLE);
}

void test_display_save_reports_persist_failure_without_live_effects() {
    Probe probe;
    probe.persistSuccess = false;
    WebServer server(80);
    server.setArg("brightness", "44");
    server.setArg("bogey", "123");

    WifiDisplayColorsApiService::handleApiSave(server, makeDisplayRuntime(probe), nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(500, server.lastStatusCode);
    TEST_ASSERT_TRUE(contains(server.lastBody, "settings_persist_failed"));
    TEST_ASSERT_EQUAL_INT(1, probe.displayApplyCalls);
    TEST_ASSERT_EQUAL_INT(0, probe.brightnessCalls);
    TEST_ASSERT_EQUAL_INT(0, probe.redrawCalls);
    TEST_ASSERT_EQUAL_INT(0, probe.previewCalls);
}

void test_display_reset_reports_persist_failure_without_live_effects() {
    Probe probe;
    probe.persistSuccess = false;
    WebServer server(80);

    WifiDisplayColorsApiService::handleApiReset(server, makeDisplayRuntime(probe), nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(500, server.lastStatusCode);
    TEST_ASSERT_TRUE(contains(server.lastBody, "settings_persist_failed"));
    TEST_ASSERT_EQUAL_INT(1, probe.resetCalls);
    TEST_ASSERT_EQUAL_INT(0, probe.redrawCalls);
    TEST_ASSERT_EQUAL_INT(0, probe.previewCalls);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_quiet_get_preserves_its_exact_field_set);
    RUN_TEST(test_audio_get_preserves_quiet_superset_order_and_fields);
    RUN_TEST(test_quiet_post_uses_shared_update_but_ignores_audio_only_fields);
    RUN_TEST(test_audio_post_applies_shared_and_audio_only_fields_once);
    RUN_TEST(test_audio_post_reports_persist_failure_without_live_volume_change);
    RUN_TEST(test_quiet_post_reports_persist_failure);
    RUN_TEST(test_device_settings_post_reports_persist_failure);
    RUN_TEST(test_display_save_reports_persist_failure_without_live_effects);
    RUN_TEST(test_display_reset_reports_persist_failure_without_live_effects);
    return UNITY_END();
}

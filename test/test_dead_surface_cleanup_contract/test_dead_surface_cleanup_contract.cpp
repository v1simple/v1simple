#include <unity.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace {

std::string readTextFile(const char* path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        return {};
    }
    return std::string((std::istreambuf_iterator<char>(stream)),
                       std::istreambuf_iterator<char>());
}

bool pathExists(const char* path) {
    return std::filesystem::exists(path);
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_removed_camera_label_helper_is_no_longer_declared_or_defined() {
    const std::string displayHeader = readTextFile("src/display.h");
    const std::string displayFrequency = readTextFile("src/display_frequency.cpp");

    TEST_ASSERT_FALSE_MESSAGE(displayHeader.empty(), "failed to read src/display.h");
    TEST_ASSERT_FALSE_MESSAGE(displayFrequency.empty(), "failed to read src/display_frequency.cpp");
    TEST_ASSERT_EQUAL(std::string::npos, displayHeader.find("drawCameraLabel"));
    TEST_ASSERT_EQUAL(std::string::npos, displayFrequency.find("drawCameraLabel"));
}

void test_first_batch_timing_state_uses_uint32_and_explicit_wrap_math() {
    const std::string batteryHeader = readTextFile("src/battery_manager.h");
    const std::string displayHeader = readTextFile("src/display.h");
    const std::string touchHeader = readTextFile("src/touch_handler.h");
    const std::string touchSource = readTextFile("src/touch_handler.cpp");
    const std::string displayStatus = readTextFile("src/display_status_bar.cpp");

    TEST_ASSERT_FALSE_MESSAGE(batteryHeader.empty(), "failed to read src/battery_manager.h");
    TEST_ASSERT_FALSE_MESSAGE(displayHeader.empty(), "failed to read src/display.h");
    TEST_ASSERT_FALSE_MESSAGE(touchHeader.empty(), "failed to read src/touch_handler.h");
    TEST_ASSERT_FALSE_MESSAGE(touchSource.empty(), "failed to read src/touch_handler.cpp");
    TEST_ASSERT_FALSE_MESSAGE(displayStatus.empty(), "failed to read src/display_status_bar.cpp");

    TEST_ASSERT_NOT_EQUAL(std::string::npos, batteryHeader.find("uint32_t lastButtonPress_"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, batteryHeader.find("uint32_t buttonPressStart_"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, batteryHeader.find("uint32_t lastUpdateMs_"));
    TEST_ASSERT_EQUAL(std::string::npos, batteryHeader.find("unsigned long lastButtonPress_"));
    TEST_ASSERT_EQUAL(std::string::npos, batteryHeader.find("unsigned long buttonPressStart_"));
    TEST_ASSERT_EQUAL(std::string::npos, batteryHeader.find("unsigned long lastUpdateMs_"));

    TEST_ASSERT_NOT_EQUAL(std::string::npos, displayHeader.find("uint32_t wifiConnectedTime_ = 0;"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, displayHeader.find("uint32_t profileChangedTime_ = 0;"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, displayHeader.find("static constexpr uint32_t HIDE_TIMEOUT_MS = 3000;"));
    TEST_ASSERT_EQUAL(std::string::npos, displayHeader.find("unsigned long wifiConnectedTime_ = 0;"));
    TEST_ASSERT_EQUAL(std::string::npos, displayHeader.find("unsigned long profileChangedTime_ = 0;"));
    TEST_ASSERT_EQUAL(std::string::npos, displayHeader.find("static const unsigned long HIDE_TIMEOUT_MS = 3000;"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, displayStatus.find("const uint32_t nowMs = static_cast<uint32_t>(millis());"));

    TEST_ASSERT_NOT_EQUAL(std::string::npos, touchHeader.find("uint32_t lastTouchTime_"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, touchHeader.find("uint32_t lastReleaseTime_"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, touchHeader.find("uint32_t touchDebounceMs_"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, touchHeader.find("uint32_t releaseDebounceMs_"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, touchHeader.find("void noteNoTouch(uint32_t now);"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, touchHeader.find("bool isI2cPollBackoffActive(uint32_t now) const;"));
    TEST_ASSERT_EQUAL(std::string::npos, touchHeader.find("unsigned long lastTouchTime_"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, touchSource.find("inline bool hasElapsedMs(uint32_t now, uint32_t start, uint32_t intervalMs)"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, touchSource.find("inline bool isBeforeDeadlineMs(uint32_t now, uint32_t deadline)"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, touchSource.find("static_cast<int32_t>(now - deadline) < 0"));
}

void test_legacy_display_dirty_region_tracking_is_fully_removed() {
    const std::string displayHeader = readTextFile("src/display.h");
    const std::string displayFrequency = readTextFile("src/display_frequency.cpp");

    TEST_ASSERT_FALSE_MESSAGE(displayHeader.empty(), "failed to read src/display.h");
    TEST_ASSERT_FALSE_MESSAGE(displayFrequency.empty(), "failed to read src/display_frequency.cpp");
    TEST_ASSERT_FALSE(pathExists("src/display_cards.cpp"));
    TEST_ASSERT_EQUAL(std::string::npos, displayHeader.find("markFrequencyDirtyRegion"));
    TEST_ASSERT_EQUAL(std::string::npos, displayFrequency.find("markFrequencyDirtyRegion"));
    TEST_ASSERT_EQUAL(std::string::npos, displayHeader.find("frequencyRenderDirty_"));
    TEST_ASSERT_EQUAL(std::string::npos, displayHeader.find("frequencyDirtyValid_"));
    TEST_ASSERT_EQUAL(std::string::npos, displayHeader.find("frequencyDirtyX_"));
    TEST_ASSERT_EQUAL(std::string::npos, displayHeader.find("frequencyDirtyY_"));
    TEST_ASSERT_EQUAL(std::string::npos, displayHeader.find("frequencyDirtyW_"));
    TEST_ASSERT_EQUAL(std::string::npos, displayHeader.find("frequencyDirtyH_"));
    TEST_ASSERT_EQUAL(std::string::npos, displayHeader.find("secondaryCardsRenderDirty_"));
    TEST_ASSERT_EQUAL(std::string::npos, displayFrequency.find("Legacy dirty region tracking"));
}

void test_wifi_toggle_setup_mode_is_fully_removed() {
    const std::string header = readTextFile("src/wifi_manager.h");
    const std::string source = readTextFile("src/wifi_manager_lifecycle.cpp");

    TEST_ASSERT_FALSE_MESSAGE(header.empty(), "failed to read src/wifi_manager.h");
    TEST_ASSERT_FALSE_MESSAGE(source.empty(), "failed to read src/wifi_manager_lifecycle.cpp");
    TEST_ASSERT_EQUAL(std::string::npos, header.find("toggleSetupMode"));
    TEST_ASSERT_EQUAL(std::string::npos, source.find("toggleSetupMode"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, header.find("bool startSetupMode("));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, header.find("bool stopSetupMode("));
}

void test_alert_persistence_update_is_fully_removed() {
    const std::string header = readTextFile("src/modules/alert_persistence/alert_persistence_module.h");
    const std::string source = readTextFile("src/modules/alert_persistence/alert_persistence_module.cpp");

    TEST_ASSERT_FALSE_MESSAGE(header.empty(), "failed to read alert_persistence_module.h");
    TEST_ASSERT_FALSE_MESSAGE(source.empty(), "failed to read alert_persistence_module.cpp");
    TEST_ASSERT_EQUAL(std::string::npos, header.find("void update("));
    TEST_ASSERT_EQUAL(std::string::npos, source.find("AlertPersistenceModule::update("));
    TEST_ASSERT_EQUAL(std::string::npos, header.find("Compatibility-retained no-op hook."));
    TEST_ASSERT_EQUAL(std::string::npos,
                      source.find("Compatibility-retained no-op: production no longer needs loop work here."));
    TEST_ASSERT_EQUAL(std::string::npos, header.find("initialized = false"));
    TEST_ASSERT_EQUAL(std::string::npos, source.find("initialized = true;"));
}

void test_perf_display_screen_uses_explicit_mapping_and_removes_retired_values() {
    const std::string perfHeader = readTextFile("src/perf_metrics.h");
    const std::string displayHeader = readTextFile("src/display.h");
    const std::string displayCore = readTextFile("src/display.cpp");
    const std::string displayScreens = readTextFile("src/display_screens.cpp");
    const std::string displayUpdate = readTextFile("src/display_update.cpp");

    TEST_ASSERT_FALSE_MESSAGE(perfHeader.empty(), "failed to read src/perf_metrics.h");
    TEST_ASSERT_FALSE_MESSAGE(displayHeader.empty(), "failed to read src/display.h");
    TEST_ASSERT_FALSE_MESSAGE(displayCore.empty(), "failed to read src/display.cpp");
    TEST_ASSERT_FALSE_MESSAGE(displayScreens.empty(), "failed to read src/display_screens.cpp");
    TEST_ASSERT_FALSE_MESSAGE(displayUpdate.empty(), "failed to read src/display_update.cpp");
    TEST_ASSERT_NOT_EQUAL(std::string::npos, perfHeader.find("Unknown = 0"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, perfHeader.find("Resting = 1"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, perfHeader.find("Scanning = 2"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, perfHeader.find("Live = 4"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, perfHeader.find("Persisted = 5"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          perfHeader.find("Current producers emit only Unknown,"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          perfHeader.find("retired Disconnected"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          perfHeader.find("retired Camera"));
    TEST_ASSERT_EQUAL(std::string::npos, perfHeader.find("Disconnected = 3"));
    TEST_ASSERT_EQUAL(std::string::npos, perfHeader.find("Camera = 6"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          displayHeader.find("static PerfDisplayScreen perfScreenForMode(ScreenMode mode);"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          displayCore.find("PerfDisplayScreen V1Display::perfScreenForMode(ScreenMode mode)"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          displayCore.find("case ScreenMode::Disconnected:"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          displayCore.find("return PerfDisplayScreen::Unknown;"));
    TEST_ASSERT_EQUAL(std::string::npos,
                      displayScreens.find("static_cast<PerfDisplayScreen>(static_cast<uint8_t>(currentScreen))"));
    TEST_ASSERT_EQUAL(std::string::npos,
                      displayUpdate.find("static_cast<PerfDisplayScreen>(static_cast<uint8_t>(currentScreen))"));
    TEST_ASSERT_EQUAL(std::string::npos, displayScreens.find("PerfDisplayScreen::Camera"));
    TEST_ASSERT_EQUAL(std::string::npos, displayUpdate.find("PerfDisplayScreen::Camera"));
    TEST_ASSERT_EQUAL(std::string::npos, displayScreens.find("PerfDisplayScreen::Disconnected"));
    TEST_ASSERT_EQUAL(std::string::npos, displayUpdate.find("PerfDisplayScreen::Disconnected"));
}

void test_local_audio_playback_surface_is_restored() {
    TEST_ASSERT_TRUE(pathExists("src/audio_beep.h"));
    TEST_ASSERT_TRUE(pathExists("src/audio_beep.cpp"));
    TEST_ASSERT_TRUE(pathExists("src/audio_voice.cpp"));
    TEST_ASSERT_TRUE(pathExists("include/audio_internals.h"));
    TEST_ASSERT_TRUE(pathExists("include/audio_task_utils.h"));
    TEST_ASSERT_TRUE(pathExists("src/modules/voice/voice_module.h"));
    TEST_ASSERT_TRUE(pathExists("src/modules/voice/voice_module.cpp"));
    TEST_ASSERT_TRUE(pathExists("src/modules/wifi/wifi_audio_api_service.h"));
    TEST_ASSERT_TRUE(pathExists("src/modules/wifi/wifi_audio_api_service.cpp"));
    TEST_ASSERT_TRUE(pathExists("config/audio_asset_manifest.json"));
    TEST_ASSERT_TRUE(pathExists("interface/scripts/audio-manifest.js"));
    TEST_ASSERT_TRUE(pathExists("interface/src/routes/audio"));
    TEST_ASSERT_FALSE(pathExists("tools/voice_audio"));
    TEST_ASSERT_FALSE(pathExists("tools/asset_generators"));
}

void test_secondary_alert_cards_do_not_carry_render_state_or_paint_geometry() {
    const std::string displayHeader = readTextFile("src/display.h");
    const std::string displayLayout = readTextFile("include/display_layout.h");
    const std::string displayCaches = readTextFile("include/display_element_caches.h");
    const std::string displayUpdate = readTextFile("src/display_update.cpp");

    TEST_ASSERT_FALSE(pathExists("src/display_cards.cpp"));
    TEST_ASSERT_FALSE_MESSAGE(displayHeader.empty(), "failed to read src/display.h");
    TEST_ASSERT_FALSE_MESSAGE(displayLayout.empty(), "failed to read include/display_layout.h");
    TEST_ASSERT_FALSE_MESSAGE(displayCaches.empty(), "failed to read include/display_element_caches.h");
    TEST_ASSERT_FALSE_MESSAGE(displayUpdate.empty(), "failed to read src/display_update.cpp");

    TEST_ASSERT_EQUAL(std::string::npos, displayHeader.find("drawSecondaryAlertCards"));
    TEST_ASSERT_EQUAL(std::string::npos, displayLayout.find("SECONDARY_ROW_HEIGHT"));
    TEST_ASSERT_EQUAL(std::string::npos, displayLayout.find("SECONDARY_ROW_Y"));
    TEST_ASSERT_EQUAL(std::string::npos, displayLayout.find("kSecondaryCardsRect"));
    TEST_ASSERT_EQUAL(std::string::npos, displayCaches.find("CardsRenderCache"));
    TEST_ASSERT_EQUAL(std::string::npos, displayCaches.find("cards"));
    TEST_ASSERT_EQUAL(std::string::npos, displayUpdate.find("PerfDisplayRenderSubphase::Cards"));
    TEST_ASSERT_EQUAL(std::string::npos, displayUpdate.find("drawSecondaryAlertCards"));
}

void test_legacy_wifi_control_routes_are_fully_removed() {
    const std::string routes = readTextFile("src/wifi_routes.cpp");
    const std::string statusHeader = readTextFile("src/modules/wifi/wifi_status_api_service.h");
    const std::string statusSource = readTextFile("src/modules/wifi/wifi_status_api_service.cpp");
    const std::string wifiHeader = readTextFile("src/wifi_manager.h");
    const std::string orchestrator = readTextFile("src/modules/wifi/wifi_orchestrator_module.cpp");

    TEST_ASSERT_FALSE_MESSAGE(routes.empty(), "failed to read src/wifi_routes.cpp");
    TEST_ASSERT_FALSE_MESSAGE(statusHeader.empty(), "failed to read wifi_status_api_service.h");
    TEST_ASSERT_FALSE_MESSAGE(statusSource.empty(), "failed to read wifi_status_api_service.cpp");
    TEST_ASSERT_FALSE_MESSAGE(wifiHeader.empty(), "failed to read src/wifi_manager.h");
    TEST_ASSERT_FALSE_MESSAGE(orchestrator.empty(), "failed to read wifi_orchestrator_module.cpp");

    TEST_ASSERT_EQUAL(std::string::npos, routes.find("server_.on(\"/status\""));
    TEST_ASSERT_EQUAL(std::string::npos, routes.find("server_.on(\"/darkmode\""));
    TEST_ASSERT_EQUAL(std::string::npos, routes.find("server_.on(\"/mute\""));
    TEST_ASSERT_EQUAL(std::string::npos, routes.find("server_.on(\"/api/profile/push\""));
    TEST_ASSERT_EQUAL(std::string::npos, routes.find("server_.on(\"/api/wifi/ap/disable\""));

    TEST_ASSERT_EQUAL(std::string::npos, statusHeader.find("handleApiLegacyStatus"));
    TEST_ASSERT_EQUAL(std::string::npos, statusSource.find("handleApiLegacyStatus"));
    TEST_ASSERT_FALSE(pathExists("src/modules/wifi/wifi_control_api_service.h"));
    TEST_ASSERT_FALSE(pathExists("src/modules/wifi/wifi_control_api_service.cpp"));

    TEST_ASSERT_EQUAL(std::string::npos, wifiHeader.find("setCommandCallback"));
    TEST_ASSERT_EQUAL(std::string::npos, wifiHeader.find("setProfilePushCallback"));
    TEST_ASSERT_EQUAL(std::string::npos, wifiHeader.find("sendV1Command_"));
    TEST_ASSERT_EQUAL(std::string::npos, wifiHeader.find("requestProfilePush_"));
    TEST_ASSERT_EQUAL(std::string::npos, wifiHeader.find("dropApKeepingSta"));
    TEST_ASSERT_EQUAL(std::string::npos, orchestrator.find("setCommandCallback"));
    TEST_ASSERT_EQUAL(std::string::npos, orchestrator.find("setProfilePushCallback"));
}

void test_direct_wifi_connect_route_is_fully_removed() {
    const std::string routes = readTextFile("src/wifi_routes.cpp");
    const std::string clientHeader = readTextFile("src/modules/wifi/wifi_client_api_service.h");
    const std::string clientSource = readTextFile("src/modules/wifi/wifi_client_api_service.cpp");
    const std::string runtimeSource = readTextFile("src/wifi_runtimes.cpp");

    TEST_ASSERT_FALSE_MESSAGE(routes.empty(), "failed to read src/wifi_routes.cpp");
    TEST_ASSERT_FALSE_MESSAGE(clientHeader.empty(), "failed to read wifi_client_api_service.h");
    TEST_ASSERT_FALSE_MESSAGE(clientSource.empty(), "failed to read wifi_client_api_service.cpp");
    TEST_ASSERT_FALSE_MESSAGE(runtimeSource.empty(), "failed to read src/wifi_runtimes.cpp");

    TEST_ASSERT_EQUAL(std::string::npos, routes.find("server_.on(\"/api/wifi/connect\""));
    TEST_ASSERT_EQUAL(std::string::npos, clientHeader.find("handleApiConnect"));
    TEST_ASSERT_EQUAL(std::string::npos, clientSource.find("handleApiConnect"));
    TEST_ASSERT_EQUAL(std::string::npos, clientSource.find("parseConnectRequest"));
    TEST_ASSERT_EQUAL(std::string::npos, clientSource.find("sendConnectStarted"));
    TEST_ASSERT_EQUAL(std::string::npos, clientSource.find("no_saved_network_slot"));
    TEST_ASSERT_EQUAL(std::string::npos, clientHeader.find("selectSlotForConnect"));
    TEST_ASSERT_EQUAL(std::string::npos, clientHeader.find("connectToNetworkSlot"));
    TEST_ASSERT_EQUAL(std::string::npos, runtimeSource.find("selectSlotForNetworkConnect"));
    TEST_ASSERT_EQUAL(std::string::npos, runtimeSource.find("connectToNetworkSlot"));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_removed_camera_label_helper_is_no_longer_declared_or_defined);
    RUN_TEST(test_first_batch_timing_state_uses_uint32_and_explicit_wrap_math);
    RUN_TEST(test_legacy_display_dirty_region_tracking_is_fully_removed);
    RUN_TEST(test_wifi_toggle_setup_mode_is_fully_removed);
    RUN_TEST(test_alert_persistence_update_is_fully_removed);
    RUN_TEST(test_perf_display_screen_uses_explicit_mapping_and_removes_retired_values);
    RUN_TEST(test_local_audio_playback_surface_is_restored);
    RUN_TEST(test_secondary_alert_cards_do_not_carry_render_state_or_paint_geometry);
    RUN_TEST(test_legacy_wifi_control_routes_are_fully_removed);
    RUN_TEST(test_direct_wifi_connect_route_is_fully_removed);
    return UNITY_END();
}

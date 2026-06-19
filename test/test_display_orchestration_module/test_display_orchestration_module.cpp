#include <unity.h>

#include "../mocks/Arduino.h"
#include "../mocks/display.h"
#include "../mocks/ble_client.h"
#include "../mocks/modules/ble/ble_queue_module.h"
#include "../mocks/modules/display/display_preview_module.h"
#include "../mocks/modules/display/display_restore_module.h"
#include "../mocks/packet_parser.h"
#include "../mocks/settings.h"
#include "../mocks/modules/volume_fade/volume_fade_module.h"
#include "../mocks/modules/speed_mute/speed_mute_module.h"

#ifndef ARDUINO
SerialClass Serial;
SettingsManager settingsManager;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

// Perf stubs — quiet_coordinator_templates.h calls these but perf_metrics.h
// is excluded in UNIT_TEST builds.
void perfRecordSpeedVolDrop() {}
void perfRecordSpeedVolRestore() {}
void perfRecordSpeedVolRetry() {}

#include "../../src/modules/quiet/quiet_coordinator_module.cpp"
#include "../../src/modules/display/display_orchestration_module.cpp"

static V1Display display;
static V1BLEClient ble;
static BleQueueModule bleQueue;
static DisplayPreviewModule preview;
static DisplayRestoreModule restore;
static PacketParser parser;
static VolumeFadeModule volumeFade;
static SpeedMuteModule speedMute;
static QuietCoordinatorModule quiet;
static DisplayOrchestrationModule module;

static void beginModule() {
    quiet.begin(&ble, &parser);
    module.begin(&display,
                 &ble,
                 &bleQueue,
                 &preview,
                 &restore,
                 &parser,
                 &settingsManager,
                 &volumeFade,
                 &speedMute,
                 &quiet);
}

void setUp() {
    display.reset();
    ble.reset();
    bleQueue.reset();
    preview = DisplayPreviewModule{};
    restore = DisplayRestoreModule{};
    parser.reset();
    volumeFade = VolumeFadeModule{};
    speedMute = SpeedMuteModule{};
    settingsManager = SettingsManager{};
    beginModule();
}

void tearDown() {}

void test_process_early_updates_ble_context_and_proxy_status() {
    DisplayOrchestrationEarlyContext ctx;
    ctx.nowMs = 1200;
    ctx.bootSplashHoldActive = false;
    ctx.overloadThisLoop = false;
    ctx.bleContext = {true, true, -55, -66};
    ctx.bleReceiving = true;

    module.processEarly(ctx);

    TEST_ASSERT_EQUAL(1, display.setBleContextCalls);
    TEST_ASSERT_TRUE(display.lastBleContext.v1Connected);
    TEST_ASSERT_TRUE(display.lastBleContext.proxyConnected);
    TEST_ASSERT_EQUAL(-55, display.lastBleContext.v1Rssi);
    TEST_ASSERT_EQUAL(-66, display.lastBleContext.proxyRssi);
    TEST_ASSERT_EQUAL(1, display.setBLEProxyStatusCalls);
    TEST_ASSERT_TRUE(display.lastBleProxyEnabled);
    TEST_ASSERT_TRUE(display.lastBleProxyConnected);
    TEST_ASSERT_TRUE(display.lastBleReceiving);
}

void test_process_early_updates_preview_or_restore_path() {
    DisplayOrchestrationEarlyContext ctx;
    ctx.bootSplashHoldActive = false;
    ctx.overloadThisLoop = false;
    ctx.bleContext = {false, false, 0, 0};

    preview.setRunning(true);
    module.processEarly(ctx);
    TEST_ASSERT_EQUAL(1, preview.updateCalls);
    TEST_ASSERT_EQUAL(0, restore.processCalls);

    preview.setRunning(false);
    module.processEarly(ctx);
    TEST_ASSERT_EQUAL(1, restore.processCalls);
}

void test_parsed_frame_sets_status_indicators_and_requests_pipeline() {
    ble.setConnected(true);
    ble.setProxyConnected(false);

    DisplayOrchestrationParsedContext ctx;
    ctx.nowMs = 5000;
    ctx.parsedReady = true;
    ctx.bootSplashHoldActive = false;

    const auto result = module.processParsedFrame(ctx);

    TEST_ASSERT_TRUE(result.runDisplayPipeline);
}

void test_parsed_frame_skips_pipeline_when_preview_running() {
    preview.setRunning(true);

    DisplayOrchestrationParsedContext ctx;
    ctx.nowMs = 7000;
    ctx.parsedReady = true;
    ctx.bootSplashHoldActive = false;

    const auto result = module.processParsedFrame(ctx);
    TEST_ASSERT_FALSE(result.runDisplayPipeline);
    TEST_ASSERT_EQUAL_STRING("preview_running", result.reasonSkipped);
    TEST_ASSERT_EQUAL(1, display.setSpeedVolZeroActiveCalls);
    TEST_ASSERT_EQUAL(0, volumeFade.processCalls);
}

void test_parsed_frame_returns_reason_when_no_parsed_frame() {
    DisplayOrchestrationParsedContext ctx;
    ctx.nowMs = 7100;
    ctx.parsedReady = false;
    ctx.bootSplashHoldActive = false;

    const auto result = module.processParsedFrame(ctx);

    TEST_ASSERT_FALSE(result.runDisplayPipeline);
    TEST_ASSERT_EQUAL_STRING("no_parsed_frame", result.reasonSkipped);
    TEST_ASSERT_EQUAL(1, display.setSpeedVolZeroActiveCalls);
    TEST_ASSERT_EQUAL(0, volumeFade.processCalls);
}

void test_parsed_frame_returns_reason_when_boot_splash_blocks_pipeline() {
    DisplayOrchestrationParsedContext ctx;
    ctx.nowMs = 7200;
    ctx.parsedReady = true;
    ctx.bootSplashHoldActive = true;

    const auto result = module.processParsedFrame(ctx);

    TEST_ASSERT_FALSE(result.runDisplayPipeline);
    TEST_ASSERT_EQUAL_STRING("boot_splash", result.reasonSkipped);
    TEST_ASSERT_EQUAL(1, display.setSpeedVolZeroActiveCalls);
    TEST_ASSERT_EQUAL(0, volumeFade.processCalls);
}

void test_parsed_frame_syncs_speedvol_zero_presentation_from_coordinator() {
    // Use the setter so hasVolumeData flips true — the quiet coordinator's
    // DROP path defers until V1 has delivered real volume data
    // (docs/plans/SPEED_MUTE_DROP_BASELINE_20260422.md).
    parser.setMainVolume(6);
    parser.setMuteVolume(2);
    speedMute.begin(true, 25, 3, 0);
    speedMute.state_.muteActive = true;

    DisplayOrchestrationParsedContext ctx;
    ctx.nowMs = 7800;
    ctx.parsedReady = true;
    ctx.bootSplashHoldActive = false;

    module.processParsedFrame(ctx);

    TEST_ASSERT_EQUAL(1, display.setSpeedVolZeroActiveCalls);
    TEST_ASSERT_TRUE(display.lastSpeedVolZeroActiveValue);
}

void test_parsed_frame_executes_volume_fade_when_pipeline_runs() {
    parser.state.mainVolume = 8;
    parser.state.muteVolume = 2;
    parser.state.muted = false;
    parser.setAlerts({
        AlertData::create(BAND_KA, DIR_FRONT, 6, 0, 35500, true, true)
    });
    volumeFade.nextAction.type = VolumeFadeAction::Type::FADE_DOWN;
    volumeFade.nextAction.targetVolume = 3;
    volumeFade.nextAction.targetMuteVolume = 1;

    DisplayOrchestrationParsedContext ctx;
    ctx.nowMs = 5200;
    ctx.parsedReady = true;
    ctx.bootSplashHoldActive = false;

    module.processParsedFrame(ctx);

    TEST_ASSERT_EQUAL(1, volumeFade.processCalls);
    TEST_ASSERT_TRUE(volumeFade.lastContext.hasAlert);
    TEST_ASSERT_EQUAL_UINT8(8, volumeFade.lastContext.currentVolume);
    TEST_ASSERT_EQUAL_UINT8(2, volumeFade.lastContext.currentMuteVolume);
    TEST_ASSERT_EQUAL_UINT16(35500u, volumeFade.lastContext.currentFrequency);
    TEST_ASSERT_EQUAL(1, ble.setVolumeCalls);
    TEST_ASSERT_EQUAL_UINT8(3, ble.lastVolume);
    TEST_ASSERT_EQUAL_UINT8(1, ble.lastMuteVolume);
}

void test_lightweight_refresh_reports_priority_active() {
    ble.setConnected(true);
    preview.setRunning(false);

    parser.state.muted = false;
    parser.state.bogeyCounterChar = '1';
    parser.state.hasPhotoAlert = false;
    parser.setAlerts({
        AlertData::create(BAND_KA, DIR_FRONT, 6, 0, 35500, true, true)
    });

    DisplayOrchestrationRefreshContext ctx;
    ctx.nowMs = 500;
    ctx.bootSplashHoldActive = false;
    ctx.overloadLateThisLoop = true;
    ctx.pipelineRanThisLoop = false;

    const auto result = module.processLightweightRefresh(ctx);

    TEST_ASSERT_TRUE(result.signalPriorityActive);
}

void test_lightweight_refresh_reports_inactive_when_idle() {
    ble.setConnected(true);
    preview.setRunning(false);
    parser.reset();

    DisplayOrchestrationRefreshContext ctx;
    ctx.nowMs = 500;
    ctx.bootSplashHoldActive = false;
    ctx.overloadLateThisLoop = false;
    ctx.pipelineRanThisLoop = false;

    const auto result = module.processLightweightRefresh(ctx);

    TEST_ASSERT_FALSE(result.signalPriorityActive);
}

// --- Speed volume tests ---

// Helper to enable speed volume and make it active (muteActive = true).
static void enableSpeedVolume(uint8_t targetVol, uint8_t originalVol = 7,
                              uint8_t originalMuteVol = 2) {
    speedMute.settings_.v1Volume = targetVol;
    speedMute.settings_.enabled = true;
    speedMute.state_.muteActive = true;
    parser.setMainVolume(originalVol);
    parser.setMuteVolume(originalMuteVol);
}

void test_speed_vol_drops_volume_when_mute_active() {
    enableSpeedVolume(3);

    DisplayOrchestrationParsedContext ctx;
    ctx.nowMs = 1000;
    ctx.parsedReady = true;

    module.processParsedFrame(ctx);

    // Should have sent setVolume with target 3, mute vol unchanged (2).
    TEST_ASSERT_EQUAL(1, ble.setVolumeCalls);
    TEST_ASSERT_EQUAL_UINT8(3, ble.lastVolume);
    TEST_ASSERT_EQUAL_UINT8(2, ble.lastMuteVolume);
    // Volume fade should be gated.
    TEST_ASSERT_EQUAL(0, volumeFade.processCalls);
}

void test_speed_vol_confirmed_by_v1_holds_steady() {
    enableSpeedVolume(3);

    DisplayOrchestrationParsedContext ctx;
    ctx.nowMs = 1000;
    ctx.parsedReady = true;
    module.processParsedFrame(ctx);  // Activate

    // V1 confirms the target volume.
    parser.setMainVolume(3);
    ble.setVolumeCalls = 0;
    ctx.nowMs = 1100;

    module.processParsedFrame(ctx);
    // No retry needed — confirmed.
    TEST_ASSERT_EQUAL(0, ble.setVolumeCalls);
    // Still busy — fade still gated.
    TEST_ASSERT_EQUAL(0, volumeFade.processCalls);
}

void test_speed_vol_retries_until_v1_confirms_drop() {
    enableSpeedVolume(3);

    DisplayOrchestrationParsedContext ctx;
    ctx.nowMs = 1000;
    ctx.parsedReady = true;
    module.processParsedFrame(ctx);  // Activate

    // V1 hasn't confirmed (still at 7).
    ble.setVolumeCalls = 0;
    ctx.nowMs = 1000 + 80;  // Past retry interval (75ms).

    module.processParsedFrame(ctx);
    TEST_ASSERT_EQUAL(1, ble.setVolumeCalls);
    TEST_ASSERT_EQUAL_UINT8(3, ble.lastVolume);
    TEST_ASSERT_EQUAL_UINT8(2, ble.lastMuteVolume);
}

void test_speed_vol_restores_when_mute_deactivates() {
    enableSpeedVolume(3);

    DisplayOrchestrationParsedContext ctx;
    ctx.nowMs = 1000;
    ctx.parsedReady = true;
    module.processParsedFrame(ctx);  // Activate

    // Speed rises above threshold — mute deactivates.
    speedMute.state_.muteActive = false;
    parser.setMainVolume(3);  // V1 still at lowered volume.
    ble.setVolumeCalls = 0;
    volumeFade.setBaselineHintCalls = 0;
    ctx.nowMs = 2000;

    module.processParsedFrame(ctx);

    // Should restore to original volume.
    TEST_ASSERT_EQUAL(1, ble.setVolumeCalls);
    TEST_ASSERT_EQUAL_UINT8(7, ble.lastVolume);
    TEST_ASSERT_EQUAL_UINT8(2, ble.lastMuteVolume);
    // Baseline hint injected into volume fade.
    TEST_ASSERT_EQUAL(1, volumeFade.setBaselineHintCalls);
    TEST_ASSERT_EQUAL_UINT8(7, volumeFade.lastHintVolume);
    TEST_ASSERT_EQUAL_UINT8(2, volumeFade.lastHintMuteVolume);
}

void test_speed_vol_restore_retries_until_confirmed() {
    enableSpeedVolume(3);

    DisplayOrchestrationParsedContext ctx;
    ctx.nowMs = 1000;
    ctx.parsedReady = true;
    module.processParsedFrame(ctx);  // Activate

    // Deactivate.
    speedMute.state_.muteActive = false;
    parser.setMainVolume(3);
    ble.setVolumeCalls = 0;
    ctx.nowMs = 2000;
    module.processParsedFrame(ctx);  // Restore issued

    // V1 still hasn't confirmed (still at 3).
    ble.setVolumeCalls = 0;
    ctx.nowMs = 2000 + 80;  // Past retry interval.

    module.processParsedFrame(ctx);
    TEST_ASSERT_EQUAL(1, ble.setVolumeCalls);
    TEST_ASSERT_EQUAL_UINT8(7, ble.lastVolume);
    TEST_ASSERT_EQUAL_UINT8(2, ble.lastMuteVolume);

    // Confirm arrives.
    parser.setMainVolume(7);
    ble.setVolumeCalls = 0;
    volumeFade.processCalls = 0;
    ctx.nowMs = 2200;

    module.processParsedFrame(ctx);
    // No retry — confirmed. Fade should run again.
    TEST_ASSERT_EQUAL(0, ble.setVolumeCalls);
    TEST_ASSERT_EQUAL(1, volumeFade.processCalls);
}

void test_speed_vol_restore_times_out() {
    enableSpeedVolume(3);

    DisplayOrchestrationParsedContext ctx;
    ctx.nowMs = 1000;
    ctx.parsedReady = true;
    module.processParsedFrame(ctx);  // Activate

    // Deactivate.
    speedMute.state_.muteActive = false;
    parser.setMainVolume(3);
    ctx.nowMs = 2000;
    module.processParsedFrame(ctx);  // Restore issued

    // V1 never confirms — advance past timeout (2000ms).
    ble.setVolumeCalls = 0;
    volumeFade.processCalls = 0;
    ctx.nowMs = 2000 + 2001;

    module.processParsedFrame(ctx);
    // Timed out — no retry. Fade runs again.
    TEST_ASSERT_EQUAL(0, ble.setVolumeCalls);
    TEST_ASSERT_EQUAL(1, volumeFade.processCalls);
}

void test_speed_vol_band_override_restores_on_ka() {
    enableSpeedVolume(3);

    DisplayOrchestrationParsedContext ctx;
    ctx.nowMs = 1000;
    ctx.parsedReady = true;
    module.processParsedFrame(ctx);  // Activate speed vol.

    // Ka alert appears.
    parser.setActiveBands(0x02);  // BAND_KA
    parser.setAlerts({
        AlertData::create(BAND_KA, DIR_FRONT, 6, 0, 35500, true, true)
    });
    ble.setVolumeCalls = 0;
    ctx.nowMs = 2000;

    module.processParsedFrame(ctx);

    // Should restore to original.
    TEST_ASSERT_EQUAL(1, ble.setVolumeCalls);
    TEST_ASSERT_EQUAL_UINT8(7, ble.lastVolume);
    TEST_ASSERT_EQUAL_UINT8(2, ble.lastMuteVolume);
}

void test_speed_vol_band_override_restores_on_laser() {
    enableSpeedVolume(3);

    DisplayOrchestrationParsedContext ctx;
    ctx.nowMs = 1000;
    ctx.parsedReady = true;
    module.processParsedFrame(ctx);  // Activate speed vol.

    // Laser alert appears.
    parser.setActiveBands(0x01);  // BAND_LASER
    parser.setAlerts({
        AlertData::create(BAND_LASER, DIR_FRONT, 8, 0, 0, true, true)
    });
    ble.setVolumeCalls = 0;
    ctx.nowMs = 2000;

    module.processParsedFrame(ctx);

    // Should restore to original.
    TEST_ASSERT_EQUAL(1, ble.setVolumeCalls);
    TEST_ASSERT_EQUAL_UINT8(7, ble.lastVolume);
    TEST_ASSERT_EQUAL_UINT8(2, ble.lastMuteVolume);
}

void test_speed_vol_no_band_override_for_k_band() {
    enableSpeedVolume(3);

    DisplayOrchestrationParsedContext ctx;
    ctx.nowMs = 1000;
    ctx.parsedReady = true;
    module.processParsedFrame(ctx);  // Activate speed vol.

    // K-band alert appears — should NOT trigger override (only Laser/Ka do).
    parser.setActiveBands(0x04);  // BAND_K
    parser.setAlerts({
        AlertData::create(BAND_K, DIR_FRONT, 4, 0, 24150, true, true)
    });
    parser.setMainVolume(3);
    ble.setVolumeCalls = 0;
    ctx.nowMs = 2000;

    module.processParsedFrame(ctx);

    // Should NOT restore — K band is not overridden.
    TEST_ASSERT_EQUAL(0, ble.setVolumeCalls);
}

void test_speed_vol_settings_disable_restores() {
    enableSpeedVolume(3);

    DisplayOrchestrationParsedContext ctx;
    ctx.nowMs = 1000;
    ctx.parsedReady = true;
    module.processParsedFrame(ctx);  // Activate speed vol.

    // User disables speed mute entirely.
    speedMute.settings_.enabled = false;
    speedMute.state_.muteActive = false;
    parser.setMainVolume(3);
    ble.setVolumeCalls = 0;
    volumeFade.setBaselineHintCalls = 0;
    ctx.nowMs = 2000;

    module.processParsedFrame(ctx);

    // Should restore to original.
    TEST_ASSERT_EQUAL(1, ble.setVolumeCalls);
    TEST_ASSERT_EQUAL_UINT8(7, ble.lastVolume);
    TEST_ASSERT_EQUAL_UINT8(2, ble.lastMuteVolume);
    TEST_ASSERT_EQUAL(1, volumeFade.setBaselineHintCalls);
}

void test_speed_vol_gates_volume_fade() {
    enableSpeedVolume(3);
    // Use K-band alert (not overridden) to keep speed vol active.
    parser.state.muted = false;
    parser.setActiveBands(0x04);  // BAND_K
    parser.setAlerts({
        AlertData::create(BAND_K, DIR_FRONT, 4, 0, 24150, true, true)
    });
    volumeFade.nextAction.type = VolumeFadeAction::Type::FADE_DOWN;
    volumeFade.nextAction.targetVolume = 2;
    volumeFade.nextAction.targetMuteVolume = 1;

    DisplayOrchestrationParsedContext ctx;
    ctx.nowMs = 1000;
    ctx.parsedReady = true;

    module.processParsedFrame(ctx);

    // Volume fade should not run while speed vol is active.
    TEST_ASSERT_EQUAL(0, volumeFade.processCalls);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_process_early_updates_ble_context_and_proxy_status);
    RUN_TEST(test_process_early_updates_preview_or_restore_path);
    RUN_TEST(test_parsed_frame_sets_status_indicators_and_requests_pipeline);
    RUN_TEST(test_parsed_frame_skips_pipeline_when_preview_running);
    RUN_TEST(test_parsed_frame_returns_reason_when_no_parsed_frame);
    RUN_TEST(test_parsed_frame_returns_reason_when_boot_splash_blocks_pipeline);
    RUN_TEST(test_parsed_frame_syncs_speedvol_zero_presentation_from_coordinator);
    RUN_TEST(test_parsed_frame_executes_volume_fade_when_pipeline_runs);
    RUN_TEST(test_lightweight_refresh_reports_priority_active);
    RUN_TEST(test_lightweight_refresh_reports_inactive_when_idle);
    RUN_TEST(test_speed_vol_drops_volume_when_mute_active);
    RUN_TEST(test_speed_vol_confirmed_by_v1_holds_steady);
    RUN_TEST(test_speed_vol_retries_until_v1_confirms_drop);
    RUN_TEST(test_speed_vol_restores_when_mute_deactivates);
    RUN_TEST(test_speed_vol_restore_retries_until_confirmed);
    RUN_TEST(test_speed_vol_restore_times_out);
    RUN_TEST(test_speed_vol_band_override_restores_on_ka);
    RUN_TEST(test_speed_vol_band_override_restores_on_laser);
    RUN_TEST(test_speed_vol_no_band_override_for_k_band);
    RUN_TEST(test_speed_vol_settings_disable_restores);
    RUN_TEST(test_speed_vol_gates_volume_fade);
    return UNITY_END();
}

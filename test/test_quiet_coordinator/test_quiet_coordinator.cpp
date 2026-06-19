#include <unity.h>

#include "../mocks/Arduino.h"
#include "../mocks/ble_client.h"
#include "../mocks/packet_parser.h"
#include "../mocks/modules/volume_fade/volume_fade_module.h"
#include "../mocks/modules/speed_mute/speed_mute_module.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

// Perf stubs — quiet_coordinator_templates.h calls these but perf_metrics.h
// is excluded in UNIT_TEST builds.
void perfRecordSpeedVolDrop() {}
void perfRecordSpeedVolRestore() {}
void perfRecordSpeedVolRetry() {}

struct VoiceContext {
    bool isMuted = false;
    bool isSoftMuted = false;
    uint8_t mainVolume = 0;
    bool isSuppressed = false;
};

#include "../../src/modules/quiet/quiet_coordinator_module.cpp"
#include "../../src/modules/quiet/quiet_coordinator_templates.h"
#include "../../src/modules/quiet/quiet_coordinator_voice_templates.h"

static V1BLEClient ble;
static PacketParser parser;
static VolumeFadeModule volumeFade;
static SpeedMuteModule speedMute;
static QuietCoordinatorModule module;

static void beginModule() {
    module.begin(&ble, &parser);
}

void setUp() {
    ble.reset();
    parser.reset();
    volumeFade = VolumeFadeModule{};
    speedMute = SpeedMuteModule{};
    mockMillis = 0;
    mockMicros = 0;
    beginModule();
}

void tearDown() {}

void test_send_mute_tracks_desired_state_and_owner() {
    parser.state.muted = false;

    const bool sent = module.sendMute(QuietOwner::TapGesture, true);

    TEST_ASSERT_TRUE(sent);
    TEST_ASSERT_EQUAL(1, ble.setMuteCalls);
    TEST_ASSERT_TRUE(ble.lastMuteValue);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(QuietOwner::TapGesture),
                          static_cast<int>(module.getDesiredState().muteOwner));
    TEST_ASSERT_TRUE(module.getDesiredState().mutePending);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(QuietOwner::TapGesture),
                          static_cast<int>(module.getPresentationState().activeMuteOwner));
}

static void enableSpeedVol(uint8_t targetVolume) {
    speedMute.begin(true, 25, 3, targetVolume);
    speedMute.state_.muteActive = true;
}

void test_speed_volume_drop_restore_and_zero_presentation() {
    parser.setMainVolume(6);
    parser.setMuteVolume(2);
    enableSpeedVol(0);

    TEST_ASSERT_TRUE(module.processSpeedVolume(1000, speedMute, &volumeFade));
    TEST_ASSERT_EQUAL(1, ble.setVolumeCalls);
    TEST_ASSERT_EQUAL_UINT8(0, ble.lastVolume);
    TEST_ASSERT_TRUE(module.getPresentationState().speedVolZeroActive);

    speedMute.state_.muteActive = false;
    parser.setMainVolume(0);
    TEST_ASSERT_TRUE(module.processSpeedVolume(1200, speedMute, &volumeFade));
    TEST_ASSERT_EQUAL(2, ble.setVolumeCalls);
    TEST_ASSERT_EQUAL_UINT8(6, ble.lastVolume);
    TEST_ASSERT_EQUAL(1, volumeFade.setBaselineHintCalls);
    TEST_ASSERT_FALSE(module.getPresentationState().speedVolZeroActive);
}

// Regression: speed-mute must not DROP until V1 has delivered real volume data.
// Before the hasVolumeData gate, the DROP would capture the default mainVolume=0
// as the saved "original"; the subsequent RESTORE would then write 0 back to V1,
// orphaning the device at 0 and tripping the VOL 0 warning 15s later at rest.
// See docs/plans/SPEED_MUTE_DROP_BASELINE_20260422.md.
void test_speed_volume_drop_deferred_until_volume_data_received() {
    // parser.state starts with hasVolumeData=false and mainVolume=0 (defaults).
    // Do NOT call setMainVolume — we want to simulate the pre-first-packet state.
    TEST_ASSERT_FALSE(parser.state.hasVolumeData);
    TEST_ASSERT_EQUAL_UINT8(0, parser.state.mainVolume);

    enableSpeedVol(0);

    // First tick: speed below threshold, muteActive=true, but V1 volume not
    // yet known. DROP must defer — no BLE volume write, no "zero active" flag.
    TEST_ASSERT_FALSE(module.processSpeedVolume(1000, speedMute, &volumeFade));
    TEST_ASSERT_EQUAL(0, ble.setVolumeCalls);
    TEST_ASSERT_FALSE(module.getPresentationState().speedVolZeroActive);

    // Second tick, still no volume data — still deferred.
    TEST_ASSERT_FALSE(module.processSpeedVolume(1100, speedMute, &volumeFade));
    TEST_ASSERT_EQUAL(0, ble.setVolumeCalls);

    // V1 finally delivers its first volume-bearing display packet.
    parser.setMainVolume(7);
    parser.setMuteVolume(3);
    TEST_ASSERT_TRUE(parser.state.hasVolumeData);

    // Now DROP fires with the real baseline (7), not the default 0.
    TEST_ASSERT_TRUE(module.processSpeedVolume(1200, speedMute, &volumeFade));
    TEST_ASSERT_EQUAL(1, ble.setVolumeCalls);
    TEST_ASSERT_EQUAL_UINT8(0, ble.lastVolume);           // DROP target
    TEST_ASSERT_TRUE(module.getPresentationState().speedVolZeroActive);

    // Unmute: RESTORE must send 7 (the captured real baseline), not 0.
    speedMute.state_.muteActive = false;
    parser.setMainVolume(0);  // V1 currently at drop target
    TEST_ASSERT_TRUE(module.processSpeedVolume(1400, speedMute, &volumeFade));
    TEST_ASSERT_EQUAL(2, ble.setVolumeCalls);
    TEST_ASSERT_EQUAL_UINT8(7, ble.lastVolume);           // RESTORE target
    TEST_ASSERT_FALSE(module.getPresentationState().speedVolZeroActive);
}

void test_voice_presentation_does_not_bypass_real_mute_for_speed_override_without_active_speed_volume() {
    enableSpeedVol(0);

    VoiceContext ctx;
    ctx.isMuted = true;
    ctx.isSoftMuted = true;
    ctx.mainVolume = 0;

    module.applyVoicePresentation(ctx, &speedMute, true, BAND_KA);

    TEST_ASSERT_TRUE(ctx.isMuted);
    TEST_ASSERT_TRUE(ctx.isSoftMuted);
    TEST_ASSERT_EQUAL_UINT8(0, ctx.mainVolume);
    TEST_ASSERT_FALSE(module.getPresentationState().voiceAllowVolZeroBypass);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_send_mute_tracks_desired_state_and_owner);
    RUN_TEST(test_speed_volume_drop_restore_and_zero_presentation);
    RUN_TEST(test_speed_volume_drop_deferred_until_volume_data_received);
    RUN_TEST(test_voice_presentation_does_not_bypass_real_mute_for_speed_override_without_active_speed_volume);
    return UNITY_END();
}

/**
 * Regression boundary: quiet ownership, speed-volume overrides, and voice
 * presentation remain coordinated; a presentation-only override cannot
 * bypass the real mute state.
 */
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

void test_speed_volume_and_voice_honor_user_setting_for_ka() {
    parser.setMainVolume(6);
    parser.setMuteVolume(2);
    parser.setActiveBands(BAND_KA);
    parser.setAlerts({AlertData::create(BAND_KA, DIR_FRONT, 6, 0, 34700, true, true)});
    enableSpeedVol(0);

    TEST_ASSERT_TRUE(module.processSpeedVolume(1000, speedMute, &volumeFade));
    TEST_ASSERT_EQUAL_UINT8(0, ble.lastVolume);
    TEST_ASSERT_TRUE(module.getPresentationState().speedVolZeroActive);

    VoiceContext ctx;
    ctx.mainVolume = 0;
    module.applyVoicePresentation(ctx, &speedMute, true, BAND_KA);

    TEST_ASSERT_TRUE(ctx.isSuppressed);
    TEST_ASSERT_TRUE(module.getPresentationState().voiceSuppressed);
    TEST_ASSERT_FALSE(module.getPresentationState().voiceAllowVolZeroBypass);
}

// Speed-mute must not drop the volume until V1 has delivered real volume data.
// Otherwise the default mainVolume=0 becomes the restore baseline and leaves
// the device at zero volume.
void test_speed_volume_drop_deferred_until_volume_data_received() {
    // parser.state starts with hasVolumeData=false and mainVolume=0 (defaults).
    // Leave mainVolume untouched to simulate the pre-first-packet state.
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

void test_autopush_updates_speed_mute_restore_pair_without_lifting_temporary_volume() {
    parser.setMainVolume(6);
    parser.setMuteVolume(2);
    enableSpeedVol(0);

    TEST_ASSERT_TRUE(module.processSpeedVolume(1000, speedMute, &volumeFade));
    TEST_ASSERT_EQUAL_UINT8(0, ble.lastVolume);
    TEST_ASSERT_EQUAL_UINT8(2, ble.lastMuteVolume);

    TEST_ASSERT_TRUE(module.sendAutoPushVolume(8, 3));
    TEST_ASSERT_EQUAL_UINT8(0, ble.lastVolume);
    TEST_ASSERT_EQUAL_UINT8(3, ble.lastMuteVolume);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(QuietOwner::SpeedVolume),
                          static_cast<int>(module.getDesiredState().volumeOwner));

    speedMute.state_.muteActive = false;
    parser.setMainVolume(0);
    parser.setMuteVolume(3);
    TEST_ASSERT_TRUE(module.processSpeedVolume(1200, speedMute, &volumeFade));
    TEST_ASSERT_EQUAL_UINT8(8, ble.lastVolume);
    TEST_ASSERT_EQUAL_UINT8(3, ble.lastMuteVolume);
}

void test_volume_convergence_requires_both_main_and_mute_values() {
    parser.setMainVolume(4);
    parser.setMuteVolume(1);

    TEST_ASSERT_TRUE(module.sendVolume(QuietOwner::AutoPush, 7, 3));
    parser.setMainVolume(7);
    parser.setMuteVolume(1);
    (void)module.getCommittedState();
    TEST_ASSERT_TRUE(module.getDesiredState().volumePending);

    parser.setMuteVolume(3);
    (void)module.getCommittedState();
    TEST_ASSERT_FALSE(module.getDesiredState().volumePending);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_send_mute_tracks_desired_state_and_owner);
    RUN_TEST(test_speed_volume_drop_restore_and_zero_presentation);
    RUN_TEST(test_speed_volume_and_voice_honor_user_setting_for_ka);
    RUN_TEST(test_speed_volume_drop_deferred_until_volume_data_received);
    RUN_TEST(test_voice_presentation_does_not_bypass_real_mute_for_speed_override_without_active_speed_volume);
    RUN_TEST(test_autopush_updates_speed_mute_restore_pair_without_lifting_temporary_volume);
    RUN_TEST(test_volume_convergence_requires_both_main_and_mute_values);
    return UNITY_END();
}

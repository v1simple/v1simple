// Real InTheBoxModule + QuietCoordinatorModule; parser observations and BLE
// send outcomes are controlled. These tests do not prove physical delivery.
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

namespace {
V1BLEClient ble;
PacketParser parser;
QuietCoordinatorModule module;
SpeedMuteModule speedMute;
VolumeFadeModule fade;
V1InTheBoxSettings settings;

void table(uint32_t frequency, Band band = BAND_K) {
    parser.setAlerts({AlertData::create(band, DIR_FRONT, 4, 0, frequency, true, true)});
}

void display(bool softMuted, uint32_t ingress = 0) {
    auto& observation = parser.displayOnObservationValue;
    observation.available = true;
    ++observation.revision;
    observation.ingressSequence = ingress ? ingress : ble.noteV1NotificationIngress();
    parser.state.softMuted = softMuted;
    // Deliberately leave the debounced icon unchanged. Audio confirmation
    // must consume isSoft rather than DisplayState::muted.
}

void apply() {
    module.setInTheBoxSettings(settings, ble.sessionGeneration());
}

void tick(uint32_t now) {
    mockMillis = now;
    module.processInTheBox(now);
}

void confirmedOutsideMute() {
    settings.bands[2].muteOutside = true;
    apply();
    table(24040);
    tick(100);
    TEST_ASSERT_EQUAL_INT(1, ble.setMuteCalls);
    TEST_ASSERT_TRUE(ble.lastMuteValue);
    display(true);
    tick(110);
    TEST_ASSERT_TRUE(module.inTheBoxMuteActive());
    TEST_ASSERT_EQUAL_INT(1, ble.setMuteCalls);
}

void confirmRelease(uint32_t now) {
    display(false);
    tick(now);
    TEST_ASSERT_FALSE(module.inTheBoxMuteActive());
}
} // namespace

void setUp() {
    ble.reset();
    parser.reset();
    settings = V1InTheBoxSettings{};
    speedMute = SpeedMuteModule{};
    fade = VolumeFadeModule{};
    // Reconstruct too: suspension is caller-owned, not an alert-session flag.
    module = QuietCoordinatorModule{};
    mockMillis = 0;
    mockMicros = 0;
    ble.setConnected(true);
    module.begin(&ble, &parser);
    display(false);
    apply();
}
void tearDown() {}

void test_default_and_missing_display_or_table_do_not_send_mute() {
    table(24040);
    tick(100);
    table(24150);
    display(true);
    tick(200);
    TEST_ASSERT_EQUAL_INT(0, ble.setMuteCalls);
    settings.bands[2].muteOutside = true;
    settings.bands[2].unmuteInside = true;
    apply();
    parser.displayOnObservationValue.available = false;
    tick(300);
    TEST_ASSERT_EQUAL_INT(0, ble.setMuteCalls);
    display(false);
    table(24040);
    parser.freshAlertTable = false;
    tick(400);
    TEST_ASSERT_EQUAL_INT(0, ble.setMuteCalls);
}

void test_busy_and_failed_sends_retry_without_claiming_unsent_mute() {
    settings.bands[2].muteOutside = true;
    apply();
    table(24040);
    ble.nextMuteSendResult = SendResult::NOT_YET;
    tick(100);
    TEST_ASSERT_EQUAL_INT(1, ble.setMuteCalls);
    TEST_ASSERT_FALSE(module.inTheBoxMuteActive());
    tick(199);
    TEST_ASSERT_EQUAL_INT(1, ble.setMuteCalls);
    ble.nextMuteSendResult = SendResult::FAILED;
    tick(200);
    TEST_ASSERT_EQUAL_INT(2, ble.setMuteCalls);
    TEST_ASSERT_FALSE(module.inTheBoxMuteActive());
    tick(300);
    TEST_ASSERT_EQUAL_INT(3, ble.setMuteCalls);
    TEST_ASSERT_TRUE(module.inTheBoxMuteActive());
    tick(399);
    TEST_ASSERT_EQUAL_INT(3, ble.setMuteCalls);
    tick(400);
    TEST_ASSERT_EQUAL_INT(4, ble.setMuteCalls);
    display(true);
    tick(410);
    tick(600);
    TEST_ASSERT_EQUAL_INT(4, ble.setMuteCalls);
}

void test_queued_precommand_display_cannot_confirm_but_fresh_soft_bit_can() {
    settings.bands[2].muteOutside = true;
    apply();
    table(24040);
    ble.v1NotificationIngressSequenceValue = 10;
    tick(100);
    display(true, 10); // Parsed later, admitted before the write.
    tick(200);
    TEST_ASSERT_EQUAL_INT(2, ble.setMuteCalls); // Still needs convergence.
    TEST_ASSERT_FALSE(parser.state.muted);
    display(true); // New ingress 11, actual audio flag only.
    tick(210);
    tick(400);
    TEST_ASSERT_EQUAL_INT(2, ble.setMuteCalls);
    TEST_ASSERT_TRUE(module.inTheBoxMuteActive());
}

void test_inside_arrival_reverses_sent_mute_before_retry_interval() {
    settings.bands[2].muteOutside = true;
    apply();
    table(24040);
    tick(100);
    table(24150);
    tick(101); // The original mute has not echoed yet.
    TEST_ASSERT_EQUAL_INT(2, ble.setMuteCalls);
    TEST_ASSERT_FALSE(ble.lastMuteValue);
    confirmRelease(102);
    tick(400);
    TEST_ASSERT_EQUAL_INT(2, ble.setMuteCalls);
}

void test_inside_laser_clear_and_stale_each_release_only_owned_mute() {
    for (int reason = 0; reason < 4; ++reason) {
        setUp();
        confirmedOutsideMute();
        if (reason == 0) table(24150);
        if (reason == 1) parser.state.activeBands = BAND_LASER;
        if (reason == 2) parser.setAlerts({});
        if (reason == 3) parser.freshAlertTable = false;
        tick(120);
        TEST_ASSERT_EQUAL_INT(2, ble.setMuteCalls);
        TEST_ASSERT_FALSE(ble.lastMuteValue);
        confirmRelease(130);
        tick(300);
        TEST_ASSERT_EQUAL_INT(2, ble.setMuteCalls);
    }
}

void test_preexisting_manual_mute_is_not_owned_or_released_on_clear() {
    settings.bands[2].muteOutside = true;
    apply();
    display(true);
    table(24040);
    tick(100);
    TEST_ASSERT_FALSE(module.inTheBoxMuteActive());
    parser.setAlerts({});
    tick(200);
    TEST_ASSERT_EQUAL_INT(0, ble.setMuteCalls);
}

void test_obsolete_unsent_mute_and_inside_unmute_are_cancelled() {
    settings.bands[2].muteOutside = true;
    apply();
    table(24040);
    ble.nextMuteSendResult = SendResult::NOT_YET;
    tick(100);
    table(24150);
    tick(200);
    TEST_ASSERT_EQUAL_INT(1, ble.setMuteCalls);

    settings.bands[2].muteOutside = false;
    settings.bands[2].unmuteInside = true;
    apply();
    display(true);
    ble.nextMuteSendResult = SendResult::NOT_YET;
    tick(300);
    TEST_ASSERT_EQUAL_INT(2, ble.setMuteCalls);
    TEST_ASSERT_FALSE(ble.lastMuteValue);
    table(24040);
    tick(400);
    TEST_ASSERT_EQUAL_INT(2, ble.setMuteCalls);
}

void test_disconnected_or_new_session_drops_old_policy_and_pending_commands() {
    confirmedOutsideMute();
    ble.setConnected(false);
    tick(120);
    TEST_ASSERT_FALSE(module.inTheBoxMuteActive());
    ble.setConnected(true);
    ble.setSessionGeneration(2);
    display(false);
    tick(200);
    TEST_ASSERT_EQUAL_INT(1, ble.setMuteCalls);
    TEST_ASSERT_FALSE(module.inTheBoxSettings().bands[2].muteOutside);
    apply(); // Runtime explicitly loads the new address/session policy.
    tick(210);
    TEST_ASSERT_EQUAL_INT(2, ble.setMuteCalls);
    ble.setSessionGeneration(3); // Session replacement without an observed gap.
    tick(220);
    TEST_ASSERT_FALSE(module.inTheBoxMuteActive());
    TEST_ASSERT_FALSE(module.inTheBoxSettings().bands[2].muteOutside);
    TEST_ASSERT_EQUAL_INT(2, ble.setMuteCalls);
}

void test_proxy_handoff_cancels_ownership_without_delayed_command() {
    confirmedOutsideMute();
    ble.setProxyConnected(true);
    tick(120);
    TEST_ASSERT_FALSE(module.inTheBoxMuteActive());
    table(24150);
    tick(200);
    TEST_ASSERT_EQUAL_INT(1, ble.setMuteCalls);
    ble.setProxyConnected(false);
    display(false);
    table(24040);
    tick(300);
    TEST_ASSERT_EQUAL_INT(1, ble.setMuteCalls);
    parser.setAlerts({});
    tick(310);
    table(24040);
    tick(320);
    TEST_ASSERT_EQUAL_INT(2, ble.setMuteCalls);
}

void test_applying_default_policy_releases_old_owned_mute() {
    confirmedOutsideMute();
    settings = V1InTheBoxSettings{};
    apply();
    tick(120);
    TEST_ASSERT_EQUAL_INT(2, ble.setMuteCalls);
    TEST_ASSERT_FALSE(ble.lastMuteValue);
    confirmRelease(130);
    tick(400);
    TEST_ASSERT_EQUAL_INT(2, ble.setMuteCalls);
}

void test_suspension_releases_owned_mute_and_blocks_new_actions() {
    confirmedOutsideMute();
    module.setInTheBoxSuspended(true);
    tick(120);
    TEST_ASSERT_EQUAL_INT(2, ble.setMuteCalls);
    TEST_ASSERT_FALSE(ble.lastMuteValue);
    confirmRelease(130);
    tick(300);
    TEST_ASSERT_EQUAL_INT(2, ble.setMuteCalls);
    module.setInTheBoxSuspended(false);
    tick(310);
    TEST_ASSERT_EQUAL_INT(3, ble.setMuteCalls);
    TEST_ASSERT_TRUE(ble.lastMuteValue);
}

void test_local_manual_unmute_wins_until_current_alerts_clear() {
    confirmedOutsideMute();
    TEST_ASSERT_TRUE(module.sendMute(QuietOwner::TapGesture, false));
    display(false);
    tick(120);
    tick(300);
    TEST_ASSERT_EQUAL_INT(2, ble.setMuteCalls);
    TEST_ASSERT_FALSE(module.inTheBoxMuteActive());
    parser.setAlerts({});
    tick(310);
    table(24040);
    tick(320);
    TEST_ASSERT_EQUAL_INT(3, ble.setMuteCalls);
    TEST_ASSERT_TRUE(ble.lastMuteValue);
}

void test_later_local_manual_mute_survives_owned_condition_ending() {
    confirmedOutsideMute();
    TEST_ASSERT_TRUE(module.sendMute(QuietOwner::TapGesture, true));
    table(24150);
    tick(120);
    parser.setAlerts({});
    tick(300);
    TEST_ASSERT_EQUAL_INT(2, ble.setMuteCalls);
    TEST_ASSERT_TRUE(ble.lastMuteValue);
    TEST_ASSERT_FALSE(module.inTheBoxMuteActive());
}

void test_detector_unmute_after_confirmation_is_not_automatically_remuted() {
    confirmedOutsideMute();
    display(false);
    tick(120);
    tick(300);
    TEST_ASSERT_EQUAL_INT(1, ble.setMuteCalls);
    TEST_ASSERT_FALSE(module.inTheBoxMuteActive());
    parser.setAlerts({});
    tick(310);
    table(24040);
    tick(320);
    TEST_ASSERT_EQUAL_INT(2, ble.setMuteCalls);
}

void test_inside_option_unmutes_once_and_respects_later_manual_mute_and_jitter() {
    settings.bands[2].unmuteInside = true;
    settings.bands[3].unmuteInside = true;
    apply();
    display(true);
    table(24150);
    tick(100);
    TEST_ASSERT_EQUAL_INT(1, ble.setMuteCalls);
    TEST_ASSERT_FALSE(ble.lastMuteValue);
    display(false);
    tick(110);
    TEST_ASSERT_TRUE(module.sendMute(QuietOwner::TapGesture, true));
    display(true);
    table(24153);
    tick(200);
    table(24149);
    tick(300);
    table(0); // Temporarily undecodable data cannot invent a new encounter.
    tick(310);
    table(24150);
    tick(320);
    TEST_ASSERT_EQUAL_INT(2, ble.setMuteCalls);
    table(34700, BAND_KA); // Explicit option allows a distinct new threat.
    tick(400);
    TEST_ASSERT_EQUAL_INT(3, ble.setMuteCalls);
    TEST_ASSERT_FALSE(ble.lastMuteValue);
}

void test_clear_and_same_frequency_reappearance_between_ticks_is_a_new_encounter() {
    settings.bands[2].unmuteInside = true;
    apply();
    display(true);
    table(24150);
    tick(100);
    display(false);
    tick(110);
    TEST_ASSERT_TRUE(module.sendMute(QuietOwner::TapGesture, true));
    display(true);
    TEST_ASSERT_EQUAL_INT(2, ble.setMuteCalls);
    // Two parser publications in a single queue drain; the final table looks
    // identical, but alertLifetime records the intervening clear/new episode.
    parser.setAlerts({});
    table(24150);
    tick(200);
    TEST_ASSERT_EQUAL_INT(3, ble.setMuteCalls);
    TEST_ASSERT_FALSE(ble.lastMuteValue);
}

void test_clear_and_same_outside_frequency_between_ticks_rearms_after_manual_override() {
    confirmedOutsideMute();
    TEST_ASSERT_TRUE(module.sendMute(QuietOwner::TapGesture, false));
    display(false);
    tick(120);
    TEST_ASSERT_EQUAL_INT(2, ble.setMuteCalls);
    parser.setAlerts({});
    table(24040);
    tick(200);
    TEST_ASSERT_EQUAL_INT(3, ble.setMuteCalls);
    TEST_ASSERT_TRUE(ble.lastMuteValue);
}

void test_new_episode_detector_unmute_does_not_become_old_episode_manual_override() {
    confirmedOutsideMute();
    parser.setAlerts({});
    table(24040);
    display(false); // The prior alert/mute ended before this new episode.
    tick(200);
    TEST_ASSERT_EQUAL_INT(2, ble.setMuteCalls);
    TEST_ASSERT_TRUE(ble.lastMuteValue);
    TEST_ASSERT_TRUE(module.inTheBoxMuteActive());
}

void test_box_audio_actions_preserve_speed_volume_and_never_bypass_real_mute() {
    parser.setMainVolume(6);
    parser.setMuteVolume(2);
    speedMute.begin(true, 25, 3, 0);
    speedMute.state_.muteActive = true;
    TEST_ASSERT_TRUE(module.processSpeedVolume(50, speedMute, &fade));
    TEST_ASSERT_EQUAL_UINT8(0, ble.lastVolume);
    parser.setMainVolume(0);
    confirmedOutsideMute();
    VoiceContext voice;
    voice.isSoftMuted = true;
    voice.mainVolume = 0;
    module.applyVoicePresentation(voice, &speedMute, true, BAND_K);
    TEST_ASSERT_TRUE(voice.isSuppressed);
    TEST_ASSERT_TRUE(voice.isSoftMuted);
    TEST_ASSERT_EQUAL_UINT8(0, voice.mainVolume);
    table(24150);
    tick(120);
    TEST_ASSERT_FALSE(ble.lastMuteValue);
    TEST_ASSERT_EQUAL_INT(1, ble.setVolumeCalls);
    TEST_ASSERT_EQUAL_UINT8(0, ble.lastVolume);
    TEST_ASSERT_TRUE(module.getPresentationState().speedVolZeroActive);
    module.applyVoicePresentation(voice, &speedMute, true, BAND_K);
    TEST_ASSERT_TRUE(voice.isSuppressed);
    TEST_ASSERT_TRUE(voice.isSoftMuted);
    TEST_ASSERT_FALSE(module.getPresentationState().voiceAllowVolZeroBypass);
    speedMute.state_.muteActive = false;
    TEST_ASSERT_TRUE(module.processSpeedVolume(200, speedMute, &fade));
    TEST_ASSERT_EQUAL_UINT8(6, ble.lastVolume);
    TEST_ASSERT_EQUAL_UINT8(2, ble.lastMuteVolume);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_default_and_missing_display_or_table_do_not_send_mute);
    RUN_TEST(test_busy_and_failed_sends_retry_without_claiming_unsent_mute);
    RUN_TEST(test_queued_precommand_display_cannot_confirm_but_fresh_soft_bit_can);
    RUN_TEST(test_inside_arrival_reverses_sent_mute_before_retry_interval);
    RUN_TEST(test_inside_laser_clear_and_stale_each_release_only_owned_mute);
    RUN_TEST(test_preexisting_manual_mute_is_not_owned_or_released_on_clear);
    RUN_TEST(test_obsolete_unsent_mute_and_inside_unmute_are_cancelled);
    RUN_TEST(test_disconnected_or_new_session_drops_old_policy_and_pending_commands);
    RUN_TEST(test_proxy_handoff_cancels_ownership_without_delayed_command);
    RUN_TEST(test_applying_default_policy_releases_old_owned_mute);
    RUN_TEST(test_suspension_releases_owned_mute_and_blocks_new_actions);
    RUN_TEST(test_local_manual_unmute_wins_until_current_alerts_clear);
    RUN_TEST(test_later_local_manual_mute_survives_owned_condition_ending);
    RUN_TEST(test_detector_unmute_after_confirmation_is_not_automatically_remuted);
    RUN_TEST(test_inside_option_unmutes_once_and_respects_later_manual_mute_and_jitter);
    RUN_TEST(test_clear_and_same_frequency_reappearance_between_ticks_is_a_new_encounter);
    RUN_TEST(test_clear_and_same_outside_frequency_between_ticks_rearms_after_manual_override);
    RUN_TEST(test_new_episode_detector_unmute_does_not_become_old_episode_manual_override);
    RUN_TEST(test_box_audio_actions_preserve_speed_volume_and_never_bypass_real_mute);
    return UNITY_END();
}

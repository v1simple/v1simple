// Playback retry/suppression policy is independent of drawing.
#include <unity.h>
#include "../mocks/Arduino.h"
#include "../mocks/ble_client.h"
#include "../mocks/packet_parser.h"
#include "../mocks/settings.h"
#include "../../src/audio_beep.h"
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
SettingsManager settings;
#include "../../src/modules/speed_mute/speed_mute_module.cpp"
#include "../../src/modules/quiet/quiet_coordinator_module.cpp"
#include "../../src/modules/voice/voice_module.cpp"
#include "../../src/modules/voice/voice_playback_coordinator.cpp"
static AudioPlaybackResult g_voicePlaybackResult = AudioPlaybackResult::Accepted;
static int g_voicePlaybackAttempts = 0;
static int g_escalationPlaybackAttempts = 0;
static uint16_t g_lastVoiceFrequency = 0;
static AlertBand g_lastVoiceBand = AlertBand::KA;

void play_frequency_voice(AlertBand,
                          uint16_t,
                          AlertDirection,
                          VoiceAlertMode,
                          bool,
                          uint8_t) {}
void play_direction_only(AlertDirection, uint8_t) {}
void play_threat_escalation(AlertBand, uint16_t, AlertDirection, uint8_t, uint8_t, uint8_t, uint8_t) {}
AudioPlaybackResult try_play_frequency_voice(AlertBand band, uint16_t freq, AlertDirection, VoiceAlertMode, bool,
                                             uint8_t, AudioPlaybackReceipt* receipt) {
    if (receipt) receipt->state.store(AudioPlaybackReceipt::State::Started);
    ++g_voicePlaybackAttempts;
    g_lastVoiceBand = band;
    g_lastVoiceFrequency = freq;
    return g_voicePlaybackResult;
}
AudioPlaybackResult try_play_direction_only(AlertDirection, uint8_t, AudioPlaybackReceipt* receipt) {
    if (receipt) receipt->state.store(AudioPlaybackReceipt::State::Started);
    ++g_voicePlaybackAttempts;
    return g_voicePlaybackResult;
}
AudioPlaybackResult try_play_threat_escalation(AlertBand band, uint16_t freq, AlertDirection, uint8_t, uint8_t, uint8_t,
                                               uint8_t, VoiceAlertMode, bool, bool, AudioPlaybackReceipt* receipt) {
    if (receipt) receipt->state.store(AudioPlaybackReceipt::State::Started);
    ++g_voicePlaybackAttempts;
    ++g_escalationPlaybackAttempts;
    g_lastVoiceBand = band;
    g_lastVoiceFrequency = freq;
    return g_voicePlaybackResult;
}

static PacketParser parser;
static V1BLEClient ble;
static QuietCoordinatorModule quiet;
static SpeedMuteModule speedMute;
static VoiceModule voice;
static VoicePlaybackCoordinator coordinator{voice, parser, ble, &quiet, &speedMute};
static V1VoiceObservation observation;
// Selection inputs are literal bounded values in this suite. Parser state is
// retained only for the coordinator's independent pending-cancellation inputs.
static void setAlerts(std::initializer_list<AlertData> rows) {
    parser.setAlerts(rows);
    observation.count = 0;
    for (const auto& row : rows) observation.alerts[observation.count++] = row;
    observation.hasPriority = rows.size() != 0;
    observation.priority = observation.hasPriority ? *rows.begin() : AlertData{};
    observation.episode = parser.alertLifetime();
}
static void admit(uint32_t nowMs) {
    observation.muted = parser.state.muted;
    observation.softMuted = parser.state.softMuted;
    observation.mainVolume = parser.state.mainVolume;
    observation.proxyConnected = ble.isProxyClientConnected();
    const auto& options = settings.get();
    coordinator.onParsedPresentation(observation,
        {options.voiceAlertMode, options.voiceDirectionEnabled, options.announceBogeyCount}, nowMs);
}
static AlertData makeKAlert(uint16_t freq = 24148) {
    AlertData a{};
    a.isValid = true;
    a.isPriority = true;
    a.band = BAND_K;
    a.frequency = freq;
    a.direction = DIR_FRONT;
    a.frontStrength = 5;
    return a;
}

static AlertData makeKaAlert(uint16_t freq = 34520) {
    AlertData a{};
    a.isValid = true;
    a.band = BAND_KA;
    a.frequency = freq;
    a.direction = DIR_REAR;
    a.rearStrength = 4;
    return a;
}

void setUp() {
    mockMillis = mockMicros = 0;
    parser.reset();
    observation = {};
    ble.reset();
    speedMute = SpeedMuteModule{};
    voice = VoiceModule{};
    settings = SettingsManager{};
    g_voicePlaybackResult = AudioPlaybackResult::Accepted;
    g_voicePlaybackAttempts = g_escalationPlaybackAttempts = 0;
    g_lastVoiceFrequency = 0;
    g_lastVoiceBand = AlertBand::KA;
    quiet.begin(&ble, &parser);
    voice.begin(&settings);
    coordinator.begin(parser.alertLifetime());
}
void tearDown() {}
void test_busy_voice_keeps_exact_action_until_playback_accepts() {
    parser.setMainVolume(5);
    setAlerts({makeKAlert(24148)});
    g_voicePlaybackResult = AudioPlaybackResult::Busy;

    admit(1000);
    TEST_ASSERT_EQUAL_INT(1, g_voicePlaybackAttempts);
    TEST_ASSERT_EQUAL_UINT16(24148, g_lastVoiceFrequency);

    admit(1050);
    TEST_ASSERT_EQUAL_INT(1, g_voicePlaybackAttempts);

    g_voicePlaybackResult = AudioPlaybackResult::Accepted;
    admit(1100);
    TEST_ASSERT_EQUAL_INT(2, g_voicePlaybackAttempts);
    TEST_ASSERT_EQUAL_UINT16(24148, g_lastVoiceFrequency);

    admit(1150);
    TEST_ASSERT_EQUAL_INT(2, g_voicePlaybackAttempts);
}

void test_voice_without_retry_announces_across_signed_uptime_boundary() {
    for (uint32_t nowMs : {0x7FFFFFFFu, 0x80000000u, 0xFFFFFFF0u}) {
        setUp();
        parser.setMainVolume(5);
        setAlerts({makeKAlert()});
        admit(nowMs);
        TEST_ASSERT_TRUE(parser.hasAlerts());
        TEST_ASSERT_EQUAL_INT(1, g_voicePlaybackAttempts);

        // Clearing an encounter must leave the next one eligible at any uptime.
        setAlerts({});
        admit(nowMs + 1u);
        setAlerts({makeKAlert(24250)});
        admit(nowMs + 2u);
        TEST_ASSERT_EQUAL_INT(2, g_voicePlaybackAttempts);
        TEST_ASSERT_EQUAL_UINT16(24250, g_lastVoiceFrequency);
    }
}

void test_busy_voice_retry_waits_across_wrap_including_zero_deadline() {
    for (uint32_t startedAt : {UINT32_MAX - 49u, UINT32_MAX - 99u}) {
        setUp();
        parser.setMainVolume(5);
        setAlerts({makeKAlert()});
        g_voicePlaybackResult = AudioPlaybackResult::Busy;
        admit(startedAt);
        TEST_ASSERT_EQUAL_INT(1, g_voicePlaybackAttempts);

        g_voicePlaybackResult = AudioPlaybackResult::Accepted;
        admit(startedAt + 99u);
        TEST_ASSERT_EQUAL_INT(1, g_voicePlaybackAttempts);
        admit(startedAt + 100u);
        TEST_ASSERT_EQUAL_INT(2, g_voicePlaybackAttempts);
        TEST_ASSERT_EQUAL_UINT16(24148, g_lastVoiceFrequency);
        admit(startedAt + 101u);
        TEST_ASSERT_EQUAL_INT(2, g_voicePlaybackAttempts);
    }
}

void test_unavailable_voice_retry_waits_across_wrap_with_zero_deadline() {
    parser.setMainVolume(5);
    setAlerts({makeKAlert()});
    const uint32_t startedAt = UINT32_MAX - 999u;
    g_voicePlaybackResult = AudioPlaybackResult::Unavailable;
    admit(startedAt);
    TEST_ASSERT_EQUAL_INT(1, g_voicePlaybackAttempts);

    g_voicePlaybackResult = AudioPlaybackResult::Accepted;
    admit(startedAt + 999u);
    TEST_ASSERT_EQUAL_INT(1, g_voicePlaybackAttempts);
    admit(0u);
    TEST_ASSERT_EQUAL_INT(2, g_voicePlaybackAttempts);
    admit(1u);
    TEST_ASSERT_EQUAL_INT(2, g_voicePlaybackAttempts);
}

void test_busy_voice_drops_stale_pending_action_when_alert_changes() {
    parser.setMainVolume(5);
    setAlerts({makeKAlert(24148)});
    g_voicePlaybackResult = AudioPlaybackResult::Busy;
    admit(1000);
    TEST_ASSERT_EQUAL_UINT16(24148, g_lastVoiceFrequency);

    setAlerts({makeKAlert(24250)});
    g_voicePlaybackResult = AudioPlaybackResult::Accepted;
    admit(1050);

    TEST_ASSERT_EQUAL_INT(2, g_voicePlaybackAttempts);
    TEST_ASSERT_EQUAL_UINT16(24250, g_lastVoiceFrequency);
}

enum class RetryGate { SoftMute, VolumeZero, SpeedMute, Proxy, Disabled };

static void setRetryGate(RetryGate gate, bool active) {
    switch (gate) {
    case RetryGate::SoftMute: parser.state.softMuted = active; break;
    case RetryGate::VolumeZero: parser.setMainVolume(active ? 0 : 5); break;
    case RetryGate::SpeedMute:
        if (active) {
            speedMute.begin(true, 25, 3, 0, true);
            speedMute.update(0.0f, true, 2100);
        } else {
            speedMute.update(0.0f, false, 2151); // Lost speed fails open immediately.
        }
        break;
    case RetryGate::Proxy: ble.setProxyConnected(active); break;
    case RetryGate::Disabled:
        settings.settings.voiceAlertMode = active ? VOICE_MODE_DISABLED : VOICE_MODE_BAND_FREQ;
        break;
    }
}

static void assertBusyRetryHonorsCurrentGate(RetryGate gate) {
    settings.settings.muteVoiceIfVolZero = true;
    parser.setMainVolume(5);
    setAlerts({makeKAlert()});
    g_voicePlaybackResult = AudioPlaybackResult::Busy;
    admit(1000);
    TEST_ASSERT_EQUAL_INT(1, g_voicePlaybackAttempts);

    setRetryGate(gate, true);
    g_voicePlaybackResult = AudioPlaybackResult::Accepted;
    admit(2100);
    admit(2150);
    TEST_ASSERT_EQUAL_INT(1, g_voicePlaybackAttempts);

    setRetryGate(gate, false);
    admit(2151); // Suppressed calls must not postpone eligibility.
    TEST_ASSERT_EQUAL_INT(2, g_voicePlaybackAttempts);
    TEST_ASSERT_EQUAL_UINT16(24148, g_lastVoiceFrequency);
    admit(2200);
    TEST_ASSERT_EQUAL_INT(2, g_voicePlaybackAttempts);
}

void test_busy_voice_honors_new_soft_mute_then_recovers() {
    assertBusyRetryHonorsCurrentGate(RetryGate::SoftMute);
}
void test_busy_voice_honors_new_volume_zero_then_recovers() {
    assertBusyRetryHonorsCurrentGate(RetryGate::VolumeZero);
}
void test_busy_voice_honors_new_speed_suppression_then_recovers() {
    assertBusyRetryHonorsCurrentGate(RetryGate::SpeedMute);
}
void test_busy_voice_honors_new_proxy_then_recovers() {
    assertBusyRetryHonorsCurrentGate(RetryGate::Proxy);
}
void test_busy_voice_honors_disabled_mode_then_recovers() {
    assertBusyRetryHonorsCurrentGate(RetryGate::Disabled);
}
void test_busy_k_voice_is_replaced_by_current_photo_presentation() {
    parser.setMainVolume(5);
    AlertData alert = makeKAlert();
    setAlerts({alert});
    g_voicePlaybackResult = AudioPlaybackResult::Busy;
    admit(1000);
    TEST_ASSERT_EQUAL_INT(1, g_voicePlaybackAttempts);
    TEST_ASSERT_EQUAL(AlertBand::K, g_lastVoiceBand);

    alert.photoType = 1;
    setAlerts({alert});
    g_voicePlaybackResult = AudioPlaybackResult::Accepted;
    admit(1100);
    TEST_ASSERT_EQUAL_INT(2, g_voicePlaybackAttempts);
    TEST_ASSERT_EQUAL(AlertBand::PHOTO, g_lastVoiceBand);
    admit(1150);
    TEST_ASSERT_EQUAL_INT(2, g_voicePlaybackAttempts);
}

void test_busy_voice_visible_mute_and_optional_volume_zero_remain_allowed() {
    for (bool volumeZero : {false, true}) {
        setUp();
        parser.setMainVolume(5);
        setAlerts({makeKAlert()});
        g_voicePlaybackResult = AudioPlaybackResult::Busy;
        admit(1000);
        TEST_ASSERT_EQUAL_INT(1, g_voicePlaybackAttempts);
        if (volumeZero) {
            settings.settings.muteVoiceIfVolZero = false;
            parser.setMainVolume(0);
        } else {
            parser.state.muted = true;
        }
        g_voicePlaybackResult = AudioPlaybackResult::Accepted;
        admit(1100);
        TEST_ASSERT_EQUAL_INT(2, g_voicePlaybackAttempts);
    }
}

void test_busy_secondary_survives_mute_without_consuming_unheard_dedup() {
    settings.settings.announceSecondaryAlerts = true;
    settings.settings.secondaryK = true;
    parser.setMainVolume(5);
    AlertData priority = makeKaAlert();
    priority.isPriority = true;
    AlertData secondary = makeKAlert();
    secondary.isPriority = false;
    setAlerts({priority, secondary});
    admit(1000);
    TEST_ASSERT_EQUAL_UINT16(priority.frequency, g_lastVoiceFrequency);
    g_voicePlaybackResult = AudioPlaybackResult::Busy;
    admit(3000);
    TEST_ASSERT_EQUAL_INT(2, g_voicePlaybackAttempts);
    TEST_ASSERT_EQUAL_UINT16(secondary.frequency, g_lastVoiceFrequency);
    parser.state.softMuted = true;
    g_voicePlaybackResult = AudioPlaybackResult::Accepted;
    admit(3100);
    TEST_ASSERT_EQUAL_INT(2, g_voicePlaybackAttempts);
    parser.state.softMuted = false;
    admit(3101);
    TEST_ASSERT_EQUAL_INT(3, g_voicePlaybackAttempts);
    TEST_ASSERT_EQUAL_UINT16(secondary.frequency, g_lastVoiceFrequency);
    admit(3150);
    TEST_ASSERT_EQUAL_INT(3, g_voicePlaybackAttempts);
}

void test_blocked_pending_voice_does_not_survive_source_replacement_or_clear() {
    for (bool clear : {false, true}) {
        setUp();
        parser.setMainVolume(5);
        setAlerts({makeKAlert()});
        g_voicePlaybackResult = AudioPlaybackResult::Busy;
        admit(1000);
        parser.state.softMuted = true;
        if (clear) setAlerts({});
        else setAlerts({makeKAlert(24250)});
        g_voicePlaybackResult = AudioPlaybackResult::Accepted;
        admit(1100);
        TEST_ASSERT_EQUAL_INT(1, g_voicePlaybackAttempts);
        parser.state.softMuted = false;
        setAlerts({makeKAlert(24250)});
        admit(1101);
        TEST_ASSERT_EQUAL_INT(2, g_voicePlaybackAttempts);
        TEST_ASSERT_EQUAL_UINT16(24250, g_lastVoiceFrequency);
        admit(1200);
        TEST_ASSERT_EQUAL_INT(2, g_voicePlaybackAttempts);
    }
}

static void prepareBusySecondary(AlertData& priority, AlertData& secondary) {
    settings.settings.announceSecondaryAlerts = true;
    settings.settings.secondaryK = true;
    parser.setMainVolume(5);
    priority = makeKaAlert();
    priority.isPriority = true;
    secondary = makeKAlert(24125);
    secondary.isPriority = false;
    setAlerts({priority, secondary});
    admit(1000);
    TEST_ASSERT_EQUAL_UINT16(priority.frequency, g_lastVoiceFrequency);
    g_voicePlaybackResult = AudioPlaybackResult::Busy;
    admit(3000);
    TEST_ASSERT_EQUAL_INT(2, g_voicePlaybackAttempts);
    TEST_ASSERT_EQUAL_UINT16(24125, g_lastVoiceFrequency);
}

void test_busy_secondary_reclassified_as_photo_replaces_stale_k_action() {
    AlertData priority, secondary;
    prepareBusySecondary(priority, secondary);
    secondary.photoType = 1;
    setAlerts({priority, secondary});
    g_voicePlaybackResult = AudioPlaybackResult::Accepted;
    admit(3100);
    TEST_ASSERT_EQUAL_INT(3, g_voicePlaybackAttempts);
    TEST_ASSERT_EQUAL(AlertBand::PHOTO, g_lastVoiceBand);
    TEST_ASSERT_EQUAL_UINT16(24125, g_lastVoiceFrequency);
    admit(3150);
    TEST_ASSERT_EQUAL_INT(3, g_voicePlaybackAttempts);
}

void test_reclassified_photo_secondary_does_not_block_later_ordinary_k() {
    AlertData priority, secondary;
    prepareBusySecondary(priority, secondary);
    secondary.photoType = 1;
    AlertData later = makeKAlert(24200);
    later.isPriority = false;
    setAlerts({priority, secondary, later});
    g_voicePlaybackResult = AudioPlaybackResult::Accepted;
    admit(3100);
    TEST_ASSERT_EQUAL_INT(3, g_voicePlaybackAttempts);
    TEST_ASSERT_EQUAL(AlertBand::PHOTO, g_lastVoiceBand);
    TEST_ASSERT_EQUAL_UINT16(24125, g_lastVoiceFrequency);
    admit(3150);
    TEST_ASSERT_EQUAL_INT(4, g_voicePlaybackAttempts);
    TEST_ASSERT_EQUAL(AlertBand::K, g_lastVoiceBand);
    TEST_ASSERT_EQUAL_UINT16(24200, g_lastVoiceFrequency);
}

void test_ordinary_busy_secondary_retains_its_retry_timing_and_frequency() {
    AlertData priority, secondary;
    prepareBusySecondary(priority, secondary);
    g_voicePlaybackResult = AudioPlaybackResult::Accepted;
    admit(3099);
    TEST_ASSERT_EQUAL_INT(2, g_voicePlaybackAttempts);
    admit(3100);
    TEST_ASSERT_EQUAL_INT(3, g_voicePlaybackAttempts);
    TEST_ASSERT_EQUAL_UINT16(24125, g_lastVoiceFrequency);
    admit(3150);
    TEST_ASSERT_EQUAL_INT(3, g_voicePlaybackAttempts);
}

void test_busy_secondary_rechecks_current_band_filter_before_retry() {
    AlertData priority, secondary;
    prepareBusySecondary(priority, secondary);
    settings.settings.secondaryK = false;
    g_voicePlaybackResult = AudioPlaybackResult::Accepted;
    admit(3100);
    TEST_ASSERT_EQUAL_INT(2, g_voicePlaybackAttempts);
    settings.settings.secondaryK = true;
    admit(3101);
    TEST_ASSERT_EQUAL_INT(3, g_voicePlaybackAttempts);
    TEST_ASSERT_EQUAL_UINT16(24125, g_lastVoiceFrequency);
    admit(3150);
    TEST_ASSERT_EQUAL_INT(3, g_voicePlaybackAttempts);
}

static void prepareBusyEscalation(AlertData& priority, AlertData& secondary) {
    settings.settings.announceSecondaryAlerts = true;
    settings.settings.secondaryK = true;
    parser.setMainVolume(5);
    priority = makeKaAlert();
    priority.isPriority = true;
    secondary = makeKAlert(24125);
    secondary.isPriority = false;
    secondary.frontStrength = 2;
    setAlerts({priority, secondary});
    admit(1000);
    admit(4000); // Accept the ordinary secondary first.
    TEST_ASSERT_EQUAL_INT(2, g_voicePlaybackAttempts);
    voice.testUpdateAlertHistory(BAND_K, 24125, 2, 4100);
    voice.testUpdateAlertHistory(BAND_K, 24125, 5, 4200);
    secondary.frontStrength = 5;
    setAlerts({priority, secondary});
    g_voicePlaybackResult = AudioPlaybackResult::Busy;
    admit(4800);
    TEST_ASSERT_EQUAL_INT(3, g_voicePlaybackAttempts);
    TEST_ASSERT_EQUAL_INT(1, g_escalationPlaybackAttempts);
    TEST_ASSERT_EQUAL_UINT16(24125, g_lastVoiceFrequency);
}

void test_busy_escalation_reclassified_as_photo_replaces_stale_action() {
    AlertData priority, secondary;
    prepareBusyEscalation(priority, secondary);
    secondary.photoType = 1;
    setAlerts({priority, secondary});
    g_voicePlaybackResult = AudioPlaybackResult::Accepted;
    admit(4900);
    TEST_ASSERT_EQUAL_INT(4, g_voicePlaybackAttempts);
    TEST_ASSERT_EQUAL_INT(1, g_escalationPlaybackAttempts);
    TEST_ASSERT_EQUAL(AlertBand::PHOTO, g_lastVoiceBand);
    secondary.photoType = 0;
    setAlerts({priority, secondary});
    admit(4901);
    TEST_ASSERT_EQUAL_INT(4, g_voicePlaybackAttempts);
    TEST_ASSERT_EQUAL_INT(1, g_escalationPlaybackAttempts);
    admit(5400); // The replacement announcement owns the normal 500 ms escalation cooldown.
    TEST_ASSERT_EQUAL_INT(5, g_voicePlaybackAttempts);
    TEST_ASSERT_EQUAL_INT(2, g_escalationPlaybackAttempts);
    TEST_ASSERT_EQUAL(AlertBand::K, g_lastVoiceBand);
    TEST_ASSERT_EQUAL_UINT16(24125, g_lastVoiceFrequency);
    admit(5450);
    TEST_ASSERT_EQUAL_INT(5, g_voicePlaybackAttempts);
}

void test_ordinary_busy_escalation_remains_retryable_until_accepted_once() {
    AlertData priority, secondary;
    prepareBusyEscalation(priority, secondary);
    g_voicePlaybackResult = AudioPlaybackResult::Accepted;
    admit(4899);
    TEST_ASSERT_EQUAL_INT(3, g_voicePlaybackAttempts);
    admit(4900);
    TEST_ASSERT_EQUAL_INT(4, g_voicePlaybackAttempts);
    TEST_ASSERT_EQUAL_INT(2, g_escalationPlaybackAttempts);
    TEST_ASSERT_EQUAL_UINT16(24125, g_lastVoiceFrequency);
    admit(5000);
    TEST_ASSERT_EQUAL_INT(4, g_voicePlaybackAttempts);
}

void test_busy_escalation_rechecks_sustained_strength_before_retry() {
    AlertData priority, secondary;
    prepareBusyEscalation(priority, secondary);
    secondary.frontStrength = 2;
    setAlerts({priority, secondary});
    g_voicePlaybackResult = AudioPlaybackResult::Accepted;
    admit(4900);
    TEST_ASSERT_EQUAL_INT(3, g_voicePlaybackAttempts);
    secondary.frontStrength = 5;
    setAlerts({priority, secondary});
    admit(5000);
    admit(5499);
    TEST_ASSERT_EQUAL_INT(3, g_voicePlaybackAttempts);
    admit(5500);
    TEST_ASSERT_EQUAL_INT(4, g_voicePlaybackAttempts);
    TEST_ASSERT_EQUAL_INT(2, g_escalationPlaybackAttempts);
    admit(5600);
    TEST_ASSERT_EQUAL_INT(4, g_voicePlaybackAttempts);
}

void test_unavailable_voice_is_not_retried_every_frame_or_committed() {
    parser.setMainVolume(5);
    setAlerts({makeKAlert(24148)});
    g_voicePlaybackResult = AudioPlaybackResult::Unavailable;
    admit(1000);
    TEST_ASSERT_EQUAL_INT(1, g_voicePlaybackAttempts);

    admit(1100);
    TEST_ASSERT_EQUAL_INT(1, g_voicePlaybackAttempts);

    g_voicePlaybackResult = AudioPlaybackResult::Accepted;
    admit(2000);
    TEST_ASSERT_EQUAL_INT(2, g_voicePlaybackAttempts);
    TEST_ASSERT_EQUAL_UINT16(24148, g_lastVoiceFrequency);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_busy_voice_keeps_exact_action_until_playback_accepts);
    RUN_TEST(test_voice_without_retry_announces_across_signed_uptime_boundary);
    RUN_TEST(test_busy_voice_retry_waits_across_wrap_including_zero_deadline);
    RUN_TEST(test_unavailable_voice_retry_waits_across_wrap_with_zero_deadline);
    RUN_TEST(test_busy_voice_drops_stale_pending_action_when_alert_changes);
    RUN_TEST(test_busy_voice_honors_new_soft_mute_then_recovers);
    RUN_TEST(test_busy_voice_honors_new_volume_zero_then_recovers);
    RUN_TEST(test_busy_voice_honors_new_speed_suppression_then_recovers);
    RUN_TEST(test_busy_voice_honors_new_proxy_then_recovers);
    RUN_TEST(test_busy_voice_honors_disabled_mode_then_recovers);
    RUN_TEST(test_busy_k_voice_is_replaced_by_current_photo_presentation);
    RUN_TEST(test_busy_voice_visible_mute_and_optional_volume_zero_remain_allowed);
    RUN_TEST(test_busy_secondary_survives_mute_without_consuming_unheard_dedup);
    RUN_TEST(test_blocked_pending_voice_does_not_survive_source_replacement_or_clear);
    RUN_TEST(test_busy_secondary_reclassified_as_photo_replaces_stale_k_action);
    RUN_TEST(test_reclassified_photo_secondary_does_not_block_later_ordinary_k);
    RUN_TEST(test_ordinary_busy_secondary_retains_its_retry_timing_and_frequency);
    RUN_TEST(test_busy_secondary_rechecks_current_band_filter_before_retry);
    RUN_TEST(test_busy_escalation_reclassified_as_photo_replaces_stale_action);
    RUN_TEST(test_ordinary_busy_escalation_remains_retryable_until_accepted_once);
    RUN_TEST(test_busy_escalation_rechecks_sustained_strength_before_retry);
    RUN_TEST(test_unavailable_voice_is_not_retried_every_frame_or_committed);
    return UNITY_END();
}

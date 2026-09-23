/**
 * VoiceModule decision-logic contract tests.
 *
 * Covers VoiceModule::process() early-exit guards, the ANNOUNCE_PRIORITY
 * happy path, the ANNOUNCE_DIRECTION case, and all static utilities.
 * Does not test audio playback (VoiceModule returns an action; caller executes it).
 *
 * Regression boundary: this suite pins voice action selection and
 * suppression. The separate playback worker remains outside this suite's
 * boundary.
 */

#include <unity.h>
#include <cstring>

#include "../mocks/ble_client.h"
#include "../mocks/settings.h"
#include "../../src/modules/voice/voice_module.h"
#include "../../src/modules/voice/voice_module.cpp"  // Pull implementation for UNIT_TEST

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

SettingsManager settings;
static VoiceModule voiceModule;
static V1BLEClient bleClient;

void setUp() {
    settings = SettingsManager();
    bleClient.reset();
    voiceModule = VoiceModule();
    voiceModule.begin(&settings, &bleClient);
    mockMillis = 10000;  // Start at 10 s so cooldown (2 s) is always pre-satisfied
}

void tearDown() {}

void test_make_alert_id_encodes_band_and_freq() {
    uint32_t id = VoiceModule::makeAlertId(BAND_KA, 34700);
    TEST_ASSERT_EQUAL_UINT32(((uint32_t)BAND_KA << 16) | 34700u, id);
}

void test_make_alert_id_different_bands_differ() {
    uint32_t ka = VoiceModule::makeAlertId(BAND_KA, 34700);
    uint32_t k  = VoiceModule::makeAlertId(BAND_K,  34700);
    TEST_ASSERT_NOT_EQUAL(ka, k);
}

void test_make_alert_id_different_freqs_differ() {
    uint32_t a = VoiceModule::makeAlertId(BAND_KA, 34700);
    uint32_t b = VoiceModule::makeAlertId(BAND_KA, 35000);
    TEST_ASSERT_NOT_EQUAL(a, b);
}

void test_make_alert_id_distinguishes_photo_presentation() {
    uint32_t k = VoiceModule::makeAlertId(BAND_K, 24125, false);
    uint32_t photo = VoiceModule::makeAlertId(BAND_K, 24125, true);
    TEST_ASSERT_NOT_EQUAL(k, photo);
}

void test_to_audio_direction_front() {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertDirection::AHEAD),
                          static_cast<int>(VoiceModule::toAudioDirection(DIR_FRONT)));
}

void test_to_audio_direction_rear() {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertDirection::BEHIND),
                          static_cast<int>(VoiceModule::toAudioDirection(DIR_REAR)));
}

void test_to_audio_direction_side_is_default() {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertDirection::SIDE),
                          static_cast<int>(VoiceModule::toAudioDirection(DIR_SIDE)));
}

void test_to_audio_direction_none_is_side() {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertDirection::SIDE),
                          static_cast<int>(VoiceModule::toAudioDirection(DIR_NONE)));
}

void test_is_band_enabled_for_secondary_ka_follows_setting() {
    const V1Settings& s = settings.get();
    TEST_ASSERT_TRUE(VoiceModule::isBandEnabledForSecondary(BAND_KA, s));

    settings.settings.secondaryKa = false;
    TEST_ASSERT_FALSE(VoiceModule::isBandEnabledForSecondary(BAND_KA, settings.get()));
    settings.settings.secondaryKa = true;  // restore
}

void test_is_band_enabled_for_secondary_laser_follows_setting() {
    const V1Settings& s = settings.get();
    TEST_ASSERT_TRUE(VoiceModule::isBandEnabledForSecondary(BAND_LASER, s));

    settings.settings.secondaryLaser = false;
    TEST_ASSERT_FALSE(VoiceModule::isBandEnabledForSecondary(BAND_LASER, settings.get()));
    settings.settings.secondaryLaser = true;
}

void test_is_band_enabled_for_secondary_unknown_band_false() {
    const V1Settings& s = settings.get();
    TEST_ASSERT_FALSE(VoiceModule::isBandEnabledForSecondary(BAND_NONE, s));
}

void test_get_alert_bars_front_direction_uses_front_strength() {
    AlertData a = AlertData::create(BAND_KA, DIR_FRONT, 6, 2, 34700);
    TEST_ASSERT_EQUAL_UINT8(6, VoiceModule::getAlertBars(a));
}

void test_get_alert_bars_rear_direction_uses_rear_strength() {
    AlertData a = AlertData::create(BAND_KA, DIR_REAR, 2, 7, 34700);
    TEST_ASSERT_EQUAL_UINT8(7, VoiceModule::getAlertBars(a));
}

void test_get_alert_bars_no_direction_uses_max() {
    AlertData a = AlertData::create(BAND_KA, DIR_NONE, 3, 5, 34700);
    TEST_ASSERT_EQUAL_UINT8(5, VoiceModule::getAlertBars(a));
}

void test_process_returns_none_when_no_priority() {
    VoiceContext ctx;
    ctx.now = mockMillis;
    ctx.mainVolume = 5;   // pass vol-zero guard so we reach the no-priority check
    VoiceAction action = voiceModule.process(ctx);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(VoiceAction::Type::NONE),
                          static_cast<int>(action.type));
}

void test_process_returns_none_when_voice_disabled() {
    settings.settings.voiceAlertMode = VOICE_MODE_DISABLED;

    AlertData alert = AlertData::create(BAND_KA, DIR_FRONT, 4, 0, 34700);
    VoiceContext ctx;
    ctx.priority = &alert;
    ctx.alerts = &alert;
    ctx.alertCount = 1;
    ctx.mainVolume = 5;
    ctx.now = mockMillis;

    VoiceAction action = voiceModule.process(ctx);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(VoiceAction::Type::NONE),
                          static_cast<int>(action.type));

    settings.settings.voiceAlertMode = VOICE_MODE_BAND_FREQ;  // restore
}

void test_process_returns_none_when_muted() {
    AlertData alert = AlertData::create(BAND_KA, DIR_FRONT, 4, 0, 34700);
    VoiceContext ctx;
    ctx.priority = &alert;
    ctx.alerts = &alert;
    ctx.alertCount = 1;
    ctx.mainVolume = 5;  // pass vol-zero guard so we reach the mute check
    // Voice follows V1's audio-mute bit, not the independently debounced LED.
    ctx.isSoftMuted = true;
    ctx.now = mockMillis;

    VoiceAction action = voiceModule.process(ctx);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(VoiceAction::Type::NONE),
                          static_cast<int>(action.type));
}

// LED mute and audio mute can disagree during debounce or acknowledgement;
// only the V1 audio-mute bit silences voice.
void test_process_announces_when_only_led_mute_set_not_softmute() {
    AlertData alert = AlertData::create(BAND_KA, DIR_FRONT, 4, 0, 34700);
    VoiceContext ctx;
    ctx.priority = &alert;
    ctx.alerts = &alert;
    ctx.alertCount = 1;
    ctx.mainVolume = 5;
    ctx.isMuted = true;        // LED bit on …
    ctx.isSoftMuted = false;   // … but V1 audio is NOT muted
    ctx.now = mockMillis;

    VoiceAction action = voiceModule.process(ctx);
    TEST_ASSERT_NOT_EQUAL(static_cast<int>(VoiceAction::Type::NONE),
                          static_cast<int>(action.type));
}

void test_process_returns_none_when_suppressed() {
    AlertData alert = AlertData::create(BAND_KA, DIR_FRONT, 4, 0, 34700);
    VoiceContext ctx;
    ctx.priority = &alert;
    ctx.alerts = &alert;
    ctx.alertCount = 1;
    ctx.mainVolume = 5;  // pass vol-zero guard
    ctx.isSuppressed = true;
    ctx.now = mockMillis;

    VoiceAction action = voiceModule.process(ctx);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(VoiceAction::Type::NONE),
                          static_cast<int>(action.type));
}

void test_process_returns_none_when_proxy_connected() {
    AlertData alert = AlertData::create(BAND_KA, DIR_FRONT, 4, 0, 34700);
    VoiceContext ctx;
    ctx.priority = &alert;
    ctx.alerts = &alert;
    ctx.alertCount = 1;
    ctx.mainVolume = 5;  // pass vol-zero guard
    ctx.isProxyConnected = true;
    ctx.now = mockMillis;

    VoiceAction action = voiceModule.process(ctx);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(VoiceAction::Type::NONE),
                          static_cast<int>(action.type));
}

void test_process_returns_none_when_vol_zero_and_mute_voice_setting() {
    settings.settings.muteVoiceIfVolZero = true;

    AlertData alert = AlertData::create(BAND_KA, DIR_FRONT, 4, 0, 34700);
    VoiceContext ctx;
    ctx.priority = &alert;
    ctx.alerts = &alert;
    ctx.alertCount = 1;
    ctx.mainVolume = 0;
    ctx.now = mockMillis;

    VoiceAction action = voiceModule.process(ctx);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(VoiceAction::Type::NONE),
                          static_cast<int>(action.type));
}

void test_process_announces_when_vol_zero_but_mute_voice_disabled() {
    settings.settings.muteVoiceIfVolZero = false;

    AlertData alert = AlertData::create(BAND_KA, DIR_FRONT, 4, 0, 34700);
    VoiceContext ctx;
    ctx.priority = &alert;
    ctx.alerts = &alert;
    ctx.alertCount = 1;
    ctx.mainVolume = 0;
    ctx.now = mockMillis;

    VoiceAction action = voiceModule.process(ctx);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(VoiceAction::Type::ANNOUNCE_PRIORITY),
                          static_cast<int>(action.type));

    settings.settings.muteVoiceIfVolZero = true;  // restore
}

void test_process_returns_none_when_priority_band_is_none() {
    AlertData alert = AlertData::create(BAND_NONE, DIR_FRONT, 4, 0, 34700);
    VoiceContext ctx;
    ctx.priority = &alert;
    ctx.alerts = &alert;
    ctx.alertCount = 1;
    ctx.mainVolume = 5;  // pass vol-zero guard so we reach the band check
    ctx.now = mockMillis;

    VoiceAction action = voiceModule.process(ctx);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(VoiceAction::Type::NONE),
                          static_cast<int>(action.type));
}

void test_process_announces_priority_for_new_ka_alert() {
    AlertData alert = AlertData::create(BAND_KA, DIR_FRONT, 4, 0, 34700);
    VoiceContext ctx;
    ctx.priority = &alert;
    ctx.alerts = &alert;
    ctx.alertCount = 1;
    ctx.mainVolume = 5;
    ctx.now = mockMillis;

    VoiceAction action = voiceModule.process(ctx);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(VoiceAction::Type::ANNOUNCE_PRIORITY),
                          static_cast<int>(action.type));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertBand::KA), static_cast<int>(action.band));
    TEST_ASSERT_EQUAL_UINT16(34700, action.freq);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertDirection::AHEAD), static_cast<int>(action.dir));
}

// Boot window: a fresh module must announce immediately — the 0-initialized
// last-announcement timestamp is not a real cooldown baseline. (The suite's
// setUp starts at 10 s; this test explicitly covers the first two seconds.)
void test_process_announces_within_first_seconds_of_uptime() {
    mockMillis = 100;  // fresh boot, well inside VOICE_ALERT_COOLDOWN_MS

    AlertData alert = AlertData::create(BAND_KA, DIR_FRONT, 4, 0, 34700);
    VoiceContext ctx;
    ctx.priority = &alert;
    ctx.alerts = &alert;
    ctx.alertCount = 1;
    ctx.mainVolume = 5;
    ctx.now = mockMillis;

    VoiceAction action = voiceModule.process(ctx);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(VoiceAction::Type::ANNOUNCE_PRIORITY),
                          static_cast<int>(action.type));

    ctx.now = mockMillis + 100;
    VoiceAction action2 = voiceModule.process(ctx);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(VoiceAction::Type::NONE),
                          static_cast<int>(action2.type));
}

void test_clear_all_state_restores_first_announcement_eligibility() {
    AlertData first = AlertData::create(BAND_KA, DIR_FRONT, 4, 0, 34700);
    VoiceContext ctx;
    ctx.priority = &first;
    ctx.alerts = &first;
    ctx.alertCount = 1;
    ctx.mainVolume = 5;
    ctx.now = 100;

    TEST_ASSERT_EQUAL_INT(static_cast<int>(VoiceAction::Type::ANNOUNCE_PRIORITY),
                          static_cast<int>(voiceModule.process(ctx).type));

    voiceModule.clearAllState();

    AlertData next = AlertData::create(BAND_K, DIR_FRONT, 4, 0, 24125);
    ctx.priority = &next;
    ctx.alerts = &next;
    ctx.now = 200;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(VoiceAction::Type::ANNOUNCE_PRIORITY),
                          static_cast<int>(voiceModule.process(ctx).type));
}

void test_process_does_not_reannounce_same_alert_within_cooldown() {
    AlertData alert = AlertData::create(BAND_KA, DIR_FRONT, 4, 0, 34700);
    VoiceContext ctx;
    ctx.priority = &alert;
    ctx.alerts = &alert;
    ctx.alertCount = 1;
    ctx.mainVolume = 5;
    ctx.now = mockMillis;

    voiceModule.process(ctx);

    ctx.now = mockMillis + 100;
    VoiceAction action2 = voiceModule.process(ctx);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(VoiceAction::Type::NONE),
                          static_cast<int>(action2.type));
}

void test_process_reannounces_after_cooldown() {
    AlertData alert = AlertData::create(BAND_KA, DIR_FRONT, 4, 0, 34700);
    VoiceContext ctx;
    ctx.priority = &alert;
    ctx.alerts = &alert;
    ctx.alertCount = 1;
    ctx.mainVolume = 5;
    ctx.now = mockMillis;

    voiceModule.process(ctx);

    ctx.now = mockMillis + 2001u;
    VoiceAction action2 = voiceModule.process(ctx);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(VoiceAction::Type::NONE),
                          static_cast<int>(action2.type));
}

void test_process_announces_on_band_change() {
    AlertData kaAlert = AlertData::create(BAND_KA, DIR_FRONT, 4, 0, 34700);
    VoiceContext ctx;
    ctx.priority = &kaAlert;
    ctx.alerts = &kaAlert;
    ctx.alertCount = 1;
    ctx.mainVolume = 5;
    ctx.now = mockMillis;
    voiceModule.process(ctx);

    AlertData kAlert = AlertData::create(BAND_K, DIR_FRONT, 3, 0, 24150);
    ctx.priority = &kAlert;
    ctx.alerts = &kAlert;
    ctx.now = mockMillis + 2001u;

    VoiceAction action = voiceModule.process(ctx);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(VoiceAction::Type::ANNOUNCE_PRIORITY),
                          static_cast<int>(action.type));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertBand::K), static_cast<int>(action.band));
}

void test_process_announces_direction_change_on_same_alert() {
    AlertData kaFront = AlertData::create(BAND_KA, DIR_FRONT, 4, 0, 34700);
    VoiceContext ctx;
    ctx.priority = &kaFront;
    ctx.alerts = &kaFront;
    ctx.alertCount = 1;
    ctx.mainVolume = 5;
    ctx.now = mockMillis;
    voiceModule.process(ctx);

    AlertData kaRear = AlertData::create(BAND_KA, DIR_REAR, 4, 4, 34700);
    ctx.priority = &kaRear;
    ctx.alerts = &kaRear;
    ctx.now = mockMillis + 2001u;

    VoiceAction action = voiceModule.process(ctx);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(VoiceAction::Type::ANNOUNCE_DIRECTION),
                          static_cast<int>(action.type));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertDirection::BEHIND),
                          static_cast<int>(action.dir));
}

void test_process_does_not_announce_direction_when_dir_disabled() {
    settings.settings.voiceDirectionEnabled = false;

    AlertData kaFront = AlertData::create(BAND_KA, DIR_FRONT, 4, 0, 34700);
    VoiceContext ctx;
    ctx.priority = &kaFront;
    ctx.alerts = &kaFront;
    ctx.alertCount = 1;
    ctx.mainVolume = 5;
    ctx.now = mockMillis;
    voiceModule.process(ctx);

    AlertData kaRear = AlertData::create(BAND_KA, DIR_REAR, 4, 4, 34700);
    ctx.priority = &kaRear;
    ctx.alerts = &kaRear;
    ctx.now = mockMillis + 2001u;

    VoiceAction action = voiceModule.process(ctx);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(VoiceAction::Type::NONE),
                          static_cast<int>(action.type));

    settings.settings.voiceDirectionEnabled = true;  // restore
}

// VR data/AlertData.java synthesizes AlertBand.Photo=0xFE only when the raw
// band is K and photoType is non-zero. Preserve that physical K identity but
// select the Photo presentation for voice.
void test_process_announces_photo_for_priority_photo_radar() {
    AlertData alert = AlertData::create(BAND_K, DIR_FRONT, 4, 0, 24125);
    alert.photoType = 1;  // VR prtMRCT — any non-zero value means photo
    VoiceContext ctx;
    ctx.priority = &alert;
    ctx.alerts = &alert;
    ctx.alertCount = 1;
    ctx.mainVolume = 5;
    ctx.now = mockMillis;

    VoiceAction action = voiceModule.process(ctx);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(VoiceAction::Type::ANNOUNCE_PRIORITY),
                          static_cast<int>(action.type));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertBand::PHOTO), static_cast<int>(action.band));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(BAND_K), static_cast<int>(action.sourceBand));
    TEST_ASSERT_TRUE(action.sourcePhoto);
    TEST_ASSERT_EQUAL_UINT16(24125, action.freq);
}

void test_process_announces_normal_k_when_phototype_zero() {
    AlertData alert = AlertData::create(BAND_K, DIR_FRONT, 4, 0, 24125);
    alert.photoType = 0;  // Real K-band radar, not photo
    VoiceContext ctx;
    ctx.priority = &alert;
    ctx.alerts = &alert;
    ctx.alertCount = 1;
    ctx.mainVolume = 5;
    ctx.now = mockMillis;

    VoiceAction action = voiceModule.process(ctx);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(VoiceAction::Type::ANNOUNCE_PRIORITY),
                          static_cast<int>(action.type));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertBand::K), static_cast<int>(action.band));
}

void test_secondary_photo_uses_photo_presentation_before_later_ordinary_k() {
    AlertData alerts[] = {
        AlertData::create(BAND_KA, DIR_FRONT, 4, 0, 34700),
        AlertData::create(BAND_K, DIR_FRONT, 2, 0, 24125),
        AlertData::create(BAND_K, DIR_REAR, 2, 0, 24200),
    };
    alerts[1].photoType = 1;
    VoiceContext ctx;
    ctx.priority = &alerts[0];
    ctx.alerts = alerts;
    ctx.alertCount = 3;
    ctx.mainVolume = 5;
    ctx.now = 1000;
    TEST_ASSERT_EQUAL(VoiceAction::Type::ANNOUNCE_PRIORITY, voiceModule.process(ctx).type);
    ctx.now = 3000;
    const VoiceAction action = voiceModule.prepareAction(ctx);
    TEST_ASSERT_EQUAL(VoiceAction::Type::ANNOUNCE_SECONDARY, action.type);
    TEST_ASSERT_EQUAL_UINT16(24125, action.freq);
    TEST_ASSERT_EQUAL(AlertBand::PHOTO, action.band);
    TEST_ASSERT_TRUE(action.sourcePhoto);
    TEST_ASSERT_EQUAL_UINT8(3, action.sourceAlertCount);
    TEST_ASSERT_EQUAL_UINT8(1, alerts[1].photoType);
}

void test_secondary_photo_and_same_frequency_k_have_distinct_dedup_identity() {
    AlertData alerts[] = {
        AlertData::create(BAND_KA, DIR_FRONT, 4, 0, 34700),
        AlertData::create(BAND_K, DIR_FRONT, 2, 0, 24125),
    };
    alerts[1].photoType = 1;
    VoiceContext ctx;
    ctx.priority = &alerts[0];
    ctx.alerts = alerts;
    ctx.alertCount = 2;
    ctx.mainVolume = 5;
    ctx.now = 1000;
    TEST_ASSERT_EQUAL(VoiceAction::Type::ANNOUNCE_PRIORITY, voiceModule.process(ctx).type);
    ctx.now = 3000;
    const VoiceAction photo = voiceModule.process(ctx);
    TEST_ASSERT_EQUAL(VoiceAction::Type::ANNOUNCE_SECONDARY, photo.type);
    TEST_ASSERT_EQUAL(AlertBand::PHOTO, photo.band);
    alerts[1].photoType = 0;
    ctx.now = 5000;
    const VoiceAction recovered = voiceModule.process(ctx);
    TEST_ASSERT_EQUAL(VoiceAction::Type::ANNOUNCE_SECONDARY, recovered.type);
    TEST_ASSERT_EQUAL(AlertBand::K, recovered.band);
    TEST_ASSERT_EQUAL_UINT16(24125, recovered.freq);
    ctx.now = 5001;
    TEST_ASSERT_EQUAL(VoiceAction::Type::NONE, voiceModule.process(ctx).type);
}

void test_secondary_stability_treats_zero_as_a_real_timestamp() {
    for (unsigned long firstSeenMs : {0UL, 1UL}) {
        voiceModule = VoiceModule();
        voiceModule.begin(&settings, &bleClient);

        AlertData alerts[] = {
            AlertData::create(BAND_KA, DIR_FRONT, 4, 0, 34700),
            AlertData::create(BAND_K, DIR_REAR, 2, 0, 24125),
        };
        VoiceContext ctx;
        ctx.priority = &alerts[0];
        ctx.alerts = alerts;
        ctx.alertCount = 2;
        ctx.mainVolume = 5;
        ctx.now = firstSeenMs;
        TEST_ASSERT_EQUAL(VoiceAction::Type::ANNOUNCE_PRIORITY, voiceModule.process(ctx).type);

        ctx.now = firstSeenMs + 3000UL;
        TEST_ASSERT_EQUAL_MESSAGE(
            VoiceAction::Type::ANNOUNCE_SECONDARY, voiceModule.process(ctx).type,
            firstSeenMs == 0 ? "millis zero must start the priority-stability window"
                             : "nonzero priority-stability control must still announce");
    }
}

void test_priority_k_to_photo_waits_for_cooldown_then_announces_photo_once() {
    AlertData alert = AlertData::create(BAND_K, DIR_FRONT, 4, 0, 24125);
    VoiceContext ctx;
    ctx.priority = &alert;
    ctx.alerts = &alert;
    ctx.alertCount = 1;
    ctx.mainVolume = 5;
    ctx.now = 10000;

    const VoiceAction k = voiceModule.process(ctx);
    TEST_ASSERT_EQUAL(VoiceAction::Type::ANNOUNCE_PRIORITY, k.type);
    TEST_ASSERT_EQUAL(AlertBand::K, k.band);

    alert.photoType = 1;
    ctx.now = 11999;
    TEST_ASSERT_EQUAL(VoiceAction::Type::NONE, voiceModule.process(ctx).type);
    ctx.now = 12000;
    const VoiceAction photo = voiceModule.process(ctx);
    TEST_ASSERT_EQUAL(VoiceAction::Type::ANNOUNCE_PRIORITY, photo.type);
    TEST_ASSERT_EQUAL(AlertBand::PHOTO, photo.band);
    TEST_ASSERT_TRUE(photo.sourcePhoto);
    ctx.now = 14000;
    TEST_ASSERT_EQUAL(VoiceAction::Type::NONE, voiceModule.process(ctx).type);
}

void test_prepare_priority_does_not_commit_until_playback_accepts() {
    AlertData alert = AlertData::create(BAND_KA, DIR_FRONT, 4, 0, 34700);
    VoiceContext ctx;
    ctx.priority = &alert;
    ctx.alerts = &alert;
    ctx.alertCount = 1;
    ctx.mainVolume = 5;
    ctx.now = 10000;

    const VoiceAction first = voiceModule.prepareAction(ctx);
    const VoiceAction busyRetry = voiceModule.prepareAction(ctx);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(VoiceAction::Type::ANNOUNCE_PRIORITY), static_cast<int>(first.type));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(first.type), static_cast<int>(busyRetry.type));
    TEST_ASSERT_EQUAL_UINT16(first.freq, busyRetry.freq);

    voiceModule.commitAction(first, ctx.now);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(VoiceAction::Type::NONE),
                          static_cast<int>(voiceModule.prepareAction(ctx).type));
}

void test_prepare_direction_and_secondary_remain_retryable_until_commit() {
    settings.settings.announceSecondaryAlerts = true;
    settings.settings.secondaryK = true;
    AlertData alerts[2] = {
        AlertData::create(BAND_KA, DIR_FRONT, 5, 0, 34700),
        AlertData::create(BAND_K, DIR_FRONT, 2, 0, 24120),
    };
    VoiceContext ctx;
    ctx.priority = &alerts[0];
    ctx.alerts = alerts;
    ctx.alertCount = 2;
    ctx.mainVolume = 5;
    ctx.now = 10000;
    voiceModule.process(ctx); // accepted priority baseline

    ctx.now = 13000;
    const VoiceAction secondary = voiceModule.prepareAction(ctx);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(VoiceAction::Type::ANNOUNCE_SECONDARY),
                          static_cast<int>(secondary.type));
    TEST_ASSERT_EQUAL_UINT16(secondary.freq, voiceModule.prepareAction(ctx).freq);
    voiceModule.commitAction(secondary, ctx.now);

    alerts[0].direction = DIR_REAR;
    ctx.now = 16000;
    const VoiceAction direction = voiceModule.prepareAction(ctx);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(VoiceAction::Type::ANNOUNCE_DIRECTION),
                          static_cast<int>(direction.type));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(VoiceAction::Type::ANNOUNCE_DIRECTION),
                          static_cast<int>(voiceModule.prepareAction(ctx).type));
    voiceModule.commitAction(direction, ctx.now);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(VoiceAction::Type::NONE),
                          static_cast<int>(voiceModule.prepareAction(ctx).type));
}

void test_prepare_escalation_is_not_marked_announced_before_commit() {
    settings.settings.announceSecondaryAlerts = true;
    settings.settings.secondaryK = true;
    AlertData alerts[2] = {
        AlertData::create(BAND_KA, DIR_FRONT, 5, 0, 34700),
        AlertData::create(BAND_K, DIR_REAR, 2, 0, 24120),
    };
    VoiceContext ctx;
    ctx.priority = &alerts[0];
    ctx.alerts = alerts;
    ctx.alertCount = 2;
    ctx.mainVolume = 5;
    ctx.now = 1000;
    voiceModule.process(ctx);
    ctx.now = 4000;
    voiceModule.process(ctx); // accepted secondary announcement

    voiceModule.testUpdateAlertHistory(BAND_K, 24120, 2, 4100);
    voiceModule.testUpdateAlertHistory(BAND_K, 24120, 5, 4200);
    alerts[1].rearStrength = 5;
    ctx.now = 4800;
    const VoiceAction escalation = voiceModule.prepareAction(ctx);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(VoiceAction::Type::ANNOUNCE_ESCALATION),
                          static_cast<int>(escalation.type));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(VoiceAction::Type::ANNOUNCE_ESCALATION),
                          static_cast<int>(voiceModule.prepareAction(ctx).type));
    voiceModule.commitAction(escalation, ctx.now);
    TEST_ASSERT_FALSE(voiceModule.prepareAction(ctx).type == VoiceAction::Type::ANNOUNCE_ESCALATION);
}

// Run one process() cycle with `priority` first and `secondary` second. Keeping
// the same priority alert across calls holds canAnnounceSecondary() satisfied.
static VoiceAction runWithSecondary(const AlertData& priority, const AlertData& secondary, unsigned long now) {
    AlertData alerts[2] = {priority, secondary};
    VoiceContext ctx;
    ctx.priority = &alerts[0];
    ctx.alerts = alerts;
    ctx.alertCount = 2;
    ctx.mainVolume = 5;
    ctx.now = now;
    return voiceModule.process(ctx);
}

void test_announced_set_evicts_oldest_instead_of_saturating() {
    settings.settings.announceSecondaryAlerts = true;

    // Establish a stable priority alert. This also consumes one announced slot.
    AlertData priority = AlertData::create(BAND_KA, DIR_FRONT, 5, 0, 34700);
    VoiceContext ctx;
    ctx.priority = &priority;
    ctx.alerts = &priority;
    ctx.alertCount = 1;
    ctx.mainVolume = 5;
    ctx.now = mockMillis;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(VoiceAction::Type::ANNOUNCE_PRIORITY),
                          static_cast<int>(voiceModule.process(ctx).type));

    // Announce 10 distinct secondaries. The priority already holds a slot, so the
    // window fills before the loop ends — the tail is exactly what the old
    // saturating set silently refused to record.
    const int kSecondaries = 10;  // VoiceModule::MAX_ANNOUNCED_ALERTS
    uint16_t freqs[kSecondaries];
    unsigned long now = mockMillis;
    for (int i = 0; i < kSecondaries; i++) {
        now += 3000;  // clears PRIORITY_STABILITY_MS and POST_PRIORITY_GAP_MS
        freqs[i] = static_cast<uint16_t>(24100 + i);
        AlertData secondary = AlertData::create(BAND_K, DIR_FRONT, 3, 0, freqs[i]);
        VoiceAction announced = runWithSecondary(priority, secondary, now);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(VoiceAction::Type::ANNOUNCE_SECONDARY),
                              static_cast<int>(announced.type));
        TEST_ASSERT_EQUAL_UINT16(freqs[i], announced.freq);
    }

    // The newest secondary must be remembered. Once the old set saturated it
    // stopped recording, so this bogey announced again on every later cycle.
    now += 3000;
    const uint16_t newestFreq = freqs[kSecondaries - 1];
    AlertData newest = AlertData::create(BAND_K, DIR_FRONT, 3, 0, newestFreq);
    VoiceAction repeat = runWithSecondary(priority, newest, now);
    TEST_ASSERT_FALSE(repeat.type == VoiceAction::Type::ANNOUNCE_SECONDARY && repeat.freq == newestFreq);

    // Eviction is oldest-first: one more new secondary pushes freqs[0] out of the
    // window, so it re-arms while the rest of the window stays suppressed.
    now += 3000;
    const uint16_t extraFreq = 24200;
    AlertData extra = AlertData::create(BAND_K, DIR_FRONT, 3, 0, extraFreq);
    VoiceAction extraAction = runWithSecondary(priority, extra, now);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(VoiceAction::Type::ANNOUNCE_SECONDARY),
                          static_cast<int>(extraAction.type));
    TEST_ASSERT_EQUAL_UINT16(extraFreq, extraAction.freq);

    now += 3000;
    AlertData evicted = AlertData::create(BAND_K, DIR_FRONT, 3, 0, freqs[0]);
    VoiceAction reArmed = runWithSecondary(priority, evicted, now);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(VoiceAction::Type::ANNOUNCE_SECONDARY),
                          static_cast<int>(reArmed.type));
    TEST_ASSERT_EQUAL_UINT16(freqs[0], reArmed.freq);

    // Re-announcing freqs[0] above pushed freqs[1] out in turn, so the oldest id
    // still inside the window is freqs[2]. It must stay suppressed.
    now += 3000;
    AlertData stillHeld = AlertData::create(BAND_K, DIR_FRONT, 3, 0, freqs[2]);
    VoiceAction held = runWithSecondary(priority, stillHeld, now);
    TEST_ASSERT_FALSE(held.type == VoiceAction::Type::ANNOUNCE_SECONDARY && held.freq == freqs[2]);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_announced_set_evicts_oldest_instead_of_saturating);

    RUN_TEST(test_make_alert_id_encodes_band_and_freq);
    RUN_TEST(test_make_alert_id_different_bands_differ);
    RUN_TEST(test_make_alert_id_different_freqs_differ);
    RUN_TEST(test_make_alert_id_distinguishes_photo_presentation);
    RUN_TEST(test_to_audio_direction_front);
    RUN_TEST(test_to_audio_direction_rear);
    RUN_TEST(test_to_audio_direction_side_is_default);
    RUN_TEST(test_to_audio_direction_none_is_side);
    RUN_TEST(test_is_band_enabled_for_secondary_ka_follows_setting);
    RUN_TEST(test_is_band_enabled_for_secondary_laser_follows_setting);
    RUN_TEST(test_is_band_enabled_for_secondary_unknown_band_false);
    RUN_TEST(test_get_alert_bars_front_direction_uses_front_strength);
    RUN_TEST(test_get_alert_bars_rear_direction_uses_rear_strength);
    RUN_TEST(test_get_alert_bars_no_direction_uses_max);

    RUN_TEST(test_process_returns_none_when_no_priority);
    RUN_TEST(test_process_returns_none_when_voice_disabled);
    RUN_TEST(test_process_returns_none_when_muted);
    RUN_TEST(test_process_announces_when_only_led_mute_set_not_softmute);
    RUN_TEST(test_process_returns_none_when_suppressed);
    RUN_TEST(test_process_returns_none_when_proxy_connected);
    RUN_TEST(test_process_returns_none_when_vol_zero_and_mute_voice_setting);
    RUN_TEST(test_process_announces_when_vol_zero_but_mute_voice_disabled);
    RUN_TEST(test_process_returns_none_when_priority_band_is_none);

    RUN_TEST(test_process_announces_priority_for_new_ka_alert);
    RUN_TEST(test_process_announces_within_first_seconds_of_uptime);
    RUN_TEST(test_clear_all_state_restores_first_announcement_eligibility);
    RUN_TEST(test_process_does_not_reannounce_same_alert_within_cooldown);
    RUN_TEST(test_process_reannounces_after_cooldown);
    RUN_TEST(test_process_announces_on_band_change);

    RUN_TEST(test_process_announces_direction_change_on_same_alert);
    RUN_TEST(test_process_does_not_announce_direction_when_dir_disabled);

    RUN_TEST(test_process_announces_photo_for_priority_photo_radar);
    RUN_TEST(test_process_announces_normal_k_when_phototype_zero);
    RUN_TEST(test_secondary_photo_uses_photo_presentation_before_later_ordinary_k);
    RUN_TEST(test_secondary_photo_and_same_frequency_k_have_distinct_dedup_identity);
    RUN_TEST(test_secondary_stability_treats_zero_as_a_real_timestamp);
    RUN_TEST(test_priority_k_to_photo_waits_for_cooldown_then_announces_photo_once);
    RUN_TEST(test_prepare_priority_does_not_commit_until_playback_accepts);
    RUN_TEST(test_prepare_direction_and_secondary_remain_retryable_until_commit);
    RUN_TEST(test_prepare_escalation_is_not_marked_announced_before_commit);

    return UNITY_END();
}

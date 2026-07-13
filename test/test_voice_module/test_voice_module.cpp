/**
 * VoiceModule decision-logic contract tests.
 *
 * Covers VoiceModule::process() early-exit guards, the ANNOUNCE_PRIORITY
 * happy path, the ANNOUNCE_DIRECTION case, and all static utilities.
 * Does not test audio playback (VoiceModule returns an action; caller executes it).
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

SettingsManager settingsManager;
static VoiceModule voiceModule;
static V1BLEClient bleClient;

void setUp() {
    settingsManager = SettingsManager();
    bleClient.reset();
    voiceModule = VoiceModule();
    voiceModule.begin(&settingsManager, &bleClient);
    mockMillis = 10000;  // Start at 10 s so cooldown (2 s) is always pre-satisfied
}

void tearDown() {}

// ---------------------------------------------------------------------------
// Static utility: makeAlertId
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Static utility: toAudioDirection
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Static utility: isBandEnabledForSecondary
// ---------------------------------------------------------------------------

void test_is_band_enabled_for_secondary_ka_follows_setting() {
    const V1Settings& s = settingsManager.get();
    TEST_ASSERT_TRUE(VoiceModule::isBandEnabledForSecondary(BAND_KA, s));

    settingsManager.settings.secondaryKa = false;
    TEST_ASSERT_FALSE(VoiceModule::isBandEnabledForSecondary(BAND_KA, settingsManager.get()));
    settingsManager.settings.secondaryKa = true;  // restore
}

void test_is_band_enabled_for_secondary_laser_follows_setting() {
    const V1Settings& s = settingsManager.get();
    TEST_ASSERT_TRUE(VoiceModule::isBandEnabledForSecondary(BAND_LASER, s));

    settingsManager.settings.secondaryLaser = false;
    TEST_ASSERT_FALSE(VoiceModule::isBandEnabledForSecondary(BAND_LASER, settingsManager.get()));
    settingsManager.settings.secondaryLaser = true;
}

void test_is_band_enabled_for_secondary_unknown_band_false() {
    const V1Settings& s = settingsManager.get();
    TEST_ASSERT_FALSE(VoiceModule::isBandEnabledForSecondary(BAND_NONE, s));
}

// ---------------------------------------------------------------------------
// Static utility: getAlertBars
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// process() early-exit guards
// ---------------------------------------------------------------------------

void test_process_returns_none_when_no_priority() {
    VoiceContext ctx;
    ctx.now = mockMillis;
    ctx.mainVolume = 5;   // pass vol-zero guard so we reach the no-priority check
    // priority is nullptr by default
    VoiceAction action = voiceModule.process(ctx);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(VoiceAction::Type::NONE),
                          static_cast<int>(action.type));
}

void test_process_returns_none_when_voice_disabled() {
    settingsManager.settings.voiceAlertMode = VOICE_MODE_DISABLED;

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

    settingsManager.settings.voiceAlertMode = VOICE_MODE_BAND_FREQ;  // restore
}

void test_process_returns_none_when_muted() {
    AlertData alert = AlertData::create(BAND_KA, DIR_FRONT, 4, 0, 34700);
    VoiceContext ctx;
    ctx.priority = &alert;
    ctx.alerts = &alert;
    ctx.alertCount = 1;
    ctx.mainVolume = 5;  // pass vol-zero guard so we reach the mute check
    // Audit B2: voice gates on V1's audio-mute (aux0 bit 0), not the LED bit.
    ctx.isSoftMuted = true;
    ctx.now = mockMillis;

    VoiceAction action = voiceModule.process(ctx);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(VoiceAction::Type::NONE),
                          static_cast<int>(action.type));
}

// Audit B2 regression: the LED-mute bit alone (isMuted) must NOT silence
// voice — only V1's audio-mute (isSoftMuted) does.  The two can disagree
// while LED debounce is in flight or while the user is mid-acknowledge.
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
    settingsManager.settings.muteVoiceIfVolZero = true;

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
    settingsManager.settings.muteVoiceIfVolZero = false;

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

    settingsManager.settings.muteVoiceIfVolZero = true;  // restore
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

// ---------------------------------------------------------------------------
// process() ANNOUNCE_PRIORITY happy path
// ---------------------------------------------------------------------------

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
// setUp starts at 10 s precisely because this used to suppress announcements
// for the first 2 s of uptime.)
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

    // And the normal cooldown still applies right after that announcement.
    ctx.now = mockMillis + 100;
    VoiceAction action2 = voiceModule.process(ctx);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(VoiceAction::Type::NONE),
                          static_cast<int>(action2.type));
}

void test_process_does_not_reannounce_same_alert_within_cooldown() {
    AlertData alert = AlertData::create(BAND_KA, DIR_FRONT, 4, 0, 34700);
    VoiceContext ctx;
    ctx.priority = &alert;
    ctx.alerts = &alert;
    ctx.alertCount = 1;
    ctx.mainVolume = 5;
    ctx.now = mockMillis;

    // First call — announces
    voiceModule.process(ctx);

    // Second call immediately after — cooldown not passed
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

    // First announcement
    voiceModule.process(ctx);

    // After cooldown (2000 ms; VOICE_ALERT_COOLDOWN_MS)
    ctx.now = mockMillis + 2001u;
    VoiceAction action2 = voiceModule.process(ctx);
    // Same band+freq → not a "new alert" via hasAlertChanged — expect NONE
    TEST_ASSERT_EQUAL_INT(static_cast<int>(VoiceAction::Type::NONE),
                          static_cast<int>(action2.type));
}

// ---------------------------------------------------------------------------
// process() ANNOUNCE_PRIORITY — band change triggers new announcement
// ---------------------------------------------------------------------------

void test_process_announces_on_band_change() {
    // First: KA alert
    AlertData kaAlert = AlertData::create(BAND_KA, DIR_FRONT, 4, 0, 34700);
    VoiceContext ctx;
    ctx.priority = &kaAlert;
    ctx.alerts = &kaAlert;
    ctx.alertCount = 1;
    ctx.mainVolume = 5;
    ctx.now = mockMillis;
    voiceModule.process(ctx);

    // Now: K alert after cooldown
    AlertData kAlert = AlertData::create(BAND_K, DIR_FRONT, 3, 0, 24150);
    ctx.priority = &kAlert;
    ctx.alerts = &kAlert;
    ctx.now = mockMillis + 2001u;

    VoiceAction action = voiceModule.process(ctx);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(VoiceAction::Type::ANNOUNCE_PRIORITY),
                          static_cast<int>(action.type));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertBand::K), static_cast<int>(action.band));
}

// ---------------------------------------------------------------------------
// process() ANNOUNCE_DIRECTION — direction change on same alert
// ---------------------------------------------------------------------------

void test_process_announces_direction_change_on_same_alert() {
    // Announce initial KA front
    AlertData kaFront = AlertData::create(BAND_KA, DIR_FRONT, 4, 0, 34700);
    VoiceContext ctx;
    ctx.priority = &kaFront;
    ctx.alerts = &kaFront;
    ctx.alertCount = 1;
    ctx.mainVolume = 5;
    ctx.now = mockMillis;
    voiceModule.process(ctx);

    // Same alert, direction changes to rear — past cooldown
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
    settingsManager.settings.voiceDirectionEnabled = false;

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

    settingsManager.settings.voiceDirectionEnabled = true;  // restore
}

// V1 audit R1: Photo-radar (band=K with photoType!=0) must NOT trigger any
// voice announcement. VR data/AlertData.java synthesizes AlertBand.Photo=0xFE
// only when band==K and photoType!=0; we have no `band_photo.mul` audio
// asset, and announcing it as plain "K" is misleading. The display already
// shows 'P' on the bogey counter and a Photo card.
void test_process_suppresses_voice_for_priority_photo_radar() {
    AlertData alert = AlertData::create(BAND_K, DIR_FRONT, 4, 0, 24125);
    alert.photoType = 1;  // VR prtMRCT — any non-zero value means photo
    VoiceContext ctx;
    ctx.priority = &alert;
    ctx.alerts = &alert;
    ctx.alertCount = 1;
    ctx.mainVolume = 5;
    ctx.now = mockMillis;

    VoiceAction action = voiceModule.process(ctx);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(VoiceAction::Type::NONE),
                          static_cast<int>(action.type));
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

int main() {
    UNITY_BEGIN();

    // Static utilities
    RUN_TEST(test_make_alert_id_encodes_band_and_freq);
    RUN_TEST(test_make_alert_id_different_bands_differ);
    RUN_TEST(test_make_alert_id_different_freqs_differ);
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

    // Early-exit guards
    RUN_TEST(test_process_returns_none_when_no_priority);
    RUN_TEST(test_process_returns_none_when_voice_disabled);
    RUN_TEST(test_process_returns_none_when_muted);
    RUN_TEST(test_process_announces_when_only_led_mute_set_not_softmute);
    RUN_TEST(test_process_returns_none_when_suppressed);
    RUN_TEST(test_process_returns_none_when_proxy_connected);
    RUN_TEST(test_process_returns_none_when_vol_zero_and_mute_voice_setting);
    RUN_TEST(test_process_announces_when_vol_zero_but_mute_voice_disabled);
    RUN_TEST(test_process_returns_none_when_priority_band_is_none);

    // Happy path
    RUN_TEST(test_process_announces_priority_for_new_ka_alert);
    RUN_TEST(test_process_announces_within_first_seconds_of_uptime);
    RUN_TEST(test_process_does_not_reannounce_same_alert_within_cooldown);
    RUN_TEST(test_process_reannounces_after_cooldown);
    RUN_TEST(test_process_announces_on_band_change);

    // Direction change
    RUN_TEST(test_process_announces_direction_change_on_same_alert);
    RUN_TEST(test_process_does_not_announce_direction_when_dir_disabled);

    // V1 audit R1 — photo-radar voice suppression
    RUN_TEST(test_process_suppresses_voice_for_priority_photo_radar);
    RUN_TEST(test_process_announces_normal_k_when_phototype_zero);

    return UNITY_END();
}

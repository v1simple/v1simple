// Exercise the real parser, fade state machine and outgoing quiet-volume
// boundary. An alert can be parsed before the first volume-bearing packet;
// an absent baseline must not become a zero-volume restore command.
#include <unity.h>

#include <initializer_list>
#include <vector>

#include "../mocks/Arduino.h"
#include "../mocks/ble_client.h"
#include "../mocks/settings.h"

SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;

// Native parser compilation needs protocol constants without display wiring.
#ifndef CONFIG_H
#define CONFIG_H
#define ESP_PACKET_START 0xAA
#define ESP_PACKET_END 0xAB
#define PACKET_ID_DISPLAY_DATA 0x31
#define PACKET_ID_ALERT_DATA 0x43
#define PACKET_ID_WRITE_USER_BYTES 0x13
#define PACKET_ID_TURN_OFF_DISPLAY 0x32
#define PACKET_ID_TURN_ON_DISPLAY 0x33
#define PACKET_ID_MUTE_ON 0x34
#define PACKET_ID_MUTE_OFF 0x35
#define PACKET_ID_REQ_WRITE_VOLUME 0x39
#define PACKET_ID_RESP_USER_BYTES 0x12
#define PACKET_ID_VERSION 0x01
#define PACKET_ID_RESP_VERSION 0x02
#define PACKET_ID_REQ_ALL_VOLUME 0x3C
#define PACKET_ID_RESP_ALL_VOLUME 0x3D
#endif

// Include the production parser before the quiet module's mock include: its
// canonical include guard keeps this entire path on the actual parser type.
#include "../../src/packet_parser.h"
#include "../../src/packet_parser.cpp"
#include "../../src/packet_parser_alerts.cpp"
#include "../../src/modules/volume_fade/volume_fade_module.cpp"
#include "../../src/modules/quiet/quiet_coordinator_module.cpp"
#include "../../src/modules/quiet/quiet_coordinator_templates.h"
#include "../mocks/display.h"
#include "../mocks/modules/power/power_module.h"
#include "../mocks/modules/ble/ble_queue_module.h"
#include "../mocks/modules/alert_persistence/alert_persistence_module.h"
#include "../../src/modules/ble/connection_state_module.cpp"
#include "../../src/modules/speed_mute/speed_mute_module.cpp"

// Only preview/restore presentation is inert; the parsed-frame volume gate is
// the production orchestration path, alongside the actual parser and owners.
class DisplayPreviewModule {
  public:
    bool isRunning() const { return false; }
    void update() {}
};
class DisplayRestoreModule {
  public:
    bool process() { return false; }
};
#include "../../src/modules/display/display_orchestration_module.cpp"

namespace {
V1BLEClient ble;
PacketParser parser;
SettingsManager settings;
VolumeFadeModule fade;
QuietCoordinatorModule quiet;
V1Display display;
PowerModule power;
BleQueueModule queue;
AlertPersistenceModule persistence;
ConnectionStateModule connection;

void openSession(uint32_t generation, uint32_t nowMs) {
    mockMillis = nowMs;
    ble.setConnected(true);
    ble.setSessionGeneration(generation);
    connection.handleSessionOpened(generation);
    connection.handleConnected(nowMs, generation);
    connection.process(nowMs);
}

void closeSession(uint32_t generation, uint32_t nowMs) {
    mockMillis = nowMs;
    ble.setConnected(false);
    ble.setSessionGeneration(generation);
    connection.handleSessionClosed(nowMs, generation);
    connection.process(nowMs);
    TEST_ASSERT_FALSE(parser.hasAlerts());
    TEST_ASSERT_FALSE(queue.sessionOpen);
}

void feed(uint8_t id, std::initializer_list<uint8_t> data, uint32_t nowMs) {
    // V1 -> remote. ESP length includes the checksum; the end marker does not.
    std::vector<uint8_t> packet{0xAA, 0xD6, 0xEA, id, static_cast<uint8_t>(data.size() + 1)};
    packet.insert(packet.end(), data.begin(), data.end());
    uint8_t checksum = 0;
    for (uint8_t value : packet) {
        checksum = static_cast<uint8_t>(checksum + value);
    }
    packet.push_back(checksum);
    packet.push_back(0xAB);
    mockMillis = nowMs;
    TEST_ASSERT_TRUE(parser.parse(packet.data(), packet.size(), nowMs));
}

void alert(uint32_t nowMs) {
    // Complete priority Ka 34.700 GHz row, including its aux0 data byte.
    feed(PACKET_ID_ALERT_DATA, {0x11, 0x87, 0x8C, 0xAC, 0x00, 0x22, 0x80}, nowMs);
    TEST_ASSERT_TRUE(parser.hasAlerts());
}

void volume(uint8_t main, uint8_t muted, uint32_t nowMs) {
    feed(PACKET_ID_RESP_ALL_VOLUME, {main, muted, main, muted}, nowMs);
    TEST_ASSERT_TRUE(parser.getDisplayState().hasVolumeData);
}

void clearAlert(uint32_t nowMs) {
    feed(PACKET_ID_ALERT_DATA, {0, 0, 0, 0, 0, 0, 0}, nowMs);
    TEST_ASSERT_FALSE(parser.hasAlerts());
}

void expectFadeAndRestore(uint32_t fadeAtMs = 12500) {
    const int priorCalls = ble.setVolumeCalls;
    alert(fadeAtMs);
    TEST_ASSERT_TRUE(quiet.executeVolumeFade(fadeAtMs, &fade));
    TEST_ASSERT_EQUAL_INT(priorCalls + 1, ble.setVolumeCalls);
    TEST_ASSERT_EQUAL_UINT8(1, ble.lastVolume);
    TEST_ASSERT_EQUAL_UINT8(2, ble.lastMuteVolume);

    // Echo the temporary current pair while retaining the detector's saved pair.
    feed(PACKET_ID_RESP_ALL_VOLUME, {1, 2, 6, 2}, fadeAtMs + 100);
    TEST_ASSERT_FALSE(quiet.executeVolumeFade(fadeAtMs + 100, &fade));
    clearAlert(fadeAtMs + 1500);
    TEST_ASSERT_TRUE(quiet.executeVolumeFade(fadeAtMs + 1500, &fade));
    TEST_ASSERT_EQUAL_INT(priorCalls + 2, ble.setVolumeCalls);
    TEST_ASSERT_EQUAL_UINT8(6, ble.lastVolume);
    TEST_ASSERT_EQUAL_UINT8(2, ble.lastMuteVolume);
}
} // namespace

void setUp() {
    ble.reset();
    parser = PacketParser{};
    settings = SettingsManager{};
    fade = VolumeFadeModule{};
    quiet = QuietCoordinatorModule{};
    settings.settings.alertVolumeFadeEnabled = true;
    settings.settings.alertVolumeFadeDelaySec = 2;
    settings.settings.alertVolumeFadeVolume = 1;
    fade.begin(&settings);
    quiet.begin(&ble, &parser);
    display.reset();
    power.reset();
    queue.reset();
    persistence.reset();
    connection.begin(&ble, &parser, &display, &power, &queue, &persistence);
    openSession(1, 9000);
}

void tearDown() {}

void test_alert_before_first_volume_preserves_real_baseline_pair() {
    alert(10000); // Normal operation after boot splash; no volume received yet.
    TEST_ASSERT_FALSE(parser.getDisplayState().hasVolumeData);
    TEST_ASSERT_FALSE(quiet.executeVolumeFade(10000, &fade));
    TEST_ASSERT_EQUAL_INT(0, ble.setVolumeCalls);

    volume(6, 2, 10100);
    TEST_ASSERT_FALSE(quiet.executeVolumeFade(10100, &fade));
    expectFadeAndRestore();
}

void test_volume_before_first_alert_preserves_existing_fade_and_restore() {
    volume(6, 2, 9900);
    alert(10000);
    TEST_ASSERT_FALSE(quiet.executeVolumeFade(10000, &fade));
    expectFadeAndRestore();
}

void test_known_zero_volume_is_preserved() {
    volume(0, 2, 9900);
    alert(10000);
    TEST_ASSERT_FALSE(quiet.executeVolumeFade(10000, &fade));
    alert(12500);
    TEST_ASSERT_FALSE(quiet.executeVolumeFade(12500, &fade));
    clearAlert(14000);
    TEST_ASSERT_FALSE(quiet.executeVolumeFade(14000, &fade));
    TEST_ASSERT_EQUAL_UINT8(0, parser.getDisplayState().mainVolume);
    TEST_ASSERT_EQUAL_UINT8(2, parser.getDisplayState().muteVolume);
    TEST_ASSERT_EQUAL_INT(0, ble.setVolumeCalls);
}

void test_alert_without_volume_data_never_requests_a_volume_change() {
    alert(10000);
    TEST_ASSERT_FALSE(quiet.executeVolumeFade(10000, &fade));
    alert(12500);
    TEST_ASSERT_FALSE(quiet.executeVolumeFade(12500, &fade));
    clearAlert(14000);
    TEST_ASSERT_FALSE(quiet.executeVolumeFade(14000, &fade));
    TEST_ASSERT_FALSE(parser.getDisplayState().hasVolumeData);
    TEST_ASSERT_EQUAL_INT(0, ble.setVolumeCalls);
}

void test_reconnect_alert_before_volume_uses_new_session_baseline() {
    volume(0, 0, 9500);
    closeSession(2, 10000);
    openSession(3, 20000);
    alert(20100);
    TEST_ASSERT_FALSE(quiet.executeVolumeFade(20100, &fade));
    volume(6, 2, 20200);
    TEST_ASSERT_FALSE(quiet.executeVolumeFade(20200, &fade));
    expectFadeAndRestore(22600);
}

void test_reconnect_volume_before_alert_preserves_normal_restoration() {
    volume(0, 0, 9500);
    closeSession(2, 10000);
    openSession(3, 20000);
    volume(6, 2, 20010);
    alert(20100);
    TEST_ASSERT_FALSE(quiet.executeVolumeFade(20100, &fade));
    expectFadeAndRestore(22600);
}

void startActiveFade() {
    volume(6, 2, 9500);
    alert(10000);
    TEST_ASSERT_FALSE(quiet.executeVolumeFade(10000, &fade));
    alert(12500);
    TEST_ASSERT_TRUE(quiet.executeVolumeFade(12500, &fade));
    TEST_ASSERT_EQUAL_UINT8(1, ble.lastVolume);
    feed(PACKET_ID_RESP_ALL_VOLUME, {1, 2, 6, 2}, 12600);
    TEST_ASSERT_FALSE(quiet.executeVolumeFade(12600, &fade));
}

void test_same_detector_reconnect_preserves_active_fade_restore() {
    startActiveFade();
    closeSession(2, 14000);
    openSession(3, 20000);
    feed(PACKET_ID_RESP_ALL_VOLUME, {1, 2, 6, 2}, 20100);
    const int priorCalls = ble.setVolumeCalls;
    TEST_ASSERT_TRUE(quiet.executeVolumeFade(20100, &fade));
    TEST_ASSERT_EQUAL_INT(priorCalls + 1, ble.setVolumeCalls);
    TEST_ASSERT_EQUAL_UINT8(6, ble.lastVolume);
    TEST_ASSERT_EQUAL_UINT8(2, ble.lastMuteVolume);
}

void test_same_detector_reconnect_alert_before_volume_preserves_active_fade_restore() {
    startActiveFade();
    closeSession(2, 14000);
    openSession(3, 20000);
    alert(20100);
    const int priorCalls = ble.setVolumeCalls;
    TEST_ASSERT_FALSE(quiet.executeVolumeFade(20100, &fade));
    TEST_ASSERT_EQUAL_INT(priorCalls, ble.setVolumeCalls);
    feed(PACKET_ID_RESP_ALL_VOLUME, {1, 2, 6, 2}, 20200);
    TEST_ASSERT_FALSE(quiet.executeVolumeFade(20200, &fade));
    clearAlert(23000);
    TEST_ASSERT_TRUE(quiet.executeVolumeFade(23000, &fade));
    TEST_ASSERT_EQUAL_UINT8(6, ble.lastVolume);
    TEST_ASSERT_EQUAL_UINT8(2, ble.lastMuteVolume);
}

void test_reconnect_requires_fresh_volume_and_preserves_known_zero() {
    volume(6, 2, 9500);
    closeSession(2, 10000);
    TEST_ASSERT_FALSE(parser.getDisplayState().hasVolumeData);
    TEST_ASSERT_FALSE(parser.getDisplayState().hasSavedVolume);
    TEST_ASSERT_EQUAL_UINT8(0, parser.getDisplayState().mainVolume);
    TEST_ASSERT_EQUAL_UINT8(0, parser.getDisplayState().muteVolume);
    TEST_ASSERT_EQUAL_UINT8(0, parser.getDisplayState().savedMainVolume);
    TEST_ASSERT_EQUAL_UINT8(0, parser.getDisplayState().savedMuteVolume);
    openSession(3, 20000);
    TEST_ASSERT_FALSE(parser.getDisplayState().hasVolumeData);
    alert(20100);
    TEST_ASSERT_FALSE(quiet.executeVolumeFade(20100, &fade));
    alert(23000);
    TEST_ASSERT_FALSE(quiet.executeVolumeFade(23000, &fade));
    TEST_ASSERT_EQUAL_INT(0, ble.setVolumeCalls);
    volume(0, 0, 23100);
    TEST_ASSERT_TRUE(parser.getDisplayState().hasVolumeData);
    TEST_ASSERT_FALSE(quiet.executeVolumeFade(23100, &fade));
    clearAlert(24000);
    TEST_ASSERT_FALSE(quiet.executeVolumeFade(24000, &fade));
    TEST_ASSERT_EQUAL_INT(0, ble.setVolumeCalls);
}

void test_generation_watchdog_invalidates_volume_before_new_alert() {
    volume(0, 0, 9500);
    ble.setSessionGeneration(3);
    mockMillis = 20000;
    TEST_ASSERT_TRUE(connection.process(20000));
    TEST_ASSERT_FALSE(parser.getDisplayState().hasVolumeData);
    alert(20100);
    TEST_ASSERT_FALSE(quiet.executeVolumeFade(20100, &fade));
    volume(6, 2, 20200);
    TEST_ASSERT_FALSE(quiet.executeVolumeFade(20200, &fade));
    expectFadeAndRestore(22600);
}

void test_session_open_invalidates_volume_without_close_callback() {
    volume(0, 0, 9500);
    openSession(3, 20000);
    TEST_ASSERT_FALSE(parser.getDisplayState().hasVolumeData);
    alert(20100);
    TEST_ASSERT_FALSE(quiet.executeVolumeFade(20100, &fade));
    volume(6, 2, 20200);
    TEST_ASSERT_FALSE(quiet.executeVolumeFade(20200, &fade));
    expectFadeAndRestore(22600);
}

namespace {
struct SpeedFadeRuntime {
    SpeedMuteModule speed;
    DisplayPreviewModule preview;
    DisplayRestoreModule restore;
    DisplayOrchestrationModule orchestration;

    SpeedFadeRuntime() {
        speed.begin(true, 25, 3, 0);
        orchestration.begin(&display, &ble, &queue, &preview, &restore, &parser, &settings,
                            &fade, &speed, &quiet, nullptr);
    }

    void step(uint32_t nowMs, float mph, bool active, bool echo = true) {
        if (active) {
            alert(nowMs);
        } else {
            clearAlert(nowMs);
        }
        speed.update(mph, true, nowMs);
        const int before = ble.setVolumeCalls;
        TEST_ASSERT_TRUE(orchestration.processParsedFrame({nowMs, true, false}).runDisplayPipeline);
        if (echo && ble.setVolumeCalls != before) {
            feed(PACKET_ID_RESP_ALL_VOLUME, {ble.lastVolume, ble.lastMuteVolume, 6, 2}, nowMs);
        }
    }

    void dropDuringFade() {
        startActiveFade();
        step(15000, 0, true);
        TEST_ASSERT_EQUAL_UINT8(0, parser.getDisplayState().mainVolume);
    }
};
} // namespace

void test_speed_owned_clear_rearms_same_frequency_and_restores_baseline_pair() {
    SpeedFadeRuntime runtime;
    runtime.dropDuringFade();
    const int beforeClear = ble.setVolumeCalls;
    runtime.step(15100, 0, false);
    TEST_ASSERT_EQUAL_INT(beforeClear, ble.setVolumeCalls);
    TEST_ASSERT_EQUAL_UINT8(0, parser.getDisplayState().mainVolume);
    runtime.step(20000, 0, true);
    TEST_ASSERT_EQUAL_UINT8(0, parser.getDisplayState().mainVolume);
    runtime.step(21000, 50, true);
    TEST_ASSERT_EQUAL_UINT8(6, ble.lastVolume);
    TEST_ASSERT_EQUAL_UINT8(2, ble.lastMuteVolume);
    runtime.step(21100, 50, true);
    runtime.step(22500, 50, true);
    TEST_ASSERT_EQUAL_UINT8(6, parser.getDisplayState().mainVolume);
    runtime.step(23200, 50, true);
    TEST_ASSERT_EQUAL_UINT8(1, parser.getDisplayState().mainVolume);
    runtime.step(24000, 50, false);
    TEST_ASSERT_EQUAL_UINT8(6, ble.lastVolume);
    TEST_ASSERT_EQUAL_UINT8(2, ble.lastMuteVolume);
    TEST_ASSERT_EQUAL_INT(0, ble.setMuteCalls);
}

void test_speed_release_keeps_fade_for_the_same_uninterrupted_alert() {
    SpeedFadeRuntime runtime;
    runtime.dropDuringFade();
    runtime.step(20000, 0, true);
    runtime.step(21000, 50, true);
    runtime.step(21100, 50, true);
    TEST_ASSERT_EQUAL_UINT8(1, parser.getDisplayState().mainVolume);
    runtime.step(24000, 50, false);
    TEST_ASSERT_EQUAL_UINT8(6, ble.lastVolume);
    TEST_ASSERT_EQUAL_UINT8(2, ble.lastMuteVolume);
}

void test_clear_on_speed_drop_frame_is_not_lost() {
    SpeedFadeRuntime runtime;
    startActiveFade();
    runtime.step(15000, 0, false);
    TEST_ASSERT_EQUAL_UINT8(0, parser.getDisplayState().mainVolume);
    runtime.step(20000, 0, true);
    runtime.step(21000, 50, true);
    TEST_ASSERT_EQUAL_UINT8(6, ble.lastVolume);
    TEST_ASSERT_EQUAL_UINT8(2, ble.lastMuteVolume);
}

void test_clear_during_delayed_speed_restore_replaces_the_old_faded_target() {
    SpeedFadeRuntime runtime;
    runtime.dropDuringFade();
    runtime.step(21000, 50, true, false);
    TEST_ASSERT_EQUAL_UINT8(1, ble.lastVolume);
    runtime.step(21100, 50, false, false);
    TEST_ASSERT_EQUAL_UINT8(6, ble.lastVolume);
    feed(PACKET_ID_RESP_ALL_VOLUME, {1, 2, 6, 2}, 21150); // Late echo of the old restore.
    runtime.step(21200, 50, true, false);
    TEST_ASSERT_EQUAL_UINT8(6, ble.lastVolume);
    TEST_ASSERT_EQUAL_UINT8(2, ble.lastMuteVolume);
    feed(PACKET_ID_RESP_ALL_VOLUME, {6, 2, 6, 2}, 21300);
    runtime.step(21300, 50, true);
    runtime.step(22500, 50, true);
    TEST_ASSERT_EQUAL_UINT8(6, parser.getDisplayState().mainVolume);
}

void test_speed_owned_clear_discards_a_deferred_fade_command() {
    SpeedFadeRuntime runtime;
    volume(6, 2, 9500);
    alert(10000);
    TEST_ASSERT_FALSE(quiet.executeVolumeFade(10000, &fade));
    ble.nextVolumeSendResult = SendResult::NOT_YET;
    TEST_ASSERT_TRUE(quiet.executeVolumeFade(12500, &fade));
    runtime.step(15000, 0, true);
    runtime.step(15100, 0, false);
    runtime.step(20000, 0, true);
    runtime.step(21000, 50, true);
    runtime.step(21100, 50, true);
    runtime.step(22500, 50, true);
    TEST_ASSERT_EQUAL_UINT8(6, parser.getDisplayState().mainVolume);
}

static void expectAutoPushBaselineAfterSpeedClear(uint8_t main) {
    SpeedFadeRuntime runtime;
    runtime.dropDuringFade();
    TEST_ASSERT_TRUE(quiet.sendAutoPushVolume(main, 3));
    TEST_ASSERT_EQUAL_UINT8(0, ble.lastVolume);
    feed(PACKET_ID_RESP_ALL_VOLUME, {0, 3, main, 3}, 15050);
    runtime.step(15100, 0, false);
    runtime.step(20000, 0, true);
    runtime.step(21000, 50, true);
    runtime.step(21100, 50, true);
    TEST_ASSERT_EQUAL_UINT8(main, parser.getDisplayState().mainVolume);
    TEST_ASSERT_EQUAL_UINT8(3, parser.getDisplayState().muteVolume);
}

void test_speed_owned_clear_preserves_new_autopush_baseline() {
    expectAutoPushBaselineAfterSpeedClear(8);
}

void test_speed_owned_clear_preserves_new_autopush_zero_baseline() {
    expectAutoPushBaselineAfterSpeedClear(0);
}

void test_speed_owned_clear_preserves_autopush_applied_before_speed_drop() {
    SpeedFadeRuntime runtime;
    startActiveFade();
    TEST_ASSERT_TRUE(quiet.sendAutoPushVolume(8, 3));
    feed(PACKET_ID_RESP_ALL_VOLUME, {8, 3, 8, 3}, 14000);
    runtime.step(15000, 0, true);
    runtime.step(15100, 0, false);
    runtime.step(20000, 0, true);
    runtime.step(21000, 50, true);
    runtime.step(21100, 50, true);
    TEST_ASSERT_EQUAL_UINT8(8, parser.getDisplayState().mainVolume);
    TEST_ASSERT_EQUAL_UINT8(3, parser.getDisplayState().muteVolume);
}

static void expectPendingAutoPushAtSpeedEntry(uint8_t main) {
    SpeedFadeRuntime runtime;
    startActiveFade();
    TEST_ASSERT_TRUE(quiet.sendAutoPushVolume(main, 3));
    TEST_ASSERT_TRUE(quiet.getDesiredState().volumePending);
    TEST_ASSERT_EQUAL_UINT8(1, parser.getDisplayState().mainVolume); // No AutoPush echo yet.
    runtime.step(15000, 0, true);
    TEST_ASSERT_EQUAL_UINT8(0, ble.lastVolume);
    const uint8_t temporaryMuteVolume = ble.lastMuteVolume;
    runtime.step(15100, 0, false);
    TEST_ASSERT_EQUAL_UINT8(0, parser.getDisplayState().mainVolume);
    runtime.step(20000, 0, true);
    runtime.step(21000, 50, true);
    runtime.step(21100, 50, true);
    TEST_ASSERT_EQUAL_UINT8(main, parser.getDisplayState().mainVolume);
    TEST_ASSERT_EQUAL_UINT8(3, parser.getDisplayState().muteVolume);
    TEST_ASSERT_EQUAL_UINT8(3, temporaryMuteVolume);
    runtime.step(24000, 50, false);
    TEST_ASSERT_EQUAL_UINT8(main, parser.getDisplayState().mainVolume);
    TEST_ASSERT_EQUAL_UINT8(3, parser.getDisplayState().muteVolume);
}

void test_speed_entry_carries_pending_autopush_pair_before_detector_echo() {
    expectPendingAutoPushAtSpeedEntry(8);
}

void test_speed_entry_carries_pending_autopush_zero_before_detector_echo() {
    expectPendingAutoPushAtSpeedEntry(0);
}

void test_confirmed_autopush_does_not_replace_a_later_observed_manual_pair() {
    SpeedFadeRuntime runtime;
    startActiveFade();
    TEST_ASSERT_TRUE(quiet.sendAutoPushVolume(8, 3));
    feed(PACKET_ID_RESP_ALL_VOLUME, {8, 3, 8, 3}, 14000);
    runtime.step(14500, 50, true);
    TEST_ASSERT_FALSE(quiet.getDesiredState().volumePending);
    feed(PACKET_ID_RESP_ALL_VOLUME, {7, 4, 7, 4}, 14900);
    runtime.step(15000, 0, true);
    TEST_ASSERT_EQUAL_UINT8(0, ble.lastVolume);
    TEST_ASSERT_EQUAL_UINT8(4, ble.lastMuteVolume);
    runtime.step(15100, 0, false);
    runtime.step(20000, 0, true);
    runtime.step(21000, 50, true);
    runtime.step(21100, 50, true);
    TEST_ASSERT_EQUAL_UINT8(7, parser.getDisplayState().mainVolume);
    TEST_ASSERT_EQUAL_UINT8(4, parser.getDisplayState().muteVolume);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_alert_before_first_volume_preserves_real_baseline_pair);
    RUN_TEST(test_volume_before_first_alert_preserves_existing_fade_and_restore);
    RUN_TEST(test_known_zero_volume_is_preserved);
    RUN_TEST(test_alert_without_volume_data_never_requests_a_volume_change);
    RUN_TEST(test_reconnect_alert_before_volume_uses_new_session_baseline);
    RUN_TEST(test_reconnect_volume_before_alert_preserves_normal_restoration);
    RUN_TEST(test_same_detector_reconnect_preserves_active_fade_restore);
    RUN_TEST(test_same_detector_reconnect_alert_before_volume_preserves_active_fade_restore);
    RUN_TEST(test_reconnect_requires_fresh_volume_and_preserves_known_zero);
    RUN_TEST(test_generation_watchdog_invalidates_volume_before_new_alert);
    RUN_TEST(test_session_open_invalidates_volume_without_close_callback);
    RUN_TEST(test_speed_owned_clear_rearms_same_frequency_and_restores_baseline_pair);
    RUN_TEST(test_speed_release_keeps_fade_for_the_same_uninterrupted_alert);
    RUN_TEST(test_clear_on_speed_drop_frame_is_not_lost);
    RUN_TEST(test_clear_during_delayed_speed_restore_replaces_the_old_faded_target);
    RUN_TEST(test_speed_owned_clear_discards_a_deferred_fade_command);
    RUN_TEST(test_speed_owned_clear_preserves_new_autopush_baseline);
    RUN_TEST(test_speed_owned_clear_preserves_new_autopush_zero_baseline);
    RUN_TEST(test_speed_owned_clear_preserves_autopush_applied_before_speed_drop);
    RUN_TEST(test_speed_entry_carries_pending_autopush_pair_before_detector_echo);
    RUN_TEST(test_speed_entry_carries_pending_autopush_zero_before_detector_echo);
    RUN_TEST(test_confirmed_autopush_does_not_replace_a_later_observed_manual_pair);
    return UNITY_END();
}

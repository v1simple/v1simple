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
#define PACKET_ID_REQ_CURRENT_VOLUME 0x37
#define PACKET_ID_RESP_CURRENT_VOLUME 0x38
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

void openSession(uint32_t generation, uint32_t nowMs, const char* address = "AA:BB:CC:DD:EE:01") {
    mockMillis = nowMs;
    ble.setConnected(true);
    ble.setSessionGeneration(generation);
    // Same owner entrypoint used by DriveRuntime before opening parser intake.
    quiet.beginDetectorVolumeSession(address, &fade);
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

void feed(uint8_t id, std::initializer_list<uint8_t> data, uint32_t nowMs, uint32_t queuedIngress = 0) {
    // V1 -> remote. ESP length includes the checksum; the end marker does not.
    const uint8_t destination =
        (id == PACKET_ID_DISPLAY_DATA || id == PACKET_ID_ALERT_DATA) ? 0xD8 : 0xD6;
    std::vector<uint8_t> packet{0xAA, destination, 0xEA, id,
                                static_cast<uint8_t>(data.size() + 1)};
    packet.insert(packet.end(), data.begin(), data.end());
    uint8_t checksum = 0;
    for (uint8_t value : packet) {
        checksum = static_cast<uint8_t>(checksum + value);
    }
    packet.push_back(checksum);
    packet.push_back(0xAB);
    mockMillis = nowMs;
    TEST_ASSERT_TRUE(parser.parse(packet.data(), packet.size(), nowMs,
                                  queuedIngress ? queuedIngress : ble.noteV1NotificationIngress()));
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
        orchestration.begin(&display, &ble, &preview, &restore, &parser, &fade, &speed, &quiet);
    }

    void step(uint32_t nowMs, float mph, bool active, bool echo = true) {
        if (active) {
            alert(nowMs);
        } else {
            clearAlert(nowMs);
        }
        speed.update(mph, true, nowMs);
        const int before = ble.setVolumeCalls;
        TEST_ASSERT_TRUE(orchestration.processParsedFrame({nowMs, true, false}));
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

void test_coalesced_clear_and_same_frequency_rearms_fade() {
    SpeedFadeRuntime runtime;
    startActiveFade();
    const uint32_t lifetime = parser.alertLifetime();
    // Both publications reach the real parser before one normal owner pass.
    clearAlert(13000);
    alert(13000);
    TEST_ASSERT_EQUAL_UINT32(lifetime + 2, parser.alertLifetime());
    const int before = ble.setVolumeCalls;
    runtime.step(13000, 50, true);
    TEST_ASSERT_EQUAL_INT(before + 1, ble.setVolumeCalls);
    TEST_ASSERT_EQUAL_UINT8(6, ble.lastVolume);
    TEST_ASSERT_EQUAL_UINT8(2, ble.lastMuteVolume);
    runtime.step(14999, 50, true);
    TEST_ASSERT_EQUAL_UINT8(6, parser.getDisplayState().mainVolume);
    runtime.step(15000, 50, true);
    TEST_ASSERT_EQUAL_UINT8(1, parser.getDisplayState().mainVolume);
    runtime.step(16000, 50, false);
    TEST_ASSERT_EQUAL_UINT8(6, ble.lastVolume);
    TEST_ASSERT_EQUAL_UINT8(2, ble.lastMuteVolume);
}

void test_coalesced_clear_during_speed_restores_baseline_and_rearms_fade() {
    SpeedFadeRuntime runtime;
    runtime.dropDuringFade();
    const uint32_t lifetime = parser.alertLifetime();
    clearAlert(15100);
    alert(15100);
    TEST_ASSERT_EQUAL_UINT32(lifetime + 2, parser.alertLifetime());
    const int before = ble.setVolumeCalls;
    runtime.step(15100, 0, true);
    TEST_ASSERT_EQUAL_INT(before, ble.setVolumeCalls);
    TEST_ASSERT_EQUAL_UINT8(0, parser.getDisplayState().mainVolume);
    runtime.step(21000, 50, true);
    TEST_ASSERT_EQUAL_UINT8(6, ble.lastVolume);
    TEST_ASSERT_EQUAL_UINT8(2, ble.lastMuteVolume);
    runtime.step(21100, 50, true);
    runtime.step(23099, 50, true);
    TEST_ASSERT_EQUAL_UINT8(6, parser.getDisplayState().mainVolume);
    runtime.step(23100, 50, true);
    TEST_ASSERT_EQUAL_UINT8(1, parser.getDisplayState().mainVolume);
    runtime.step(24000, 50, false);
    TEST_ASSERT_EQUAL_UINT8(6, ble.lastVolume);
    TEST_ASSERT_EQUAL_UINT8(2, ble.lastMuteVolume);
    TEST_ASSERT_EQUAL_INT(0, ble.setMuteCalls);
}

void test_coalesced_clear_before_fade_restarts_the_delay() {
    SpeedFadeRuntime runtime;
    volume(6, 2, 9500);
    runtime.step(10000, 50, true);
    clearAlert(11900);
    runtime.step(11900, 50, true);
    runtime.step(13899, 50, true);
    TEST_ASSERT_EQUAL_INT(0, ble.setVolumeCalls);
    runtime.step(13900, 50, true);
    TEST_ASSERT_EQUAL_UINT8(1, ble.lastVolume);
    TEST_ASSERT_EQUAL_UINT8(2, ble.lastMuteVolume);
}

void test_coalesced_clear_preserves_known_zero_baseline() {
    SpeedFadeRuntime runtime;
    volume(0, 2, 9500);
    runtime.step(10000, 50, true);
    runtime.step(12500, 50, true);
    clearAlert(13000);
    runtime.step(13000, 50, true);
    runtime.step(15000, 50, true);
    runtime.step(16000, 50, false);
    TEST_ASSERT_EQUAL_INT(0, ble.setVolumeCalls);
    TEST_ASSERT_EQUAL_UINT8(0, parser.getDisplayState().mainVolume);
    TEST_ASSERT_EQUAL_UINT8(2, parser.getDisplayState().muteVolume);
}

void test_coalesced_clear_cancels_deferred_fade_and_confirms_restore_pair() {
    SpeedFadeRuntime runtime;
    volume(6, 2, 9500);
    runtime.step(10000, 50, true);
    ble.nextVolumeSendResult = SendResult::NOT_YET;
    runtime.step(12500, 50, true, false);
    TEST_ASSERT_EQUAL_UINT8(1, ble.lastVolume);
    clearAlert(12510);
    ble.nextVolumeSendResult = SendResult::NOT_YET;
    runtime.step(12510, 50, true, false);
    TEST_ASSERT_EQUAL_INT(2, ble.setVolumeCalls);
    TEST_ASSERT_EQUAL_UINT8(6, ble.lastVolume);
    TEST_ASSERT_EQUAL_UINT8(2, ble.lastMuteVolume);
    TEST_ASSERT_FALSE(quiet.canApplyAutoPushVolumeExactly());
    runtime.step(12520, 50, true, false);
    TEST_ASSERT_EQUAL_INT(2, ble.setVolumeCalls);
    // Both earlier commands were deferred. Cached baseline equality cannot
    // release the obligation; the next accepted restore gets a fresh echo.
    runtime.step(12585, 50, true);
    TEST_ASSERT_EQUAL_INT(3, ble.setVolumeCalls);
    TEST_ASSERT_EQUAL_UINT8(6, ble.lastVolume);
    runtime.step(14509, 50, true);
    TEST_ASSERT_EQUAL_INT(3, ble.setVolumeCalls);
    runtime.step(14510, 50, true);
    TEST_ASSERT_EQUAL_UINT8(1, ble.lastVolume);
}

void test_repeated_coalesced_clear_retains_unconfirmed_restore_pair() {
    SpeedFadeRuntime runtime;
    startActiveFade();
    clearAlert(13000);
    runtime.step(13000, 50, true, false);
    TEST_ASSERT_EQUAL_UINT8(6, ble.lastVolume);
    TEST_ASSERT_EQUAL_UINT8(1, parser.getDisplayState().mainVolume);
    clearAlert(13100);
    runtime.step(13100, 50, true, false);
    TEST_ASSERT_EQUAL_UINT8(6, ble.lastVolume);
    TEST_ASSERT_EQUAL_UINT8(2, ble.lastMuteVolume);
    TEST_ASSERT_FALSE(quiet.canApplyAutoPushVolumeExactly());
    feed(PACKET_ID_RESP_ALL_VOLUME, {6, 2, 6, 2}, 13200);
    runtime.step(15099, 50, true);
    TEST_ASSERT_EQUAL_UINT8(6, parser.getDisplayState().mainVolume);
    runtime.step(15100, 50, true);
    TEST_ASSERT_EQUAL_UINT8(1, parser.getDisplayState().mainVolume);
    runtime.step(16000, 50, false);
    TEST_ASSERT_EQUAL_UINT8(6, ble.lastVolume);
    TEST_ASSERT_EQUAL_UINT8(2, ble.lastMuteVolume);
}

void test_coalesced_clear_during_delayed_speed_restore_replaces_faded_target() {
    SpeedFadeRuntime runtime;
    runtime.dropDuringFade();
    runtime.step(21000, 50, true, false);
    TEST_ASSERT_EQUAL_UINT8(1, ble.lastVolume);
    clearAlert(21100);
    runtime.step(21100, 50, true, false);
    TEST_ASSERT_EQUAL_UINT8(6, ble.lastVolume);
    feed(PACKET_ID_RESP_ALL_VOLUME, {1, 2, 6, 2}, 21150);
    runtime.step(21200, 50, true, false);
    TEST_ASSERT_EQUAL_UINT8(6, ble.lastVolume);
    TEST_ASSERT_EQUAL_UINT8(2, ble.lastMuteVolume);
    feed(PACKET_ID_RESP_ALL_VOLUME, {6, 2, 6, 2}, 21300);
    runtime.step(21300, 50, true);
    runtime.step(23299, 50, true);
    TEST_ASSERT_EQUAL_UINT8(6, parser.getDisplayState().mainVolume);
    runtime.step(23300, 50, true);
    TEST_ASSERT_EQUAL_UINT8(1, parser.getDisplayState().mainVolume);
}

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

static void expectAutoPushBaselineAfterSpeedClear(uint8_t main, bool coalesced = false) {
    SpeedFadeRuntime runtime;
    runtime.dropDuringFade();
    TEST_ASSERT_TRUE(quiet.sendAutoPushVolume(main, 3));
    TEST_ASSERT_EQUAL_UINT8(0, ble.lastVolume);
    feed(PACKET_ID_RESP_ALL_VOLUME, {0, 3, main, 3}, 15050);
    if (coalesced) clearAlert(15100);
    runtime.step(15100, 0, coalesced);
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

void test_coalesced_speed_clear_preserves_new_autopush_baseline() {
    expectAutoPushBaselineAfterSpeedClear(8, true);
}

void test_coalesced_speed_clear_preserves_new_autopush_zero_baseline() {
    expectAutoPushBaselineAfterSpeedClear(0, true);
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

void test_fade_restore_ownership_releases_only_after_detector_convergence() {
    volume(6, 2, 9500);
    alert(10000);
    TEST_ASSERT_FALSE(quiet.executeVolumeFade(10000, &fade));
    alert(12500);
    TEST_ASSERT_TRUE(quiet.executeVolumeFade(12500, &fade));
    feed(PACKET_ID_RESP_ALL_VOLUME, {1, 2, 6, 2}, 12600);
    TEST_ASSERT_FALSE(quiet.executeVolumeFade(12600, &fade));
    clearAlert(14000);
    TEST_ASSERT_TRUE(quiet.executeVolumeFade(14000, &fade));
    TEST_ASSERT_FALSE(quiet.canApplyAutoPushVolumeExactly());

    feed(PACKET_ID_RESP_ALL_VOLUME, {6, 2, 6, 2}, 14100);
    TEST_ASSERT_FALSE(quiet.executeVolumeFade(14100, &fade));
    TEST_ASSERT_TRUE(quiet.canApplyAutoPushVolumeExactly());
}

void test_failed_fade_down_retains_ownership_until_override_lifecycle_clears() {
    volume(6, 2, 9500);
    alert(10000);
    TEST_ASSERT_FALSE(quiet.executeVolumeFade(10000, &fade));

    ble.nextVolumeSendResult = SendResult::FAILED;
    alert(12500);
    TEST_ASSERT_TRUE(quiet.executeVolumeFade(12500, &fade));
    TEST_ASSERT_FALSE(quiet.canApplyAutoPushVolumeExactly());

    // The unsuccessful fade never changed the detector, but VolumeFade still
    // retains its baseline and can issue a later restore. Apply stays excluded
    // until that lifecycle observes the canonical original pair and clears.
    clearAlert(14000);
    TEST_ASSERT_TRUE(quiet.executeVolumeFade(14000, &fade));
    TEST_ASSERT_FALSE(quiet.canApplyAutoPushVolumeExactly());
    volume(6, 2, 14100);
    TEST_ASSERT_FALSE(quiet.executeVolumeFade(14100, &fade));
    TEST_ASSERT_TRUE(quiet.canApplyAutoPushVolumeExactly());
}

void test_not_yet_fade_down_retains_ownership_until_override_lifecycle_clears() {
    volume(6, 2, 9500);
    alert(10000);
    TEST_ASSERT_FALSE(quiet.executeVolumeFade(10000, &fade));

    ble.nextVolumeSendResult = SendResult::NOT_YET;
    alert(12500);
    TEST_ASSERT_TRUE(quiet.executeVolumeFade(12500, &fade));
    TEST_ASSERT_FALSE(quiet.canApplyAutoPushVolumeExactly());

    // Clearing the alert cancels the deferred fade-down and starts the retained
    // baseline restore. Merely sending that restore does not release ownership.
    clearAlert(14000);
    TEST_ASSERT_TRUE(quiet.executeVolumeFade(14000, &fade));
    TEST_ASSERT_FALSE(quiet.canApplyAutoPushVolumeExactly());
    volume(6, 2, 14100);
    TEST_ASSERT_FALSE(quiet.executeVolumeFade(14100, &fade));
    TEST_ASSERT_TRUE(quiet.canApplyAutoPushVolumeExactly());
}

void test_different_detector_does_not_receive_old_fade_restore() {
    startActiveFade();
    closeSession(2, 14000);
    openSession(3, 20000, "AA:BB:CC:DD:EE:02");
    volume(0, 0, 20100);
    const int before = ble.setVolumeCalls;
    TEST_ASSERT_FALSE(quiet.executeVolumeFade(20100, &fade));
    TEST_ASSERT_EQUAL_INT(before, ble.setVolumeCalls);
    TEST_ASSERT_TRUE(quiet.canApplyAutoPushVolumeExactly());
    TEST_ASSERT_EQUAL_UINT8(0, parser.getDisplayState().mainVolume);

    // The replacement detector can start an ordinary fade with its own pair.
    volume(4, 3, 20200);
    alert(20300);
    TEST_ASSERT_FALSE(quiet.executeVolumeFade(20300, &fade));
    TEST_ASSERT_TRUE(quiet.executeVolumeFade(23000, &fade));
    clearAlert(24000);
    TEST_ASSERT_TRUE(quiet.executeVolumeFade(24000, &fade));
    TEST_ASSERT_EQUAL_UINT8(4, ble.lastVolume);
    TEST_ASSERT_EQUAL_UINT8(3, ble.lastMuteVolume);
}

void test_unknown_detector_discards_deferred_fade_and_prior_identity() {
    volume(6, 2, 9500);
    alert(10000);
    TEST_ASSERT_FALSE(quiet.executeVolumeFade(10000, &fade));
    ble.nextVolumeSendResult = SendResult::NOT_YET;
    TEST_ASSERT_TRUE(quiet.executeVolumeFade(12500, &fade));
    closeSession(2, 14000);
    openSession(3, 20000, nullptr);
    volume(0, 0, 20100);
    const int before = ble.setVolumeCalls;
    TEST_ASSERT_FALSE(quiet.executeVolumeFade(20100, &fade));
    TEST_ASSERT_EQUAL_INT(before, ble.setVolumeCalls);
    closeSession(4, 21000);
    openSession(5, 22000); // Knowing the original address again cannot revive it.
    volume(0, 0, 22100);
    TEST_ASSERT_FALSE(quiet.executeVolumeFade(22100, &fade));
    TEST_ASSERT_EQUAL_INT(before, ble.setVolumeCalls);
}

void test_different_detector_discards_speed_restore_baseline() {
    SpeedFadeRuntime runtime;
    runtime.dropDuringFade();
    closeSession(2, 16000);
    openSession(3, 20000, "AA:BB:CC:DD:EE:02");
    volume(0, 0, 20100);
    const int before = ble.setVolumeCalls;
    runtime.step(21000, 50, false);
    TEST_ASSERT_EQUAL_INT(before, ble.setVolumeCalls);
    TEST_ASSERT_EQUAL_UINT8(0, parser.getDisplayState().mainVolume);
}

void test_proxy_rejected_speed_drop_cannot_restore_over_phone_choice() {
    SpeedFadeRuntime runtime;
    volume(6, 2, 10000);
    ble.setProxyConnected(true);
    // Production transport rejects local writes while a phone owns the V1.
    ble.nextVolumeSendResult = SendResult::FAILED;
    runtime.step(11000, 0, false, false);
    volume(3, 1, 12000);
    runtime.step(12000, 50, false, false);
    TEST_ASSERT_EQUAL_INT(0, ble.setVolumeCalls);
    ble.setProxyConnected(false);
    ble.nextVolumeSendResult = SendResult::SENT;
    runtime.step(12500, 50, false, false);
    TEST_ASSERT_EQUAL_INT(0, ble.setVolumeCalls);
    TEST_ASSERT_EQUAL_UINT8(3, parser.getDisplayState().mainVolume);
    TEST_ASSERT_EQUAL_UINT8(1, parser.getDisplayState().muteVolume);

    // A later local low-speed episode captures and restores the phone's pair.
    runtime.step(15000, 0, false);
    TEST_ASSERT_EQUAL_UINT8(0, ble.lastVolume);
    TEST_ASSERT_EQUAL_UINT8(1, ble.lastMuteVolume);
    runtime.step(17000, 50, false);
    TEST_ASSERT_EQUAL_UINT8(3, ble.lastVolume);
    TEST_ASSERT_EQUAL_UINT8(1, ble.lastMuteVolume);
}

void test_proxy_takeover_cancels_pending_restore_without_a_parsed_frame() {
    SpeedFadeRuntime runtime;
    runtime.dropDuringFade();
    runtime.step(21000, 50, false, false);
    TEST_ASSERT_FALSE(quiet.canApplyAutoPushVolumeExactly());
    ble.setProxyConnected(true);
    const int before = ble.setVolumeCalls;
    // DriveRuntime services this owner immediately after BLE callbacks, even
    // when an interactive owner prevents any parsed-frame dispatch.
    TEST_ASSERT_TRUE(quiet.syncProxyVolumeOwnership(&fade));
    TEST_ASSERT_FALSE(runtime.orchestration.processParsedFrame({21100, false, false}));
    TEST_ASSERT_TRUE(quiet.canApplyAutoPushVolumeExactly());
    ble.setProxyConnected(false);
    volume(3, 1, 21200);
    runtime.step(21300, 50, false, false);
    TEST_ASSERT_EQUAL_INT(before, ble.setVolumeCalls);
}

void test_proxy_takeover_discards_active_fade_without_replaying_after_disconnect() {
    SpeedFadeRuntime runtime;
    startActiveFade();
    ble.setProxyConnected(true);
    const int before = ble.setVolumeCalls;
    TEST_ASSERT_TRUE(quiet.syncProxyVolumeOwnership(&fade));
    TEST_ASSERT_FALSE(runtime.orchestration.processParsedFrame({13000, false, false}));
    ble.setProxyConnected(false);
    volume(3, 1, 13100);
    clearAlert(13200);
    TEST_ASSERT_FALSE(quiet.executeVolumeFade(13200, &fade));
    TEST_ASSERT_EQUAL_INT(before, ble.setVolumeCalls);
    TEST_ASSERT_TRUE(quiet.canApplyAutoPushVolumeExactly());
}

void test_speed_drop_not_yet_retries_and_restores_original_pair() {
    SpeedFadeRuntime runtime;
    volume(6, 2, 10000);
    ble.nextVolumeSendResult = SendResult::NOT_YET;
    runtime.step(11000, 0, false, false);
    TEST_ASSERT_EQUAL_INT(1, ble.setVolumeCalls);
    runtime.step(11050, 0, false, false);
    TEST_ASSERT_EQUAL_INT(1, ble.setVolumeCalls);
    runtime.step(11100, 0, false);
    TEST_ASSERT_EQUAL_INT(2, ble.setVolumeCalls);
    TEST_ASSERT_EQUAL_UINT8(0, parser.getDisplayState().mainVolume);
    runtime.step(14000, 50, false);
    TEST_ASSERT_EQUAL_UINT8(6, ble.lastVolume);
    TEST_ASSERT_EQUAL_UINT8(2, ble.lastMuteVolume);
}

void test_speed_restore_survives_long_holdoff_and_requires_complete_pair() {
    SpeedFadeRuntime runtime;
    volume(6, 2, 10000);
    runtime.step(11000, 0, false);
    for (uint32_t now = 14000; now <= 17000; now += 100) {
        ble.nextVolumeSendResult = SendResult::NOT_YET;
        runtime.step(now, 50, false, false);
    }
    TEST_ASSERT_FALSE(quiet.canApplyAutoPushVolumeExactly());
    const int before = ble.setVolumeCalls;
    runtime.step(17025, 50, false, false);
    TEST_ASSERT_EQUAL_INT(before, ble.setVolumeCalls);
    runtime.step(17100, 50, false, false);
    TEST_ASSERT_EQUAL_INT(before + 1, ble.setVolumeCalls);
    TEST_ASSERT_EQUAL_UINT8(6, ble.lastVolume);
    TEST_ASSERT_EQUAL_UINT8(2, ble.lastMuteVolume);

    volume(6, 1, 17200);
    runtime.step(17200, 50, false, false);
    TEST_ASSERT_FALSE(quiet.canApplyAutoPushVolumeExactly());
    TEST_ASSERT_EQUAL_UINT8(2, ble.lastMuteVolume);
    volume(6, 2, 17300);
    runtime.step(17300, 50, false, false);
    TEST_ASSERT_TRUE(quiet.canApplyAutoPushVolumeExactly());
    const int confirmed = ble.setVolumeCalls;
    runtime.step(20000, 50, false, false);
    TEST_ASSERT_EQUAL_INT(confirmed, ble.setVolumeCalls);
    TEST_ASSERT_EQUAL_INT(0, ble.setMuteCalls);
}

void test_speed_reentry_preserves_unconfirmed_restore_pair() {
    SpeedFadeRuntime runtime;
    volume(6, 2, 10000);
    runtime.step(11000, 0, false);
    ble.nextVolumeSendResult = SendResult::NOT_YET;
    runtime.step(14000, 50, false, false);
    runtime.step(15500, 0, false);
    TEST_ASSERT_TRUE(runtime.speed.getState().muteActive);
    runtime.step(18000, 50, false, false);
    TEST_ASSERT_EQUAL_UINT8(6, ble.lastVolume);
    TEST_ASSERT_EQUAL_UINT8(2, ble.lastMuteVolume);
}

void test_live_new_threat_restore_retries_until_complete_pair_after_long_holdoff() {
    startActiveFade();
    // New Ka priority, 35.500 GHz. Transport acceptance does not update the
    // production parser's canonical pair: model an unapplied detector write.
    feed(PACKET_ID_ALERT_DATA, {0x11, 0x8A, 0xAC, 0xAC, 0x00, 0x22, 0x80}, 13000);
    TEST_ASSERT_TRUE(quiet.executeVolumeFade(13000, &fade));
    TEST_ASSERT_EQUAL_UINT8(6, ble.lastVolume);
    const int firstRestore = ble.setVolumeCalls;
    quiet.executeVolumeFade(13010, &fade);
    TEST_ASSERT_EQUAL_INT(firstRestore, ble.setVolumeCalls);
    for (uint32_t now = 13100; now <= 17000; now += 100) {
        ble.nextVolumeSendResult = SendResult::NOT_YET;
        quiet.executeVolumeFade(now, &fade);
    }
    TEST_ASSERT_GREATER_THAN_INT(firstRestore, ble.setVolumeCalls);
    TEST_ASSERT_TRUE(fade.hasActiveVolumeOverride());
    TEST_ASSERT_FALSE(quiet.canApplyAutoPushVolumeExactly());
    ble.nextVolumeSendResult = SendResult::SENT;
    TEST_ASSERT_TRUE(quiet.executeVolumeFade(17100, &fade));
    TEST_ASSERT_EQUAL_UINT8(6, ble.lastVolume);
    TEST_ASSERT_EQUAL_UINT8(2, ble.lastMuteVolume);

    volume(6, 1, 17200); // Main alone is not convergence of the saved pair.
    TEST_ASSERT_TRUE(quiet.executeVolumeFade(17200, &fade));
    TEST_ASSERT_EQUAL_UINT8(6, ble.lastVolume);
    TEST_ASSERT_EQUAL_UINT8(2, ble.lastMuteVolume);
    clearAlert(17300);
    TEST_ASSERT_TRUE(quiet.executeVolumeFade(17300, &fade));
    volume(6, 2, 17400);
    TEST_ASSERT_FALSE(quiet.executeVolumeFade(17400, &fade));
    TEST_ASSERT_FALSE(fade.hasActiveVolumeOverride());
    TEST_ASSERT_TRUE(quiet.canApplyAutoPushVolumeExactly());
    const int confirmed = ble.setVolumeCalls;
    quiet.executeVolumeFade(20000, &fade);
    TEST_ASSERT_EQUAL_INT(confirmed, ble.setVolumeCalls);
    TEST_ASSERT_EQUAL_INT(0, ble.setMuteCalls);
}

void test_invalid_speed_restore_survives_same_detector_reconnect_without_default_confirmation() {
    SpeedFadeRuntime runtime;
    volume(6, 2, 10000);
    runtime.step(11000, 0, false);
    runtime.speed.update(0, false, 11100);
    ble.nextVolumeSendResult = SendResult::FAILED;
    TEST_ASSERT_TRUE(quiet.processSpeedVolume(11100, runtime.speed, &fade));
    closeSession(2, 11200);
    openSession(3, 20000);
    const int before = ble.setVolumeCalls;
    TEST_ASSERT_TRUE(quiet.processSpeedVolume(20000, runtime.speed, &fade));
    TEST_ASSERT_EQUAL_INT(before, ble.setVolumeCalls);
    TEST_ASSERT_FALSE(quiet.canApplyAutoPushVolumeExactly());
    volume(0, 2, 20100);
    TEST_ASSERT_TRUE(quiet.processSpeedVolume(20100, runtime.speed, &fade));
    TEST_ASSERT_EQUAL_UINT8(6, ble.lastVolume);
    TEST_ASSERT_EQUAL_UINT8(2, ble.lastMuteVolume);
    volume(6, 2, 20200);
    TEST_ASSERT_FALSE(quiet.processSpeedVolume(20200, runtime.speed, &fade));
    TEST_ASSERT_TRUE(quiet.canApplyAutoPushVolumeExactly());
}

void test_new_profile_supersedes_long_pending_speed_restore_including_zero() {
    for (uint8_t target : {uint8_t(8), uint8_t(0)}) {
        setUp();
        SpeedFadeRuntime runtime;
        volume(6, 2, 10000);
        runtime.step(11000, 0, false);
        ble.nextVolumeSendResult = SendResult::NOT_YET;
        runtime.step(14000, 50, false, false);
        TEST_ASSERT_TRUE(quiet.sendAutoPushVolume(target, 3));
        runtime.step(18000, 50, false, false);
        TEST_ASSERT_EQUAL_UINT8(target, ble.lastVolume);
        TEST_ASSERT_EQUAL_UINT8(3, ble.lastMuteVolume);
        volume(target, 3, 18100);
        runtime.step(18100, 50, false, false);
        TEST_ASSERT_TRUE(quiet.canApplyAutoPushVolumeExactly());
    }
}

void test_delayed_live_restore_sounds_before_refade_and_then_honors_new_profile() {
    startActiveFade();
    feed(PACKET_ID_ALERT_DATA, {0x11, 0x8A, 0xAC, 0xAC, 0x00, 0x22, 0x80}, 13000);
    ble.nextVolumeSendResult = SendResult::FAILED;
    TEST_ASSERT_TRUE(quiet.executeVolumeFade(13000, &fade));
    TEST_ASSERT_TRUE(quiet.executeVolumeFade(17000, &fade));
    TEST_ASSERT_EQUAL_UINT8(6, ble.lastVolume);
    volume(6, 2, 17100);
    const int confirmed = ble.setVolumeCalls;
    TEST_ASSERT_FALSE(quiet.executeVolumeFade(17100, &fade));
    TEST_ASSERT_FALSE(quiet.executeVolumeFade(19099, &fade));
    TEST_ASSERT_EQUAL_INT(confirmed, ble.setVolumeCalls);
    TEST_ASSERT_TRUE(quiet.executeVolumeFade(19100, &fade));
    TEST_ASSERT_EQUAL_UINT8(1, ble.lastVolume);
    volume(1, 2, 19200);
    clearAlert(20000);
    TEST_ASSERT_TRUE(quiet.executeVolumeFade(20000, &fade));
    volume(6, 2, 20100);
    TEST_ASSERT_FALSE(quiet.executeVolumeFade(20100, &fade));
    TEST_ASSERT_TRUE(quiet.beginAutoPushVolumeTransaction());
    TEST_ASSERT_TRUE(quiet.sendAutoPushVolume(0, 3));
    volume(0, 3, 20200);
    quiet.endAutoPushVolumeTransaction();
    alert(21000);
    TEST_ASSERT_FALSE(quiet.executeVolumeFade(21000, &fade));
    TEST_ASSERT_FALSE(quiet.executeVolumeFade(24000, &fade));
    clearAlert(25000);
    TEST_ASSERT_FALSE(quiet.executeVolumeFade(25000, &fade));
    TEST_ASSERT_EQUAL_UINT8(0, parser.getDisplayState().mainVolume);
    TEST_ASSERT_EQUAL_UINT8(3, parser.getDisplayState().muteVolume);
}

void test_pending_new_threat_restore_yields_to_proxy_and_changed_detector() {
    for (bool proxy : {false, true}) {
        setUp();
        startActiveFade();
        feed(PACKET_ID_ALERT_DATA, {0x11, 0x8A, 0xAC, 0xAC, 0x00, 0x22, 0x80}, 13000);
        TEST_ASSERT_TRUE(quiet.executeVolumeFade(13000, &fade));
        const int before = ble.setVolumeCalls;
        if (proxy) {
            ble.setProxyConnected(true);
            TEST_ASSERT_TRUE(quiet.syncProxyVolumeOwnership(&fade));
            ble.setProxyConnected(false);
        } else {
            closeSession(2, 14000);
            openSession(3, 20000, "AA:BB:CC:DD:EE:02");
        }
        volume(3, 1, 20100);
        clearAlert(20200);
        TEST_ASSERT_FALSE(quiet.executeVolumeFade(20200, &fade));
        TEST_ASSERT_EQUAL_INT(before, ble.setVolumeCalls);
        TEST_ASSERT_FALSE(fade.hasActiveVolumeOverride());
        TEST_ASSERT_TRUE(quiet.canApplyAutoPushVolumeExactly());
    }
}

// A detector can echo the reduction after policy has already released it. Use
// the real parsed-frame owner and canonical packets; BLE delivery is the first
// mocked boundary. Both ordinary feedback and the delayed-feedback schedule
// must retain the restore through SENT, NOT_YET and FAILED transport outcomes.
void test_delayed_reduction_feedback_cannot_confirm_an_unsent_restore() {
    for (bool speedOwner : {false, true}) {
        for (bool dropObserved : {false, true}) {
            for (SendResult restoreResult : {SendResult::SENT, SendResult::NOT_YET, SendResult::FAILED}) {
                setUp();
                SpeedFadeRuntime runtime;
                volume(6, 2, 10000);
                if (speedOwner) {
                    runtime.step(11000, 0, false, false);
                } else {
                    runtime.step(10000, 50, true, false);
                    runtime.step(12500, 50, true, false);
                }
                TEST_ASSERT_EQUAL_UINT8(speedOwner ? 0 : 1, ble.lastVolume);
                TEST_ASSERT_EQUAL_INT(1, ble.setVolumeCalls);
                if (dropObserved) volume(speedOwner ? 0 : 1, 2, 12501);
                // This original pair entered before restoration. Parse it only
                // after the write: it must not count as later detector evidence.
                const uint32_t queuedBaseline = ble.noteV1NotificationIngress();
                ble.nextVolumeSendResult = restoreResult;
                if (speedOwner) runtime.speed.update(0, false, 12600);
                clearAlert(12600);
                runtime.orchestration.processParsedFrame({12600, true, false});
                TEST_ASSERT_EQUAL_INT(2, ble.setVolumeCalls);
                TEST_ASSERT_EQUAL_UINT8(6, ble.lastVolume);
                feed(PACKET_ID_RESP_ALL_VOLUME, {6, 2, 6, 2}, 12610, queuedBaseline);
                runtime.orchestration.processParsedFrame({12610, true, false});
                TEST_ASSERT_FALSE(quiet.canApplyAutoPushVolumeExactly());
                TEST_ASSERT_EQUAL_INT(2, ble.setVolumeCalls);
                // Now the delayed drop becomes visible. Both owners must retry.
                volume(speedOwner ? 0 : 1, 2, 12700);
                runtime.orchestration.processParsedFrame({12700, true, false});
                TEST_ASSERT_EQUAL_INT(3, ble.setVolumeCalls);
                TEST_ASSERT_EQUAL_UINT8(6, ble.lastVolume);
                TEST_ASSERT_EQUAL_UINT8(2, ble.lastMuteVolume);
                volume(6, 2, 12710);
                runtime.orchestration.processParsedFrame({12710, true, false});
                TEST_ASSERT_TRUE(quiet.canApplyAutoPushVolumeExactly());
                runtime.orchestration.processParsedFrame({14000, true, false});
                TEST_ASSERT_EQUAL_INT(3, ble.setVolumeCalls);
            }
        }
    }
}

void test_restore_accepts_volume_feedback_received_inside_the_send() {
    static uint32_t duringSendIngress = 0;
    for (bool speedOwner : {false, true}) {
        setUp();
        SpeedFadeRuntime runtime;
        volume(6, 2, 10000);
        if (speedOwner) runtime.step(11000, 0, false);
        else {
            runtime.step(10000, 50, true);
            runtime.step(12500, 50, true);
        }
        if (speedOwner) runtime.speed.update(0, false, 12600);
        clearAlert(12600);
        ble.setVolumeSendHook = [] { duringSendIngress = ble.noteV1NotificationIngress(); };
        runtime.orchestration.processParsedFrame({12600, true, false});
        ble.setVolumeSendHook = nullptr;
        TEST_ASSERT_EQUAL_UINT8(6, ble.lastVolume);
        TEST_ASSERT_EQUAL_INT(2, ble.setVolumeCalls);
        // As on hardware, the callback arrived during the synchronous write,
        // and the main loop parses its canonical volume packet after return.
        feed(PACKET_ID_RESP_ALL_VOLUME, {6, 2, 6, 2}, 12610, duringSendIngress);
        runtime.orchestration.processParsedFrame({12610, true, false});
        TEST_ASSERT_TRUE(quiet.canApplyAutoPushVolumeExactly());
        runtime.orchestration.processParsedFrame({14000, true, false});
        TEST_ASSERT_EQUAL_INT(2, ble.setVolumeCalls);
    }
}

void test_restore_acknowledgement_survives_retry_and_ingress_rollover() {
    for (bool speedOwner : {false, true}) {
        setUp();
        SpeedFadeRuntime runtime;
        volume(6, 2, 10000);
        if (speedOwner) runtime.step(11000, 0, false);
        else {
            runtime.step(10000, 50, true);
            runtime.step(12500, 50, true);
        }
        ble.v1NotificationIngressSequenceValue = UINT32_MAX - 1;
        if (speedOwner) runtime.speed.update(0, false, 12600);
        clearAlert(12600);
        runtime.orchestration.processParsedFrame({12600, true, false});
        const uint32_t acknowledgement = ble.noteV1NotificationIngress();
        TEST_ASSERT_EQUAL_UINT32(1, acknowledgement);
        clearAlert(12700);
        runtime.orchestration.processParsedFrame({12700, true, false});
        TEST_ASSERT_EQUAL_INT(3, ble.setVolumeCalls);
        // Receipt after the first send still acknowledges it if a retry was
        // transmitted before this already-queued packet reached the parser.
        feed(PACKET_ID_RESP_ALL_VOLUME, {6, 2, 6, 2}, 12710, acknowledgement);
        runtime.orchestration.processParsedFrame({12710, true, false});
        TEST_ASSERT_TRUE(quiet.canApplyAutoPushVolumeExactly());
        TEST_ASSERT_EQUAL_INT(3, ble.setVolumeCalls);
    }
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_delayed_reduction_feedback_cannot_confirm_an_unsent_restore);
    RUN_TEST(test_restore_acknowledgement_survives_retry_and_ingress_rollover);
    RUN_TEST(test_restore_accepts_volume_feedback_received_inside_the_send);
    RUN_TEST(test_speed_restore_survives_long_holdoff_and_requires_complete_pair);
    RUN_TEST(test_speed_reentry_preserves_unconfirmed_restore_pair);
    RUN_TEST(test_live_new_threat_restore_retries_until_complete_pair_after_long_holdoff);
    RUN_TEST(test_invalid_speed_restore_survives_same_detector_reconnect_without_default_confirmation);
    RUN_TEST(test_new_profile_supersedes_long_pending_speed_restore_including_zero);
    RUN_TEST(test_delayed_live_restore_sounds_before_refade_and_then_honors_new_profile);
    RUN_TEST(test_pending_new_threat_restore_yields_to_proxy_and_changed_detector);
    RUN_TEST(test_coalesced_clear_and_same_frequency_rearms_fade);
    RUN_TEST(test_coalesced_clear_during_speed_restores_baseline_and_rearms_fade);
    RUN_TEST(test_coalesced_clear_before_fade_restarts_the_delay);
    RUN_TEST(test_coalesced_clear_preserves_known_zero_baseline);
    RUN_TEST(test_coalesced_clear_cancels_deferred_fade_and_confirms_restore_pair);
    RUN_TEST(test_repeated_coalesced_clear_retains_unconfirmed_restore_pair);
    RUN_TEST(test_coalesced_clear_during_delayed_speed_restore_replaces_faded_target);
    RUN_TEST(test_coalesced_speed_clear_preserves_new_autopush_baseline);
    RUN_TEST(test_coalesced_speed_clear_preserves_new_autopush_zero_baseline);
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
    RUN_TEST(test_fade_restore_ownership_releases_only_after_detector_convergence);
    RUN_TEST(test_failed_fade_down_retains_ownership_until_override_lifecycle_clears);
    RUN_TEST(test_not_yet_fade_down_retains_ownership_until_override_lifecycle_clears);
    RUN_TEST(test_different_detector_does_not_receive_old_fade_restore);
    RUN_TEST(test_unknown_detector_discards_deferred_fade_and_prior_identity);
    RUN_TEST(test_different_detector_discards_speed_restore_baseline);
    RUN_TEST(test_proxy_rejected_speed_drop_cannot_restore_over_phone_choice);
    RUN_TEST(test_proxy_takeover_cancels_pending_restore_without_a_parsed_frame);
    RUN_TEST(test_proxy_takeover_discards_active_fade_without_replaying_after_disconnect);
    RUN_TEST(test_speed_drop_not_yet_retries_and_restores_original_pair);
    return UNITY_END();
}

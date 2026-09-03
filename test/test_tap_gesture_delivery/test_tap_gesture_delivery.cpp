#include <unity.h>

#include "../mocks/Arduino.h"
#include "../../src/packet_parser.h"
#include "../../src/touch_handler.cpp"
#include "../mocks/ble_client.h"
#include "../mocks/display.h"
#include "../mocks/settings.h"
#include "../../include/display_mode.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../src/packet_parser.cpp"
#include "../../src/packet_parser_alerts.cpp"

class AutoPushModule {
  public:
    enum class QueueResult : uint8_t { QUEUED };
    bool isActive() const { return false; }
    QueueResult queueSlotPush(int) { return QueueResult::QUEUED; }
};

class AlertPersistenceModule {
  public:
    void clearPersistence() {}
};

#include "../../src/modules/quiet/quiet_coordinator_module.cpp"
#include "../../src/modules/touch/tap_gesture_module.cpp"
#include "../../src/modules/touch/touch_ui_module.cpp"

void audio_set_volume(uint8_t) {}
void play_test_voice() {}

namespace {
TouchHandler touch;
SettingsManager settings;
V1Display display;
V1BLEClient ble;
PacketParser parser;
AutoPushModule autoPush;
AlertPersistenceModule persistence;
DisplayMode displayMode = DisplayMode::IDLE;
QuietCoordinatorModule quiet;
TapGestureModule tap;
TouchUiModule touchUi;
int maintenanceBootRequests = 0;
bool profileCycleAllowed = true;

void parseV1Packet(uint8_t packetId, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> bytes{0xAA, 0xDA, 0xE4, packetId, static_cast<uint8_t>(payload.size() + 1)};
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    uint8_t checksum = 0;
    for (uint8_t byte : bytes) {
        checksum += byte;
    }
    bytes.push_back(checksum);
    bytes.push_back(0xAB);
    TEST_ASSERT_TRUE(parser.parse(bytes.data(), bytes.size(), mockMillis));
}

void setV1Alert(bool active, uint16_t frequency = 34700) {
    if (!active) {
        parseV1Packet(0x43, {0});
        return;
    }
    parseV1Packet(0x43, {0x11, static_cast<uint8_t>(frequency >> 8), static_cast<uint8_t>(frequency),
                         0xB0, 0, 0x22, 0x80, 0});
}

void setV1Display(bool laser, bool muted = false) {
    const uint8_t image = static_cast<uint8_t>(0x20 | (laser ? 0x01 : 0) | (muted ? 0x10 : 0));
    parseV1Packet(0x31, {0x06, 0x06, 0x03, image, image, 0x04, 0, 0x73});
}

bool wifiInactive(void*) { return false; }
void requestMaintenanceBoot(void*) { ++maintenanceBootRequests; }

void processInput(unsigned long nowMs, bool bootPressed = false) {
    mockMillis = nowMs;
    // Match normal runtime ownership: BOOT/settings first, then screen taps.
    if (!touchUi.process(nowMs, bootPressed)) {
        tap.process(nowMs, profileCycleAllowed);
    }
}

void pollTouch(unsigned long nowMs, bool active, uint16_t coordinate = 10) {
    std::vector<uint8_t> data(32, 0);
    data[1] = active ? 1 : 0;
    data[2] = data[4] = coordinate >> 8;
    data[3] = data[5] = coordinate;
    Wire.queueRequestFrom(data.size(), data);
    processInput(nowMs);
}
}

void setUp() {
    mockMillis = 0;
    Wire.resetMock();
    touch = TouchHandler{};
    TEST_ASSERT_TRUE(touch.begin());
    settings = SettingsManager{};
    display = V1Display{};
    ble.reset();
    parser = PacketParser{};
    setV1Alert(true);
    quiet = QuietCoordinatorModule{};
    quiet.begin(&ble, &parser);
    tap = TapGestureModule{};
    touchUi = TouchUiModule{};
    maintenanceBootRequests = 0;
    profileCycleAllowed = true;
    TouchUiModule::Callbacks callbacks{};
    callbacks.isWifiSetupActive = wifiInactive;
    callbacks.requestMaintenanceBoot = requestMaintenanceBoot;
    touchUi.begin(&display, &touch, &settings, callbacks);
    tap.begin(&touch, &settings, &display, &ble, &parser, &autoPush, &persistence, &displayMode, &quiet);
}

void test_alert_clear_drops_stale_mute_retry() {
    ble.nextMuteSendResult = SendResult::NOT_YET;
    pollTouch(200, true);
    TEST_ASSERT_EQUAL_INT(1, ble.setMuteCalls);

    setV1Alert(false);
    pollTouch(225, false);
    TEST_ASSERT_EQUAL_INT(1, ble.setMuteCalls);

    pollTouch(450, true); // Allow the real reader's tap/release debounce.
    TEST_ASSERT_EQUAL_INT(1, ble.setMuteCalls);
}

void tearDown() {}

void test_active_alert_tap_retries_one_transient_mute_without_resend_after_success() {
    ble.nextMuteSendResult = SendResult::NOT_YET;
    pollTouch(200, true);
    TEST_ASSERT_EQUAL_INT(1, ble.setMuteCalls);
    TEST_ASSERT_TRUE(ble.lastMuteValue);

    processInput(210);
    TEST_ASSERT_EQUAL_INT(1, ble.setMuteCalls);
    pollTouch(225, false);
    TEST_ASSERT_EQUAL_INT(2, ble.setMuteCalls);
    TEST_ASSERT_TRUE(ble.lastMuteValue);

    pollTouch(250, false);
    TEST_ASSERT_EQUAL_INT(2, ble.setMuteCalls);
}

void test_failed_touch_reads_invalidate_level_without_turning_recovery_into_a_tap() {
    for (bool shortRead : {false, true}) {
        setUp();
        setV1Alert(false);
        pollTouch(1000, true);

        for (unsigned long nowMs = 1025; nowMs <= 5100; nowMs += 25) {
            if (shortRead) {
                Wire.queueRequestFrom(0, {});
            } else {
                Wire.queueEndTransmission(2);
            }
            processInput(nowMs);
            TEST_ASSERT_FALSE(touch.isTouchActive());
        }
        TEST_ASSERT_GREATER_THAN_INT(0, Wire.endCalls); // Includes recovery backoff.
        TEST_ASSERT_EQUAL_INT(0, maintenanceBootRequests);

        Wire.resetMock(); // Discard fault responses queued during backoff.
        setV1Alert(true);
        pollTouch(5500, true);
        TEST_ASSERT_EQUAL_INT(0, ble.setMuteCalls);

        pollTouch(9500, true);
        TEST_ASSERT_EQUAL_INT(0, ble.setMuteCalls);

        pollTouch(9525, false);
        pollTouch(9700, true);
        TEST_ASSERT_EQUAL_INT(1, ble.setMuteCalls);
        pollTouch(13700, true);
        TEST_ASSERT_EQUAL_INT(1, ble.setMuteCalls);
        TEST_ASSERT_EQUAL_INT(0, maintenanceBootRequests);
    }
}

void test_screen_holds_cannot_request_maintenance_but_boot_hold_can() {
    for (uint16_t coordinate : {10, 514}) {
        setUp();
        setV1Alert(false);
        for (unsigned long nowMs = 1000; nowMs <= 5500; nowMs += 25) {
            pollTouch(nowMs, true, coordinate);
            TEST_ASSERT_EQUAL(coordinate == 10, touch.isTouchActive());
            TEST_ASSERT_EQUAL_INT(0, maintenanceBootRequests);
        }
        TEST_ASSERT_EQUAL_UINT8(0, settings.get().activeSlot);

        // Exercise the same live callback through its sole authorized input.
        processInput(6000, true);
        processInput(10000, true);
        TEST_ASSERT_EQUAL_INT(0, maintenanceBootRequests);
        processInput(10025, false);
        TEST_ASSERT_EQUAL_INT(1, maintenanceBootRequests);
    }
}

void test_three_screen_taps_still_cycle_profile_once() {
    setV1Alert(false);
    pollTouch(1000, true);
    pollTouch(1050, false);
    pollTouch(1250, true);
    TEST_ASSERT_EQUAL_UINT8(0, settings.get().activeSlot);
    pollTouch(1300, false);
    pollTouch(1500, true);
    TEST_ASSERT_EQUAL_UINT8(1, settings.get().activeSlot);
    TEST_ASSERT_EQUAL_INT(1, settings.saveDeferredBackupCalls);
    TEST_ASSERT_EQUAL_INT(1, display.drawProfileIndicatorCalls);
    pollTouch(5500, true);
    TEST_ASSERT_EQUAL_UINT8(1, settings.get().activeSlot);
    TEST_ASSERT_EQUAL_INT(1, display.drawProfileIndicatorCalls);
    TEST_ASSERT_EQUAL_INT(0, maintenanceBootRequests);
}

void test_invalid_touch_coordinates_cannot_mute_a_live_alert() {
    pollTouch(1000, true, 514);
    TEST_ASSERT_FALSE(touch.isTouchActive());
    TEST_ASSERT_EQUAL_INT(0, ble.setMuteCalls);
    pollTouch(1250, true, 10);
    TEST_ASSERT_TRUE(touch.isTouchActive());
    TEST_ASSERT_EQUAL_INT(1, ble.setMuteCalls);
    TEST_ASSERT_TRUE(ble.lastMuteValue);
}


void test_pending_mute_cannot_cross_complete_alert_replacement_between_polls() {
    for (uint16_t replacement : {34700, 35500}) {
        setUp();
        ble.nextMuteSendResult = SendResult::NOT_YET;
        pollTouch(1000, true);
        setV1Alert(false);
        setV1Alert(true, replacement);
        pollTouch(1025, false);
        TEST_ASSERT_EQUAL_INT(1, ble.setMuteCalls);
    }
}

void test_pending_mute_cannot_cross_session_reset_before_next_poll() {
    ble.nextMuteSendResult = SendResult::NOT_YET;
    pollTouch(1000, true);
    parser.resetAlertState();
    setV1Alert(true);
    pollTouch(1025, false);
    TEST_ASSERT_EQUAL_INT(1, ble.setMuteCalls);
}

void test_pending_mute_survives_ongoing_priority_change() {
    ble.nextMuteSendResult = SendResult::NOT_YET;
    pollTouch(1000, true);
    setV1Alert(true, 35500); // No aggregate clear: this remains one episode.
    pollTouch(1025, false);
    TEST_ASSERT_EQUAL_INT(2, ble.setMuteCalls);
    TEST_ASSERT_TRUE(ble.lastMuteValue);
}

void test_empty_radar_table_does_not_cancel_live_laser_mute() {
    setV1Alert(false);
    setV1Display(true);
    TEST_ASSERT_TRUE(parser.hasAlerts());
    ble.nextMuteSendResult = SendResult::NOT_YET;
    pollTouch(1000, true);
    setV1Alert(false);
    TEST_ASSERT_TRUE(parser.hasAlerts());
    pollTouch(1025, false);
    TEST_ASSERT_EQUAL_INT(2, ble.setMuteCalls);
    TEST_ASSERT_TRUE(ble.lastMuteValue);
}

void test_confirmed_mute_state_stops_pending_retry() {
    ble.nextMuteSendResult = SendResult::NOT_YET;
    pollTouch(1000, true);
    setV1Display(false, true);
    setV1Display(false, true);
    TEST_ASSERT_TRUE(parser.getDisplayState().muted);
    pollTouch(1025, false);
    TEST_ASSERT_EQUAL_INT(1, ble.setMuteCalls);
}

void test_partial_idle_taps_cannot_cross_unobserved_alert_or_session_boundary() {
    for (bool sessionReset : {false, true}) {
        setUp();
        setV1Alert(false);
        pollTouch(1000, true);
        pollTouch(1050, false);
        pollTouch(1250, true);
        pollTouch(1300, false);
        if (sessionReset) {
            parser.resetAlertState();
        } else {
            setV1Alert(true);
            setV1Alert(false); // Both publications happen before the next touch poll.
        }
        processInput(1310); // Also exercise cancellation while touch polling is gated.
        pollTouch(1500, true);
        TEST_ASSERT_EQUAL_UINT8(0, settings.get().activeSlot);
    }
}

void test_presentation_suspension_cancels_partial_taps_and_pending_mute() {
    ble.nextMuteSendResult = SendResult::NOT_YET;
    pollTouch(1000, true);
    tap.suspendForPresentationOwner();
    pollTouch(1025, false);
    TEST_ASSERT_EQUAL_INT(1, ble.setMuteCalls);
    setV1Alert(false);
    pollTouch(1250, true);
    pollTouch(1300, false);
    pollTouch(1500, true);
    pollTouch(1550, false);
    tap.suspendForPresentationOwner();
    pollTouch(1750, true);
    TEST_ASSERT_EQUAL_UINT8(0, settings.get().activeSlot);
}


void test_alp_only_context_consumes_contacts_without_cycling_profiles() {
    setV1Alert(false);
    pollTouch(1000, true);
    pollTouch(1050, false);
    pollTouch(1250, true);
    pollTouch(1300, false);
    profileCycleAllowed = false;
    processInput(1310); // Context cancellation is independent of reader polling.
    pollTouch(1500, true);
    pollTouch(1550, false);
    TEST_ASSERT_EQUAL_UINT8(0, settings.get().activeSlot);
    TEST_ASSERT_EQUAL_INT(0, ble.setMuteCalls);
    profileCycleAllowed = true;
    pollTouch(1750, true);
    pollTouch(2000, true); // The held blocked/fresh contact is never another tap.
    TEST_ASSERT_EQUAL_UINT8(0, settings.get().activeSlot);
    pollTouch(2025, false);
    pollTouch(2250, true);
    pollTouch(2300, false);
    pollTouch(2500, true);
    TEST_ASSERT_EQUAL_UINT8(1, settings.get().activeSlot);
}

void test_v1_mute_retry_remains_available_with_alp_overlay() {
    profileCycleAllowed = false;
    ble.nextMuteSendResult = SendResult::NOT_YET;
    pollTouch(1000, true);
    pollTouch(1025, false);
    TEST_ASSERT_EQUAL_INT(2, ble.setMuteCalls);
    TEST_ASSERT_TRUE(ble.lastMuteValue);
    TEST_ASSERT_EQUAL_UINT8(0, settings.get().activeSlot);
}

void test_lifetime_tracks_aggregate_start_end_and_explicit_session_reset() {
    setV1Alert(false);
    const uint32_t idle = parser.alertLifetime();
    setV1Alert(false);
    TEST_ASSERT_EQUAL_UINT32(idle, parser.alertLifetime());
    setV1Display(true);
    const uint32_t laser = parser.alertLifetime();
    TEST_ASSERT_TRUE(laser != idle);
    setV1Alert(true);
    setV1Alert(false);
    TEST_ASSERT_EQUAL_UINT32(laser, parser.alertLifetime());
    setV1Display(false);
    const uint32_t ended = parser.alertLifetime();
    TEST_ASSERT_TRUE(ended != laser);
    parser.resetAlertState();
    TEST_ASSERT_TRUE(ended != parser.alertLifetime());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_alp_only_context_consumes_contacts_without_cycling_profiles);
    RUN_TEST(test_v1_mute_retry_remains_available_with_alp_overlay);
    RUN_TEST(test_lifetime_tracks_aggregate_start_end_and_explicit_session_reset);
    RUN_TEST(test_pending_mute_cannot_cross_complete_alert_replacement_between_polls);
    RUN_TEST(test_pending_mute_cannot_cross_session_reset_before_next_poll);
    RUN_TEST(test_pending_mute_survives_ongoing_priority_change);
    RUN_TEST(test_empty_radar_table_does_not_cancel_live_laser_mute);
    RUN_TEST(test_confirmed_mute_state_stops_pending_retry);
    RUN_TEST(test_partial_idle_taps_cannot_cross_unobserved_alert_or_session_boundary);
    RUN_TEST(test_presentation_suspension_cancels_partial_taps_and_pending_mute);
    RUN_TEST(test_invalid_touch_coordinates_cannot_mute_a_live_alert);
    RUN_TEST(test_active_alert_tap_retries_one_transient_mute_without_resend_after_success);
    RUN_TEST(test_alert_clear_drops_stale_mute_retry);
    RUN_TEST(test_failed_touch_reads_invalidate_level_without_turning_recovery_into_a_tap);
    RUN_TEST(test_screen_holds_cannot_request_maintenance_but_boot_hold_can);
    RUN_TEST(test_three_screen_taps_still_cycle_profile_once);
    return UNITY_END();
}

/**
 * Error injection tests — exercises failure paths that happy-path tests miss.
 *
 * Covers:
 *   - AutoPush: BLE volume command permanently fails → exhausts retries, aborts cleanly
 *   - AutoPush: BLE mode command permanently fails → exhausts retries, aborts cleanly
 *   - AutoPush: BLE disconnect mid-push → immediate abort with perf metric
 *   - AutoPush: BLE disconnect mid-push for PushNow → no autoPushDisconnectAbort
 *   - AutoPush: all BLE commands fail simultaneously → clean abort
 *   - QuietCoordinator: setVolume failure during speed-vol drop → state stays consistent
 *   - QuietCoordinator: setVolume failure during speed-vol restore → retry path
 *   - AutoPush: profile write fails all retries → advances to Display step anyway
 */

#include <unity.h>

#include "../mocks/Arduino.h"
#include "../mocks/settings.h"
#include "../mocks/v1_profiles.h"
#include "../mocks/ble_client.h"
#include "../mocks/display.h"
#include "../mocks/packet_parser.h"
#include "../mocks/modules/volume_fade/volume_fade_module.h"
#include "../mocks/modules/speed_mute/speed_mute_module.h"

#include "../../src/perf_metrics.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

PerfCounters perfCounters;
PerfExtendedMetrics perfExtended;
void perfRecordDisplayRenderUs(uint32_t /*us*/) {}
void perfRecordDisplayScenarioRenderUs(uint32_t /*us*/) {}
void perfRecordSpeedVolDrop() {}
void perfRecordSpeedVolRestore() {}
void perfRecordSpeedVolRetry() {}

#include "../../src/modules/quiet/quiet_coordinator_module.cpp"
#include "../../src/modules/quiet/quiet_coordinator_templates.h"
#include "../../src/modules/auto_push/auto_push_module.cpp"

SettingsManager settingsManager;
static V1ProfileManager profileManager;
static V1BLEClient bleClient;
static V1Display display;
static PacketParser parser;
static QuietCoordinatorModule quiet;
static AutoPushModule autoPush;
static VolumeFadeModule volumeFade;
static SpeedMuteModule speedMute;

static V1Profile makeProfile(const char* name, uint8_t byte0 = 0xFF) {
    V1Profile profile;
    profile.name = name;
    profile.settings.bytes[0] = byte0;
    return profile;
}

static void advanceTime(unsigned long deltaMs) {
    mockMillis += deltaMs;
    mockMicros = mockMillis * 1000;
}

static void runUntilIdle(size_t maxTicks = 64) {
    for (size_t tick = 0; tick < maxTicks; ++tick) {
        autoPush.process();
        if (!autoPush.isActive()) {
            return;
        }
        advanceTime(40);
    }
    TEST_FAIL_MESSAGE("AutoPushModule did not become idle within maxTicks");
}

static void setupFullSlotPush(int slot = 0) {
    bleClient.setConnected(true);
    settingsManager.setSlot(slot, "Road", V1_MODE_LOGIC);
    settingsManager.setSlotVolumes(slot, 6, 2);
    settingsManager.setSlotDarkMode(slot, false);
    settingsManager.setSlotMuteToZero(slot, true);

    profileManager.loadProfileResult = true;
    profileManager.loadableProfileName = "Road";
    profileManager.loadableProfile = makeProfile("Road", 0xFF);
}

void setUp() {
    mockMillis = 1000;
    mockMicros = 1000000;
    perfCounters.reset();
    perfExtended.reset();

    settingsManager = SettingsManager{};
    profileManager.reset();
    bleClient.reset();
    display.reset();
    parser.reset();
    volumeFade = VolumeFadeModule{};
    speedMute = SpeedMuteModule{};

    quiet.begin(&bleClient, &parser);
    autoPush = AutoPushModule{};
    autoPush.begin(&settingsManager, &profileManager, &bleClient, &display, &quiet);
}

void tearDown() {}

// ── AutoPush: volume command permanently fails (PushNow) ──────────

void test_push_now_volume_fails_all_retries_aborts_cleanly() {
    setupFullSlotPush();
    bleClient.setVolumeFailuresRemaining = 100;  // always fail

    AutoPushModule::PushNowRequest request;
    request.slotIndex = 0;
    request.activateSlot = true;

    const auto result = autoPush.queuePushNow(request);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AutoPushModule::QueueResult::QUEUED),
                          static_cast<int>(result));

    runUntilIdle();

    TEST_ASSERT_FALSE(autoPush.isActive());
    // Volume step should have retried up to 8 times then failed
    TEST_ASSERT_TRUE(perfCounters.pushNowRetries.load() > 0);
    TEST_ASSERT_EQUAL_UINT32(1, perfCounters.pushNowFailures.load());
}

// ── AutoPush: mode command permanently fails (PushNow) ────────────

void test_push_now_mode_fails_all_retries_aborts_cleanly() {
    setupFullSlotPush();
    bleClient.setModeFailuresRemaining = 100;  // always fail

    AutoPushModule::PushNowRequest request;
    request.slotIndex = 0;
    request.activateSlot = true;

    const auto result = autoPush.queuePushNow(request);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AutoPushModule::QueueResult::QUEUED),
                          static_cast<int>(result));

    runUntilIdle();

    TEST_ASSERT_FALSE(autoPush.isActive());
    TEST_ASSERT_TRUE(perfCounters.pushNowRetries.load() > 0);
    TEST_ASSERT_EQUAL_UINT32(1, perfCounters.pushNowFailures.load());
}

// ── AutoPush: BLE disconnect mid-push (slot push) ─────────────────

void test_slot_push_ble_disconnect_mid_push_aborts_with_metric() {
    setupFullSlotPush();

    autoPush.queueSlotPush(0);
    TEST_ASSERT_TRUE(autoPush.isActive());

    // Process a couple of steps
    advanceTime(40);
    autoPush.process();  // WaitReady -> Profile
    advanceTime(40);

    // Disconnect BLE while Profile step is pending
    bleClient.setConnected(false);

    autoPush.process();

    TEST_ASSERT_FALSE(autoPush.isActive());
    TEST_ASSERT_EQUAL_UINT32(1, perfCounters.autoPushDisconnectAbort.load());
}

// ── AutoPush: BLE disconnect mid-push (PushNow) ──────────────────

void test_push_now_ble_disconnect_does_not_increment_disconnect_abort() {
    setupFullSlotPush();

    AutoPushModule::PushNowRequest request;
    request.slotIndex = 0;
    request.activateSlot = true;

    autoPush.queuePushNow(request);
    TEST_ASSERT_TRUE(autoPush.isActive());

    advanceTime(40);
    autoPush.process();
    advanceTime(40);

    bleClient.setConnected(false);
    autoPush.process();

    TEST_ASSERT_FALSE(autoPush.isActive());
    // PushNow disconnects should NOT increment autoPushDisconnectAbort
    TEST_ASSERT_EQUAL_UINT32(0, perfCounters.autoPushDisconnectAbort.load());
}

// ── AutoPush: all BLE commands fail simultaneously ────────────────

void test_push_now_all_commands_fail_aborts_after_max_retries() {
    setupFullSlotPush();
    bleClient.writeUserBytesFailuresRemaining = 100;
    bleClient.setDisplayOnFailuresRemaining = 100;
    bleClient.setModeFailuresRemaining = 100;
    bleClient.setVolumeFailuresRemaining = 100;

    AutoPushModule::PushNowRequest request;
    request.slotIndex = 0;
    request.activateSlot = true;

    autoPush.queuePushNow(request);
    runUntilIdle();

    TEST_ASSERT_FALSE(autoPush.isActive());
    TEST_ASSERT_EQUAL_UINT32(1, perfCounters.pushNowFailures.load());
}

// ── AutoPush: profile write fails all retries (slot push) ─────────

void test_slot_push_profile_write_fails_all_retries_still_completes() {
    setupFullSlotPush();
    bleClient.writeUserBytesResult = false;  // permanent fail

    autoPush.queueSlotPush(0);
    runUntilIdle();

    // Slot push should still complete even if profile write failed —
    // it advances to Display/Mode/Volume steps after exhausting retries
    TEST_ASSERT_FALSE(autoPush.isActive());
    TEST_ASSERT_EQUAL_UINT32(1, perfCounters.autoPushProfileWriteFail.load());
    // Display, mode, and volume commands should still have been attempted
    TEST_ASSERT_TRUE(bleClient.setDisplayOnCalls > 0);
    TEST_ASSERT_TRUE(bleClient.setModeCalls > 0);
    TEST_ASSERT_TRUE(bleClient.setVolumeCalls > 0);
}

// ── QuietCoordinator: setVolume fails during speed-vol drop ───────

void test_speed_vol_drop_ble_failure_still_sets_state() {
    parser.setMainVolume(6);
    parser.setMuteVolume(2);
    speedMute.begin(true, 25, 3, 0);
    speedMute.state_.muteActive = true;

    // Make setVolume fail
    bleClient.setVolumeFailuresRemaining = 1;

    const bool result = quiet.processSpeedVolume(1000, speedMute, &volumeFade);

    // processSpeedVolume calls sendVolume which calls setVolume —
    // even if BLE write fails, internal state should track the intent
    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_EQUAL(1, bleClient.setVolumeCalls);

    // The coordinator should NOT mark the volume owner if send failed
    // because sendVolume returns false and presentation owner is only set on success
    const auto& desired = quiet.getDesiredState();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(QuietOwner::SpeedVolume),
                          static_cast<int>(desired.volumeOwner));
    TEST_ASSERT_TRUE(desired.volumePending);
}

// ── QuietCoordinator: setVolume fails during speed-vol restore ────

void test_speed_vol_restore_ble_failure_triggers_retry_path() {
    // First: successfully drop volume
    parser.setMainVolume(6);
    parser.setMuteVolume(2);
    speedMute.begin(true, 25, 3, 0);
    speedMute.state_.muteActive = true;

    quiet.processSpeedVolume(1000, speedMute, &volumeFade);
    TEST_ASSERT_EQUAL(1, bleClient.setVolumeCalls);

    // Now speed rises — unmute
    speedMute.state_.muteActive = false;
    parser.setMainVolume(0);  // V1 is now at muted level

    // Make restore's setVolume fail
    bleClient.setVolumeFailuresRemaining = 1;

    const bool result = quiet.processSpeedVolume(1200, speedMute, &volumeFade);

    // Should still attempt — the retry path handles the failure
    TEST_ASSERT_EQUAL(2, bleClient.setVolumeCalls);

    // Retry should be armed because V1 hasn't confirmed restore
    // (parser still shows volume 0, not 6)
    const bool retried = quiet.retryPendingSpeedVolRestore(1400);
    TEST_ASSERT_EQUAL(3, bleClient.setVolumeCalls);
    TEST_ASSERT_EQUAL_UINT8(6, bleClient.lastVolume);
}

// ── AutoPush: display command failure non-PushNow proceeds anyway ─

void test_slot_push_display_command_failure_proceeds_to_mode() {
    setupFullSlotPush();
    bleClient.setDisplayOnFailuresRemaining = 1;  // fail once

    autoPush.queueSlotPush(0);
    runUntilIdle();

    TEST_ASSERT_FALSE(autoPush.isActive());
    // Slot push (non-PushNow) doesn't retry — it just proceeds
    TEST_ASSERT_EQUAL_INT(1, bleClient.setDisplayOnCalls);
    // But it should still reach Mode and Volume steps
    TEST_ASSERT_TRUE(bleClient.setModeCalls > 0);
    TEST_ASSERT_TRUE(bleClient.setVolumeCalls > 0);
    TEST_ASSERT_EQUAL_UINT32(1, perfCounters.autoPushCompletes.load());
}

// ── main ──────────────────────────────────────────────────────────

void runAllTests() {
    RUN_TEST(test_push_now_volume_fails_all_retries_aborts_cleanly);
    RUN_TEST(test_push_now_mode_fails_all_retries_aborts_cleanly);
    RUN_TEST(test_slot_push_ble_disconnect_mid_push_aborts_with_metric);
    RUN_TEST(test_push_now_ble_disconnect_does_not_increment_disconnect_abort);
    RUN_TEST(test_push_now_all_commands_fail_aborts_after_max_retries);
    RUN_TEST(test_slot_push_profile_write_fails_all_retries_still_completes);
    RUN_TEST(test_speed_vol_drop_ble_failure_still_sets_state);
    RUN_TEST(test_speed_vol_restore_ble_failure_triggers_retry_path);
    RUN_TEST(test_slot_push_display_command_failure_proceeds_to_mode);
}

#ifdef ARDUINO
void setup() {
    delay(2000);
    UNITY_BEGIN();
    runAllTests();
    UNITY_END();
}

void loop() {}
#else
int main(int argc, char** argv) {
    UNITY_BEGIN();
    runAllTests();
    return UNITY_END();
}
#endif

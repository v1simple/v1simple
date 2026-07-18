#include <unity.h>

#include "../../src/modules/wifi/wifi_maintenance_recovery_module.cpp"

// Regression coverage for the maintenance-boot WiFi recovery policy.
//
// Escaped-bug class (2026-07): the maintenance AP could stop (failed start at
// entry, emergency low-SRAM stop) with no restart path, leaving a WiFi-dead
// session until the maintenance timeout rebooted the device. The recovery
// module is the loop's guarantee that a down service gets bounded, scheduled
// restart attempts, and that an active service resets the schedule.

static WifiMaintenanceRecoveryInput makeInput(bool serviceReachable, unsigned long nowMs) {
    WifiMaintenanceRecoveryInput input;
    input.maintenanceBootActive = true;
    input.wifiServiceReachable = serviceReachable;
    input.nowMs = nowMs;
    return input;
}

void test_service_active_never_attempts() {
    WifiMaintenanceRecoveryModule module;
    for (unsigned long t = 0; t <= 120000UL; t += 1000UL) {
        const WifiMaintenanceRecoveryResult result = module.evaluate(makeInput(true, t));
        TEST_ASSERT_FALSE(result.attemptRestart);
    }
    TEST_ASSERT_EQUAL_UINT32(0u, module.attemptCount());
}

void test_outside_maintenance_never_attempts() {
    WifiMaintenanceRecoveryModule module;
    WifiMaintenanceRecoveryInput input = makeInput(false, 10000UL);
    input.maintenanceBootActive = false;
    const WifiMaintenanceRecoveryResult result = module.evaluate(input);
    TEST_ASSERT_FALSE(result.attemptRestart);
}

void test_first_attempt_waits_for_first_retry_delay() {
    WifiMaintenanceRecoveryModule module;
    // First down observation anchors the schedule and never fires.
    TEST_ASSERT_FALSE(module.evaluate(makeInput(false, 1000UL)).attemptRestart);
    // Just before the delay elapses: still waiting.
    TEST_ASSERT_FALSE(module.evaluate(makeInput(false, 1000UL + WifiMaintenanceRecoveryModule::kFirstRetryDelayMs - 1))
                          .attemptRestart);
    // Exactly at the delay: fire attempt #1.
    const WifiMaintenanceRecoveryResult result =
        module.evaluate(makeInput(false, 1000UL + WifiMaintenanceRecoveryModule::kFirstRetryDelayMs));
    TEST_ASSERT_TRUE(result.attemptRestart);
    TEST_ASSERT_EQUAL_UINT32(1u, result.attemptNumber);
}

void test_repeat_attempts_follow_retry_interval() {
    WifiMaintenanceRecoveryModule module;
    TEST_ASSERT_FALSE(module.evaluate(makeInput(false, 0UL)).attemptRestart);
    const unsigned long firstAttemptMs = WifiMaintenanceRecoveryModule::kFirstRetryDelayMs;
    TEST_ASSERT_TRUE(module.evaluate(makeInput(false, firstAttemptMs)).attemptRestart);

    // Between attempts: quiet.
    TEST_ASSERT_FALSE(
        module.evaluate(makeInput(false, firstAttemptMs + WifiMaintenanceRecoveryModule::kRetryIntervalMs - 1))
            .attemptRestart);

    // At the interval: attempt #2, and the schedule re-anchors on it.
    const unsigned long secondAttemptMs = firstAttemptMs + WifiMaintenanceRecoveryModule::kRetryIntervalMs;
    const WifiMaintenanceRecoveryResult second = module.evaluate(makeInput(false, secondAttemptMs));
    TEST_ASSERT_TRUE(second.attemptRestart);
    TEST_ASSERT_EQUAL_UINT32(2u, second.attemptNumber);

    const WifiMaintenanceRecoveryResult third =
        module.evaluate(makeInput(false, secondAttemptMs + WifiMaintenanceRecoveryModule::kRetryIntervalMs));
    TEST_ASSERT_TRUE(third.attemptRestart);
    TEST_ASSERT_EQUAL_UINT32(3u, third.attemptNumber);
}

void test_service_recovery_resets_schedule() {
    WifiMaintenanceRecoveryModule module;
    TEST_ASSERT_FALSE(module.evaluate(makeInput(false, 0UL)).attemptRestart);
    TEST_ASSERT_TRUE(
        module.evaluate(makeInput(false, WifiMaintenanceRecoveryModule::kFirstRetryDelayMs)).attemptRestart);

    // Service comes back: full reset.
    TEST_ASSERT_FALSE(module.evaluate(makeInput(true, 60000UL)).attemptRestart);
    TEST_ASSERT_EQUAL_UINT32(0u, module.attemptCount());

    // Goes down again: the first-retry delay applies afresh from the new
    // anchor, not from stale state.
    TEST_ASSERT_FALSE(module.evaluate(makeInput(false, 70000UL)).attemptRestart);
    TEST_ASSERT_FALSE(module.evaluate(makeInput(false, 70000UL + WifiMaintenanceRecoveryModule::kFirstRetryDelayMs - 1))
                          .attemptRestart);
    const WifiMaintenanceRecoveryResult result =
        module.evaluate(makeInput(false, 70000UL + WifiMaintenanceRecoveryModule::kFirstRetryDelayMs));
    TEST_ASSERT_TRUE(result.attemptRestart);
    TEST_ASSERT_EQUAL_UINT32(1u, result.attemptNumber);
}

void test_now_zero_anchor_is_preserved_across_repeated_ticks() {
    WifiMaintenanceRecoveryModule module;
    TEST_ASSERT_FALSE(module.evaluate(makeInput(false, 0UL)).attemptRestart);
    TEST_ASSERT_FALSE(module.evaluate(makeInput(false, 0UL)).attemptRestart);
    TEST_ASSERT_FALSE(
        module.evaluate(makeInput(false, WifiMaintenanceRecoveryModule::kFirstRetryDelayMs - 1)).attemptRestart);
    TEST_ASSERT_TRUE(
        module.evaluate(makeInput(false, WifiMaintenanceRecoveryModule::kFirstRetryDelayMs)).attemptRestart);
}

void test_rollover_safe_delta() {
    WifiMaintenanceRecoveryModule module;
    // Anchor just before unsigned rollover; the delay elapses across it.
    const unsigned long nearMax = static_cast<unsigned long>(-1) - 1000UL;
    TEST_ASSERT_FALSE(module.evaluate(makeInput(false, nearMax)).attemptRestart);
    const unsigned long afterRollover = WifiMaintenanceRecoveryModule::kFirstRetryDelayMs - 1000UL;
    TEST_ASSERT_TRUE(module.evaluate(makeInput(false, afterRollover)).attemptRestart);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_service_active_never_attempts);
    RUN_TEST(test_outside_maintenance_never_attempts);
    RUN_TEST(test_first_attempt_waits_for_first_retry_delay);
    RUN_TEST(test_repeat_attempts_follow_retry_interval);
    RUN_TEST(test_service_recovery_resets_schedule);
    RUN_TEST(test_now_zero_anchor_is_preserved_across_repeated_ticks);
    RUN_TEST(test_rollover_safe_delta);
    return UNITY_END();
}

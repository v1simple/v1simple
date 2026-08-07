/**
 * Regression boundary: classify latch write/readback outcomes and choose the
 * matching rail-wait or fallback path.
 */
#include <unity.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "../../include/battery_latch_shutdown_policy.h"

namespace {

using battery_latch_shutdown_policy::Verification;

std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

size_t countOccurrences(const std::string& text, const std::string& needle) {
    size_t count = 0;
    size_t pos = 0;
    while ((pos = text.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

void assertDecision(const battery_latch_shutdown_policy::Input& input, Verification expectedVerification,
                    bool expectedWait, const char* expectedExternalOutcome, const char* expectedFallbackReason) {
    const auto decision = battery_latch_shutdown_policy::evaluate(input);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(expectedVerification), static_cast<int>(decision.verification));
    TEST_ASSERT_EQUAL(expectedWait, decision.waitForRailCollapse);
    TEST_ASSERT_EQUAL_STRING(expectedExternalOutcome,
                             battery_latch_shutdown_policy::externalOutcomeName(decision.verification));
    TEST_ASSERT_EQUAL_STRING(expectedFallbackReason,
                             battery_latch_shutdown_policy::batteryFallbackReason(decision.verification));
}

} // namespace

void setUp() {}
void tearDown() {}

void test_write_failure_skips_rail_wait_and_reports_write_failure() {
    assertDecision({false, false, false}, Verification::WRITE_FAILED, false, "WRITE_FAILED", "latch_write_failed");
}

void test_readback_failure_still_waits_for_a_possible_rail_collapse() {
    assertDecision({true, false, false}, Verification::READBACK_FAILED, true, "READBACK_FAILED",
                   "latch_readback_failed");
}

void test_verified_low_reports_success_but_falls_back_if_the_rail_stays_alive() {
    assertDecision({true, true, true}, Verification::LATCH_LOW, true, "LOW", "rail_alive_after_latch");
}

void test_verified_high_reports_stuck_latch_and_specific_fallback() {
    assertDecision({true, true, false}, Verification::LATCH_HIGH, true, "HIGH_STUCK", "latch_readback_high");
}

void test_failed_write_dominates_impossible_readback_values() {
    assertDecision({false, true, true}, Verification::WRITE_FAILED, false, "WRITE_FAILED", "latch_write_failed");
}

void test_unknown_verification_fails_toward_readback_failure() {
    const auto unknown = static_cast<Verification>(255);
    TEST_ASSERT_EQUAL_STRING("READBACK_FAILED", battery_latch_shutdown_policy::externalOutcomeName(unknown));
    TEST_ASSERT_EQUAL_STRING("latch_readback_failed", battery_latch_shutdown_policy::batteryFallbackReason(unknown));
}

void test_production_routes_both_latch_outcomes_through_policy() {
    const std::string source = readFile(std::filesystem::path(PROJECT_DIR) / "src" / "battery_manager.cpp");

    TEST_ASSERT_EQUAL_UINT32(
        2, static_cast<uint32_t>(countOccurrences(source, "battery_latch_shutdown_policy::evaluate(")));
    TEST_ASSERT_EQUAL_UINT32(
        1, static_cast<uint32_t>(countOccurrences(source, "battery_latch_shutdown_policy::externalOutcomeName(")));
    TEST_ASSERT_EQUAL_UINT32(
        1, static_cast<uint32_t>(countOccurrences(source, "battery_latch_shutdown_policy::batteryFallbackReason(")));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, source.find("if (latchDecision.waitForRailCollapse)"));
    TEST_ASSERT_EQUAL(std::string::npos, source.find("const char* fallbackReason = latchDropped ?"));
}

void test_battery_fallback_feeds_watchdog_before_button_release_wait() {
    const std::string source = readFile(std::filesystem::path(PROJECT_DIR) / "src" / "battery_manager.cpp");
    const size_t fallback = source.find("const char* fallbackReason");
    const size_t watchdogFeed = source.find("(void)esp_task_wdt_reset();", fallback);
    const size_t buttonWait = source.find("waitForPinHigh(PWR_BUTTON_GPIO", fallback);

    TEST_ASSERT_NOT_EQUAL(std::string::npos, fallback);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, watchdogFeed);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, buttonWait);
    TEST_ASSERT_TRUE(watchdogFeed < buttonWait);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_write_failure_skips_rail_wait_and_reports_write_failure);
    RUN_TEST(test_readback_failure_still_waits_for_a_possible_rail_collapse);
    RUN_TEST(test_verified_low_reports_success_but_falls_back_if_the_rail_stays_alive);
    RUN_TEST(test_verified_high_reports_stuck_latch_and_specific_fallback);
    RUN_TEST(test_failed_write_dominates_impossible_readback_values);
    RUN_TEST(test_unknown_verification_fails_toward_readback_failure);
    RUN_TEST(test_production_routes_both_latch_outcomes_through_policy);
    RUN_TEST(test_battery_fallback_feeds_watchdog_before_button_release_wait);
    return UNITY_END();
}

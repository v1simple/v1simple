// Regression suite for bug #18: the maintenance-boot timeout ignored UI
// activity, so a user working in the maintenance web UI was rebooted mid-task
// at exactly ten minutes.
//
// Two layers are covered here because the bug lived across both:
//   1. MainRuntimePolicy::evaluateMaintenanceSession — the pure deadline /
//      extension decision, exercised directly. src/main.cpp cannot be compiled
//      natively, so this is the only layer that can actually execute the logic.
//   2. The wiring in src/main.cpp and the /api/status emit in
//      src/main_runtime_wiring.cpp — asserted by source inspection, matching
//      the pattern already used by test/test_main_manual_wifi_intent. This is
//      what catches somebody reintroducing the elapsed-since-start comparison
//      or breaking the deadline-anchor/status-payload contract.

#include <unity.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "../../include/main_runtime_state.h"

namespace {

constexpr uint32_t kMinuteMs = 60UL * 1000UL;
constexpr uint32_t kIdleWindowMs = static_cast<uint32_t>(MainRuntimePolicy::MaintenanceBootTimeoutMs);
constexpr uint32_t kMaxSessionMs = static_cast<uint32_t>(MainRuntimePolicy::MaintenanceBootMaxSessionMs);

MainRuntimePolicy::MaintenanceSessionInput sessionAt(uint32_t startedMs, uint32_t nowMs, uint32_t lastActivityMs) {
    MainRuntimePolicy::MaintenanceSessionInput input;
    input.sessionActive = true;
    input.sessionStartedMs = startedMs;
    input.nowMs = nowMs;
    input.lastUiActivityMs = lastActivityMs;
    return input;
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return text;
}

std::string extractFunctionBody(const std::string& text, const std::string& signature) {
    const size_t sigPos = text.find(signature);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, sigPos);

    const size_t braceStart = text.find('{', sigPos);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, braceStart);

    int depth = 0;
    for (size_t i = braceStart; i < text.size(); ++i) {
        if (text[i] == '{') {
            depth++;
        } else if (text[i] == '}') {
            depth--;
            if (depth == 0) {
                return text.substr(braceStart, i - braceStart + 1);
            }
        }
    }

    TEST_FAIL_MESSAGE("Failed to locate function body end");
    return {};
}

std::string projectRoot() {
    return std::string(PROJECT_DIR);
}

} // namespace

void setUp() {}
void tearDown() {}

// ─── Policy constants ─────────────────────────────────────────────────

void test_idle_window_is_still_ten_minutes_and_extension_is_bounded() {
    // The idle window must stay at the historical value so an untouched
    // session behaves exactly as it did before the fix.
    TEST_ASSERT_EQUAL_UINT32(10UL * 60UL * 1000UL, kIdleWindowMs);
    // The cap must exist, must be larger than the window (otherwise activity
    // could never extend anything), and must stay modest: it is the only bound
    // on an abandoned-but-open browser tab, whose /api/status poll marks UI
    // activity forever.
    TEST_ASSERT_GREATER_THAN_UINT32(kIdleWindowMs, kMaxSessionMs);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(60UL * 60UL * 1000UL, kMaxSessionMs);
}

// ─── Idle sessions keep the original behaviour ────────────────────────

void test_idle_session_survives_until_the_original_deadline() {
    const auto decision =
        MainRuntimePolicy::evaluateMaintenanceSession(sessionAt(1000, 1000 + kIdleWindowMs - 1, 0));

    TEST_ASSERT_FALSE(decision.shouldReboot);
    TEST_ASSERT_FALSE(decision.extended);
    TEST_ASSERT_EQUAL_UINT32(1, decision.remainingMs);
}

void test_idle_session_expires_at_exactly_the_original_deadline() {
    const auto decision = MainRuntimePolicy::evaluateMaintenanceSession(sessionAt(1000, 1000 + kIdleWindowMs, 0));

    TEST_ASSERT_TRUE(decision.shouldReboot);
    TEST_ASSERT_TRUE(decision.idleWindowExpired);
    TEST_ASSERT_FALSE(decision.maxSessionReached);
    TEST_ASSERT_EQUAL_UINT32(0, decision.remainingMs);
}

// ─── Activity extends the deadline ────────────────────────────────────

void test_activity_extends_past_the_original_deadline() {
    // Bug #18 proper: user clicked at t+9m, so t+10m must NOT reboot.
    const uint32_t start = 5000;
    const uint32_t lastActivity = start + (9 * kMinuteMs);
    const auto decision =
        MainRuntimePolicy::evaluateMaintenanceSession(sessionAt(start, start + kIdleWindowMs, lastActivity));

    TEST_ASSERT_FALSE(decision.shouldReboot);
    TEST_ASSERT_TRUE(decision.extended);
    TEST_ASSERT_EQUAL_UINT32(9 * kMinuteMs, decision.remainingMs);
}

void test_extended_session_expires_one_idle_window_after_last_activity() {
    const uint32_t start = 5000;
    const uint32_t lastActivity = start + (9 * kMinuteMs);

    const auto alive = MainRuntimePolicy::evaluateMaintenanceSession(
        sessionAt(start, lastActivity + kIdleWindowMs - 1, lastActivity));
    TEST_ASSERT_FALSE(alive.shouldReboot);

    const auto expired =
        MainRuntimePolicy::evaluateMaintenanceSession(sessionAt(start, lastActivity + kIdleWindowMs, lastActivity));
    TEST_ASSERT_TRUE(expired.shouldReboot);
    TEST_ASSERT_TRUE(expired.idleWindowExpired);
    TEST_ASSERT_FALSE(expired.maxSessionReached);
}

// ─── The extension is bounded ─────────────────────────────────────────

void test_absolute_cap_terminates_a_continuously_active_session() {
    // Activity is perpetually fresh (worst case: an abandoned tab polling
    // /api/status every three seconds). The cap must still fire.
    const uint32_t start = 5000;
    const uint32_t now = start + kMaxSessionMs;
    const auto decision = MainRuntimePolicy::evaluateMaintenanceSession(sessionAt(start, now, now));

    TEST_ASSERT_TRUE(decision.shouldReboot);
    TEST_ASSERT_TRUE(decision.maxSessionReached);
    TEST_ASSERT_FALSE(decision.idleWindowExpired);
    TEST_ASSERT_EQUAL_UINT32(0, decision.remainingMs);
}

void test_perpetual_activity_can_never_outlive_the_cap() {
    // Property check: march four hours forward marking activity on every
    // sample and assert the session is dead by the cap and stays dead.
    const uint32_t start = 12345;
    bool sawReboot = false;
    for (uint32_t elapsed = 0; elapsed <= 4UL * 60UL * kMinuteMs; elapsed += 3000) {
        const uint32_t now = start + elapsed;
        const auto decision = MainRuntimePolicy::evaluateMaintenanceSession(sessionAt(start, now, now));
        if (elapsed >= kMaxSessionMs) {
            TEST_ASSERT_TRUE(decision.shouldReboot);
            sawReboot = true;
        } else {
            TEST_ASSERT_FALSE(decision.shouldReboot);
        }
    }
    TEST_ASSERT_TRUE(sawReboot);
}

void test_cap_clamps_reported_remaining_below_the_idle_window() {
    // Five seconds before the cap, with fresh activity, the honest answer is
    // 5s remaining — not a full idle window. The UI countdown reads this.
    const uint32_t start = 5000;
    const uint32_t now = start + kMaxSessionMs - 5000;
    const auto decision = MainRuntimePolicy::evaluateMaintenanceSession(sessionAt(start, now, now));

    TEST_ASSERT_FALSE(decision.shouldReboot);
    TEST_ASSERT_EQUAL_UINT32(5000, decision.remainingMs);
}

// ─── Deadline anchor / status-payload arithmetic ──────────────────────

void test_deadline_anchor_reproduces_remaining_exactly() {
    // The web UI computes maintenanceBootTimeoutMs - maintenanceBootUptimeMs,
    // where uptime is emitted as (millis() - maintenanceBootStartedMs) and
    // maintenanceBootStartedMs is this anchor. That must equal remainingMs in
    // every regime: idle, extended, and cap-clamped.
    const uint32_t start = 7777;
    const uint32_t cases[][2] = {
        {start + 1000, 0},                                  // idle, early
        {start + kIdleWindowMs - 1, 0},                     // idle, about to expire
        {start + kIdleWindowMs, start + (9 * kMinuteMs)},   // extended
        {start + kMaxSessionMs - 5000, start + kMaxSessionMs - 5000}, // cap clamping
    };

    for (const auto& testCase : cases) {
        const auto decision =
            MainRuntimePolicy::evaluateMaintenanceSession(sessionAt(start, testCase[0], testCase[1]));
        const uint32_t uiUptimeMs = static_cast<uint32_t>(testCase[0] - decision.deadlineAnchorMs);
        TEST_ASSERT_EQUAL_UINT32(decision.remainingMs, kIdleWindowMs - uiUptimeMs);
    }
}

void test_idle_session_anchor_is_the_session_start() {
    // With no activity the anchor must not drift: maintenanceBootUptimeMs
    // still reports true session uptime for an untouched session.
    const uint32_t start = 4242;
    const auto decision = MainRuntimePolicy::evaluateMaintenanceSession(sessionAt(start, start + 90000, 0));
    TEST_ASSERT_EQUAL_UINT32(start, decision.deadlineAnchorMs);
}

void test_deadline_anchor_is_never_the_no_session_sentinel() {
    // A live session must never publish anchor 0: the status payload treats 0
    // as "not in maintenance boot" and would report a full idle window.
    const uint32_t start = 1;
    const uint32_t now = kIdleWindowMs; // consumed == now, so the raw anchor is 0
    const auto decision = MainRuntimePolicy::evaluateMaintenanceSession(sessionAt(start, now, 0));

    TEST_ASSERT_NOT_EQUAL_UINT32(0, decision.deadlineAnchorMs);
}

// ─── Rollover and malformed inputs ────────────────────────────────────

void test_deadline_that_overflows_before_now_wraps_does_not_expire_early() {
    // The canonical 49-day bug: the session starts just before the wrap, so
    // `start + idleWindow` overflows while `now` has NOT wrapped yet. Any
    // implementation written as `now >= then + interval` reboots instantly
    // here; elapsed-difference math reports one second of a ten-minute budget.
    const uint32_t start = 0xFFFFFFFFu - 5000u;
    const uint32_t now = static_cast<uint32_t>(start + 1000u);

    const auto idle = MainRuntimePolicy::evaluateMaintenanceSession(sessionAt(start, now, 0));
    TEST_ASSERT_FALSE(idle.shouldReboot);
    TEST_ASSERT_EQUAL_UINT32(1000, idle.elapsedSinceStartMs);
    TEST_ASSERT_EQUAL_UINT32(kIdleWindowMs - 1000, idle.remainingMs);

    // Same trap on the cap side: `start + maxSession` also overflows.
    const auto active = MainRuntimePolicy::evaluateMaintenanceSession(sessionAt(start, now, now));
    TEST_ASSERT_FALSE(active.shouldReboot);
    TEST_ASSERT_FALSE(active.maxSessionReached);
}

void test_survives_millis_rollover() {
    // Session starts ~30s before the 49-day wrap and is used across it.
    const uint32_t start = 0xFFFFFFFFu - 30000u;
    const uint32_t lastActivity = static_cast<uint32_t>(start + (9 * kMinuteMs)); // wrapped
    const uint32_t now = static_cast<uint32_t>(start + kIdleWindowMs);            // wrapped

    const auto decision = MainRuntimePolicy::evaluateMaintenanceSession(sessionAt(start, now, lastActivity));
    TEST_ASSERT_FALSE(decision.shouldReboot);
    TEST_ASSERT_EQUAL_UINT32(9 * kMinuteMs, decision.remainingMs);
    TEST_ASSERT_EQUAL_UINT32(kIdleWindowMs, decision.elapsedSinceStartMs);

    const auto capped = MainRuntimePolicy::evaluateMaintenanceSession(
        sessionAt(start, static_cast<uint32_t>(start + kMaxSessionMs), static_cast<uint32_t>(start + kMaxSessionMs)));
    TEST_ASSERT_TRUE(capped.shouldReboot);
    TEST_ASSERT_TRUE(capped.maxSessionReached);
}

void test_activity_timestamp_in_the_future_does_not_kill_the_session() {
    // A racing sample must not wrap to ~49 days of idleness and reboot
    // instantly. It is clamped to the session start instead.
    const uint32_t start = 5000;
    const uint32_t now = start + kMinuteMs;
    const auto decision = MainRuntimePolicy::evaluateMaintenanceSession(sessionAt(start, now, now + 10000));

    TEST_ASSERT_FALSE(decision.shouldReboot);
    TEST_ASSERT_EQUAL_UINT32(kMinuteMs, decision.elapsedSinceActivityMs);
}

void test_activity_older_than_the_session_is_clamped_to_session_start() {
    // Stale latch from a previous session: treat it as "no activity yet",
    // never as extra idleness.
    const uint32_t start = 600000;
    const uint32_t now = start + kMinuteMs;
    const auto decision = MainRuntimePolicy::evaluateMaintenanceSession(sessionAt(start, now, start - 500000));

    TEST_ASSERT_FALSE(decision.shouldReboot);
    TEST_ASSERT_FALSE(decision.extended);
    TEST_ASSERT_EQUAL_UINT32(kMinuteMs, decision.elapsedSinceActivityMs);
}

void test_inactive_session_is_inert() {
    MainRuntimePolicy::MaintenanceSessionInput input;
    input.nowMs = 999999;
    const auto decision = MainRuntimePolicy::evaluateMaintenanceSession(input);

    TEST_ASSERT_FALSE(decision.shouldReboot);
    TEST_ASSERT_EQUAL_UINT32(0, decision.remainingMs);
    TEST_ASSERT_EQUAL_UINT32(0, decision.deadlineAnchorMs);

    MainRuntimePolicy::MaintenanceSessionInput startedButFlagged = sessionAt(1000, 5000, 0);
    startedButFlagged.sessionActive = false;
    TEST_ASSERT_FALSE(MainRuntimePolicy::evaluateMaintenanceSession(startedButFlagged).shouldReboot);
}

// ─── Wiring contracts (src/main.cpp cannot be compiled natively) ──────

void test_main_loop_delegates_the_deadline_to_the_policy() {
    const std::string loopBody = extractFunctionBody(readFile(projectRoot() + "/src/main.cpp"), "void loop()");

    // The bug: a bare elapsed-since-start comparison against the timeout.
    TEST_ASSERT_EQUAL(std::string::npos,
                      loopBody.find("(now - mainRuntimeState.maintenanceBootStartedMs) >= "
                                    "MainRuntimePolicy::MaintenanceBootTimeoutMs"));
    // Stronger and wrap-proof: the loop must not do deadline arithmetic at
    // all. The timeout constant belongs to the policy and to boot logging.
    TEST_ASSERT_EQUAL(std::string::npos, loopBody.find("MainRuntimePolicy::MaintenanceBootTimeoutMs"));
    TEST_ASSERT_EQUAL(std::string::npos, loopBody.find("MainRuntimePolicy::MaintenanceBootMaxSessionMs"));

    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          loopBody.find("MainRuntimePolicy::evaluateMaintenanceSession(maintenanceSessionInput)"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, loopBody.find("maintenanceSession.shouldReboot"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, loopBody.find("markCleanShutdown();"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, loopBody.find("ESP.restart();"));
}

void test_main_loop_feeds_the_existing_ui_activity_signal() {
    const std::string loopBody = extractFunctionBody(readFile(projectRoot() + "/src/main.cpp"), "void loop()");

    // Must reuse WiFiManager's existing activity signal, not a parallel one.
    TEST_ASSERT_NOT_EQUAL(
        std::string::npos,
        loopBody.find("wifiManager.isUiActive(MainRuntimePolicy::MaintenanceUiActivityProbeMs)"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, loopBody.find("mainRuntimeState.maintenanceLastUiActivityMs ="));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, loopBody.find("maintenanceSessionInput.lastUiActivityMs ="));
}

void test_status_payload_deadline_anchor_contract_holds() {
    // Both halves of the contract that keeps the web UI countdown equal to the
    // device's real remaining time without changing the /api/status shape.
    const std::string loopBody = extractFunctionBody(readFile(projectRoot() + "/src/main.cpp"), "void loop()");
    TEST_ASSERT_NOT_EQUAL(
        std::string::npos,
        loopBody.find("mainRuntimeState.maintenanceBootStartedMs = maintenanceSession.deadlineAnchorMs;"));

    const std::string wiringBody = extractFunctionBody(
        readFile(projectRoot() + "/src/main_runtime_wiring.cpp"), "void configureWifiRuntimeModule()");
    TEST_ASSERT_NOT_EQUAL(std::string::npos, wiringBody.find("obj[\"maintenanceBootUptimeMs\"]"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          wiringBody.find("millis() - mainRuntimeState.maintenanceBootStartedMs"));
    TEST_ASSERT_NOT_EQUAL(
        std::string::npos,
        wiringBody.find("obj[\"maintenanceBootTimeoutMs\"] = MainRuntimePolicy::MaintenanceBootTimeoutMs;"));
}

void test_session_start_is_latched_once_and_never_moves() {
    const std::string source = readFile(projectRoot() + "/src/main.cpp");
    const std::string initBody = extractFunctionBody(source, "static void initializeMaintenanceBootFlow(");
    const std::string loopBody = extractFunctionBody(source, "void loop()");

    // Trailing '=' only, so clang-format is free to wrap the assignment.
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          initBody.find("mainRuntimeState.maintenanceBootSessionStartedMs ="));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, initBody.find("mainRuntimeState.maintenanceLastUiActivityMs = 0;"));
    // The cap is only a terminal escape if the loop never rewrites the start.
    TEST_ASSERT_EQUAL(std::string::npos, loopBody.find("mainRuntimeState.maintenanceBootSessionStartedMs ="));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_idle_window_is_still_ten_minutes_and_extension_is_bounded);
    RUN_TEST(test_idle_session_survives_until_the_original_deadline);
    RUN_TEST(test_idle_session_expires_at_exactly_the_original_deadline);
    RUN_TEST(test_activity_extends_past_the_original_deadline);
    RUN_TEST(test_extended_session_expires_one_idle_window_after_last_activity);
    RUN_TEST(test_absolute_cap_terminates_a_continuously_active_session);
    RUN_TEST(test_perpetual_activity_can_never_outlive_the_cap);
    RUN_TEST(test_cap_clamps_reported_remaining_below_the_idle_window);
    RUN_TEST(test_deadline_anchor_reproduces_remaining_exactly);
    RUN_TEST(test_idle_session_anchor_is_the_session_start);
    RUN_TEST(test_deadline_anchor_is_never_the_no_session_sentinel);
    RUN_TEST(test_deadline_that_overflows_before_now_wraps_does_not_expire_early);
    RUN_TEST(test_survives_millis_rollover);
    RUN_TEST(test_activity_timestamp_in_the_future_does_not_kill_the_session);
    RUN_TEST(test_activity_older_than_the_session_is_clamped_to_session_start);
    RUN_TEST(test_inactive_session_is_inert);
    RUN_TEST(test_main_loop_delegates_the_deadline_to_the_policy);
    RUN_TEST(test_main_loop_feeds_the_existing_ui_activity_signal);
    RUN_TEST(test_status_payload_deadline_anchor_contract_holds);
    RUN_TEST(test_session_start_is_latched_once_and_never_moves);
    return UNITY_END();
}

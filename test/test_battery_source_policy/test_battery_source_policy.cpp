/**
 * Regression tests for include/battery_source_policy.h — the pure decision
 * layer extracted from BatteryManager for bug #17 (battery source-classification
 * races).
 *
 * src/battery_manager.cpp cannot be compiled natively (esp_adc, driver/gpio,
 * SD_MMC, FreeRTOS), and test/mocks/battery_manager.h replaces the whole class
 * for every other native suite. Any decision left inside that .cpp is untested
 * by construction. These tests exist so the three defects below fail here first:
 *
 *   A - zero-spaced samples: five reads in one tight loop are not majority
 *       voting; a transient is read five times identically.
 *   B - PWR-wake boots misclassified as USB for ~1s because the button held
 *       GPIO16 LOW through the first classification.
 *   C - ADC-init failure permanently disabled the power button, because the
 *       button was gated on hasBattery(), which requires a live ADC.
 *
 * The last section is a source contract over src/battery_manager.cpp: it cannot
 * be executed here, so its wiring is asserted textually instead.
 */
#include <unity.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "../../include/battery_source_policy.h"

namespace {

using battery_source_policy::armEvidenceReplay;
using battery_source_policy::ButtonGateInputs;
using battery_source_policy::classifyRound;
using battery_source_policy::Config;
using battery_source_policy::EvidenceReplayConfig;
using battery_source_policy::EvidenceReplayState;
using battery_source_policy::Observation;
using battery_source_policy::observe;
using battery_source_policy::onBattery;
using battery_source_policy::Outcome;
using battery_source_policy::powerButtonHandlingEnabled;
using battery_source_policy::resolveOnBattery;
using battery_source_policy::Result;
using battery_source_policy::roundDue;
using battery_source_policy::Source;
using battery_source_policy::sourceName;
using battery_source_policy::takeEvidenceReplay;

constexpr Config kConfig{};

// GPIO16 HIGH on every sample: running from the cell, button released.
Observation batteryRound() {
    Observation o;
    o.highSamples = kConfig.samplesPerRound;
    o.totalSamples = kConfig.samplesPerRound;
    return o;
}

// GPIO16 LOW on every sample: external supply, OR the button is held.
Observation usbRound() {
    Observation o;
    o.highSamples = 0;
    o.totalSamples = kConfig.samplesPerRound;
    return o;
}

// A round the caller knows to be untrustworthy (PWR hold in flight).
Observation heldRound() {
    Observation o = usbRound();
    o.buttonInteraction = true;
    return o;
}

int sourceInt(Source s) {
    return static_cast<int>(s);
}

int outcomeInt(Outcome o) {
    return static_cast<int>(o);
}

// Drive a state to a settled BATTERY classification and return the time cursor
// just past the confirming round.
battery_source_policy::State settledBattery(uint32_t& nowMs) {
    battery_source_policy::State s;
    observe(s, nowMs, batteryRound(), kConfig);
    nowMs += kConfig.roundSpacingMs;
    observe(s, nowMs, batteryRound(), kConfig);
    TEST_ASSERT_EQUAL_INT(sourceInt(Source::Battery), sourceInt(s.classification));
    return s;
}

// Drive a state to a settled USB classification (requires the persistence
// holdoff to elapse) and return the time cursor just past the confirming round.
battery_source_policy::State settledUsb(uint32_t& nowMs) {
    battery_source_policy::State s;
    observe(s, nowMs, usbRound(), kConfig);
    nowMs += kConfig.roundSpacingMs;
    observe(s, nowMs, usbRound(), kConfig);
    nowMs += kConfig.usbConfirmMs;
    observe(s, nowMs, usbRound(), kConfig);
    TEST_ASSERT_EQUAL_INT(sourceInt(Source::Usb), sourceInt(s.classification));
    return s;
}

std::string readProjectFile(const char* relativePath) {
    const std::filesystem::path path = std::filesystem::path(PROJECT_DIR) / relativePath;
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        return {};
    }
    return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
}

bool contains(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

} // namespace

void setUp() {}
void tearDown() {}

// ─── Cold start: fail toward battery (defect B) ────────────────────────────

void test_cold_start_has_no_classification_and_resolves_to_battery() {
    battery_source_policy::State s;
    TEST_ASSERT_EQUAL_INT(sourceInt(Source::Unknown), sourceInt(s.classification));
    TEST_ASSERT_TRUE(onBattery(s));
    TEST_ASSERT_TRUE(resolveOnBattery(Source::Unknown));
    TEST_ASSERT_TRUE(resolveOnBattery(Source::Battery));
    TEST_ASSERT_FALSE(resolveOnBattery(Source::Usb));
}

void test_cold_start_first_round_is_always_due() {
    battery_source_policy::State s;
    TEST_ASSERT_TRUE(roundDue(s, 0, kConfig));
    TEST_ASSERT_TRUE(roundDue(s, 1, kConfig));
    TEST_ASSERT_TRUE(roundDue(s, 0xFFFFFFFFu, kConfig));
}

void test_cold_start_low_pin_never_reports_usb_inside_the_holdoff() {
    battery_source_policy::State s;
    uint32_t now = 0;
    for (uint32_t elapsed = 0; elapsed < kConfig.usbConfirmMs; elapsed += 100) {
        now = elapsed;
        const Result r = observe(s, now, usbRound(), kConfig);
        TEST_ASSERT_FALSE(r.changed);
        TEST_ASSERT_TRUE_MESSAGE(r.onBattery, "cold start must fail toward battery, never USB");
        TEST_ASSERT_NOT_EQUAL(sourceInt(Source::Usb), sourceInt(s.classification));
    }
}

void test_cold_start_usb_settles_only_after_the_persistence_window() {
    battery_source_policy::State s;
    observe(s, 0, usbRound(), kConfig);
    observe(s, kConfig.roundSpacingMs, usbRound(), kConfig);
    TEST_ASSERT_EQUAL_INT(sourceInt(Source::Unknown), sourceInt(s.classification));

    // One millisecond short of the window: still not USB.
    const Result early = observe(s, kConfig.usbConfirmMs - 1, usbRound(), kConfig);
    TEST_ASSERT_EQUAL_INT(outcomeInt(Outcome::UsbHoldoff), outcomeInt(early.outcome));
    TEST_ASSERT_TRUE(early.onBattery);

    const Result settled = observe(s, kConfig.usbConfirmMs, usbRound(), kConfig);
    TEST_ASSERT_EQUAL_INT(outcomeInt(Outcome::Changed), outcomeInt(settled.outcome));
    TEST_ASSERT_EQUAL_INT(sourceInt(Source::Usb), sourceInt(s.classification));
    TEST_ASSERT_FALSE(settled.onBattery);
    TEST_ASSERT_EQUAL_UINT32(kConfig.usbConfirmMs, settled.usbConfirmationElapsedMs);
}

void test_pwr_wake_boot_reports_battery_throughout_and_confirms_battery() {
    // Boot woken by the PWR button: GPIO16 is held LOW for the first ~800 ms,
    // then the user releases it. The device must never report USB.
    battery_source_policy::State s;
    uint32_t now = 0;

    observe(s, now, usbRound(), kConfig); // boot round 1, button still held
    TEST_ASSERT_TRUE(onBattery(s));
    now += kConfig.roundSpacingMs;
    observe(s, now, usbRound(), kConfig); // boot round 2, button still held
    TEST_ASSERT_TRUE(onBattery(s));

    for (now = 100; now < 800; now += 100) {
        observe(s, now, usbRound(), kConfig);
        TEST_ASSERT_TRUE_MESSAGE(onBattery(s), "PWR-wake boot must not report USB while the button is held");
        TEST_ASSERT_NOT_EQUAL(sourceInt(Source::Usb), sourceInt(s.classification));
    }

    // Button released — pin goes HIGH.
    now = 1000;
    observe(s, now, batteryRound(), kConfig);
    now += kConfig.roundSpacingMs;
    const Result confirm = observe(s, now, batteryRound(), kConfig);
    TEST_ASSERT_EQUAL_INT(sourceInt(Source::Battery), sourceInt(s.classification));
    TEST_ASSERT_TRUE(confirm.onBattery);
    TEST_ASSERT_TRUE(confirm.changed);
}

// ─── Two-round agreement (defect A) ────────────────────────────────────────

void test_two_agreeing_rounds_change_the_classification() {
    uint32_t now = 10000;
    battery_source_policy::State s = settledUsb(now);

    now += kConfig.cycleIntervalMs;
    const Result first = observe(s, now, batteryRound(), kConfig);
    TEST_ASSERT_EQUAL_INT(outcomeInt(Outcome::AwaitingConfirmation), outcomeInt(first.outcome));
    TEST_ASSERT_FALSE(first.changed);
    TEST_ASSERT_EQUAL_INT(sourceInt(Source::Usb), sourceInt(s.classification));

    now += kConfig.roundSpacingMs;
    const Result second = observe(s, now, batteryRound(), kConfig);
    TEST_ASSERT_EQUAL_INT(outcomeInt(Outcome::Changed), outcomeInt(second.outcome));
    TEST_ASSERT_TRUE(second.changed);
    TEST_ASSERT_EQUAL_INT(sourceInt(Source::Battery), sourceInt(s.classification));
}

void test_one_round_alone_never_changes_the_classification() {
    uint32_t now = 10000;
    battery_source_policy::State s = settledBattery(now);

    now += kConfig.cycleIntervalMs;
    const Result only = observe(s, now, usbRound(), kConfig);
    TEST_ASSERT_FALSE_MESSAGE(only.changed, "a single round must never flip the classification");
    TEST_ASSERT_EQUAL_INT(sourceInt(Source::Battery), sourceInt(s.classification));
    TEST_ASSERT_TRUE(only.onBattery);
}

void test_two_disagreeing_rounds_hold_the_previous_classification() {
    uint32_t now = 10000;
    battery_source_policy::State s = settledBattery(now);

    now += kConfig.cycleIntervalMs;
    observe(s, now, usbRound(), kConfig); // round 1 disagrees
    now += kConfig.roundSpacingMs;
    const Result second = observe(s, now, batteryRound(), kConfig); // round 2 contradicts it

    TEST_ASSERT_FALSE(second.changed);
    TEST_ASSERT_EQUAL_INT(sourceInt(Source::Battery), sourceInt(s.classification));
    TEST_ASSERT_TRUE(second.onBattery);
}

void test_two_disagreeing_rounds_from_unknown_hold_unknown() {
    battery_source_policy::State s;
    observe(s, 0, batteryRound(), kConfig);
    const Result second = observe(s, kConfig.roundSpacingMs, usbRound(), kConfig);

    TEST_ASSERT_EQUAL_INT(outcomeInt(Outcome::Disagreed), outcomeInt(second.outcome));
    TEST_ASSERT_FALSE(second.changed);
    TEST_ASSERT_EQUAL_INT(sourceInt(Source::Unknown), sourceInt(s.classification));
    TEST_ASSERT_TRUE(second.onBattery);
}

void test_alternating_transients_never_flip_the_classification() {
    // This is defect A's failure mode: a bouncing/transient pin. Zero-spaced
    // sampling reads one instant five times; spaced rounds never agree.
    uint32_t now = 10000;
    battery_source_policy::State s = settledBattery(now);

    for (int i = 0; i < 40; ++i) {
        now += kConfig.roundSpacingMs;
        const Result r = observe(s, now, (i % 2 == 0) ? usbRound() : batteryRound(), kConfig);
        TEST_ASSERT_FALSE_MESSAGE(r.changed, "alternating rounds must never reach agreement");
        TEST_ASSERT_EQUAL_INT(sourceInt(Source::Battery), sourceInt(s.classification));
    }
}

void test_a_real_usb_attach_still_settles_when_no_button_is_involved() {
    // The safety rules must not make USB unreachable: a genuine, persistent,
    // unsuppressed LOW does eventually classify as USB.
    uint32_t now = 10000;
    battery_source_policy::State s = settledBattery(now);

    // Fixed step, not kConfig.roundSpacingMs: the loop bound must not depend on
    // a value under test.
    constexpr uint32_t kStepMs = 50;
    bool sawUsb = false;
    for (uint32_t elapsed = 0; elapsed <= kConfig.usbConfirmMs + 2 * kConfig.cycleIntervalMs; elapsed += kStepMs) {
        observe(s, now + elapsed, usbRound(), kConfig);
        if (s.classification == Source::Usb) {
            sawUsb = true;
            break;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(sawUsb, "a persistent unsuppressed LOW must eventually classify as USB");
}

void test_usb_to_battery_recovery_needs_no_holdoff() {
    uint32_t now = 10000;
    battery_source_policy::State s = settledUsb(now);

    now += kConfig.cycleIntervalMs;
    observe(s, now, batteryRound(), kConfig);
    now += kConfig.roundSpacingMs;
    observe(s, now, batteryRound(), kConfig);
    TEST_ASSERT_EQUAL_INT(sourceInt(Source::Battery), sourceInt(s.classification));
}

// ─── Suppression: button held (defects A + B) ──────────────────────────────

void test_button_held_rounds_are_suppressed_and_change_nothing() {
    uint32_t now = 10000;
    battery_source_policy::State s = settledBattery(now);

    for (uint32_t elapsed = 0; elapsed < 10000; elapsed += 250) {
        const Result r = observe(s, now + elapsed, heldRound(), kConfig);
        TEST_ASSERT_EQUAL_INT(outcomeInt(Outcome::Suppressed), outcomeInt(r.outcome));
        TEST_ASSERT_FALSE(r.changed);
        TEST_ASSERT_TRUE(r.onBattery);
        TEST_ASSERT_EQUAL_INT(sourceInt(Source::Battery), sourceInt(s.classification));
    }
}

void test_long_button_hold_never_out_waits_the_usb_persistence_window() {
    // A power-off hold is 2000 ms and users hold longer. A suppressed hold must
    // never accumulate USB evidence, no matter how long it lasts.
    TEST_ASSERT_GREATER_THAN_UINT32(2000u, kConfig.usbConfirmMs);

    uint32_t now = 10000;
    battery_source_policy::State s = settledBattery(now);
    for (uint32_t elapsed = 0; elapsed < 30000; elapsed += 100) {
        observe(s, now + elapsed, heldRound(), kConfig);
    }
    TEST_ASSERT_EQUAL_INT(sourceInt(Source::Battery), sourceInt(s.classification));
    TEST_ASSERT_FALSE(s.hasUsbCandidate);
}

void test_suppressed_round_cannot_be_half_of_an_agreement() {
    uint32_t now = 10000;
    battery_source_policy::State s = settledBattery(now);

    now += kConfig.cycleIntervalMs;
    observe(s, now, usbRound(), kConfig); // round 1: pending USB
    TEST_ASSERT_TRUE(s.hasPendingVerdict);

    now += kConfig.roundSpacingMs;
    observe(s, now, heldRound(), kConfig); // button goes down mid-cycle
    TEST_ASSERT_FALSE_MESSAGE(s.hasPendingVerdict, "a suppressed round must drop the pending verdict");
    TEST_ASSERT_FALSE(s.hasUsbCandidate);

    now += kConfig.roundSpacingMs;
    const Result r = observe(s, now, usbRound(), kConfig);
    TEST_ASSERT_EQUAL_INT(outcomeInt(Outcome::AwaitingConfirmation), outcomeInt(r.outcome));
    TEST_ASSERT_EQUAL_INT(sourceInt(Source::Battery), sourceInt(s.classification));
}

void test_suppressed_cold_start_stays_unknown_and_resolves_to_battery() {
    battery_source_policy::State s;
    for (uint32_t now = 0; now < 20000; now += 250) {
        const Result r = observe(s, now, heldRound(), kConfig);
        TEST_ASSERT_TRUE_MESSAGE(r.onBattery, "an uncertain state must resolve to battery, not USB");
        TEST_ASSERT_EQUAL_INT(sourceInt(Source::Unknown), sourceInt(s.classification));
    }
}

void test_empty_round_is_treated_as_suppressed() {
    uint32_t now = 10000;
    battery_source_policy::State s = settledBattery(now);
    Observation empty;
    empty.highSamples = 0;
    empty.totalSamples = 0;

    const Result r = observe(s, now + kConfig.cycleIntervalMs, empty, kConfig);
    TEST_ASSERT_EQUAL_INT(outcomeInt(Outcome::Suppressed), outcomeInt(r.outcome));
    TEST_ASSERT_EQUAL_INT(sourceInt(Source::Battery), sourceInt(s.classification));
}

// ─── Round scheduling ──────────────────────────────────────────────────────

void test_settled_rounds_run_at_the_cycle_interval() {
    uint32_t now = 10000;
    battery_source_policy::State s = settledBattery(now);

    TEST_ASSERT_FALSE(roundDue(s, now, kConfig));
    TEST_ASSERT_FALSE(roundDue(s, now + kConfig.cycleIntervalMs - 1, kConfig));
    TEST_ASSERT_TRUE(roundDue(s, now + kConfig.cycleIntervalMs, kConfig));
}

void test_a_pending_change_schedules_a_spaced_confirmation_round() {
    uint32_t now = 10000;
    battery_source_policy::State s = settledBattery(now);

    now += kConfig.cycleIntervalMs;
    observe(s, now, usbRound(), kConfig);
    TEST_ASSERT_TRUE(s.fastFollowUp);
    TEST_ASSERT_FALSE_MESSAGE(roundDue(s, now, kConfig), "the confirmation round must be separated in time");
    TEST_ASSERT_FALSE(roundDue(s, now + kConfig.roundSpacingMs - 1, kConfig));
    TEST_ASSERT_TRUE(roundDue(s, now + kConfig.roundSpacingMs, kConfig));
}

void test_round_spacing_is_non_zero() {
    // Defect A in one assertion: two rounds taken at the same instant are the
    // same sample. The configured spacing must be a real gap.
    TEST_ASSERT_GREATER_THAN_UINT32(0u, kConfig.roundSpacingMs);
}

// ─── millis() rollover ─────────────────────────────────────────────────────

void test_round_due_is_rollover_safe() {
    battery_source_policy::State s;
    s.clockSeeded = true;
    s.lastRoundMs = 0xFFFFFF00u; // ~256 ms before wrap

    TEST_ASSERT_FALSE(roundDue(s, 0xFFFFFF00u + kConfig.cycleIntervalMs - 1, kConfig));
    TEST_ASSERT_TRUE(roundDue(s, 0xFFFFFF00u + kConfig.cycleIntervalMs, kConfig));

    // Timestamps still on the pre-wrap side of the clock must also be rejected.
    // The naive `now >= last + span` form wraps its own right-hand side, leaving
    // every large pre-wrap `now` looking overdue.
    TEST_ASSERT_FALSE_MESSAGE(roundDue(s, 0xFFFFFF01u, kConfig), "1 ms elapsed must not be due");
    TEST_ASSERT_FALSE_MESSAGE(roundDue(s, 0xFFFFFFFFu, kConfig), "255 ms elapsed must not be due");

    s.fastFollowUp = true;
    s.lastRoundMs = 0xFFFFFFF0u;
    TEST_ASSERT_FALSE(roundDue(s, 0xFFFFFFF0u + kConfig.roundSpacingMs - 1, kConfig));
    TEST_ASSERT_TRUE(roundDue(s, 0xFFFFFFF0u + kConfig.roundSpacingMs, kConfig));
    TEST_ASSERT_FALSE_MESSAGE(roundDue(s, 0xFFFFFFF1u, kConfig), "1 ms elapsed must not be due");
}

void test_rollover_across_the_sampling_window_causes_no_spurious_change() {
    uint32_t now = 0xFFFFF000u;
    battery_source_policy::State s = settledBattery(now);

    // Steady battery rounds straddling the wrap: nothing may change.
    for (int i = 0; i < 20; ++i) {
        now += kConfig.cycleIntervalMs;
        const Result r = observe(s, now, batteryRound(), kConfig);
        TEST_ASSERT_FALSE_MESSAGE(r.changed, "a millis() wrap must not fabricate a source change");
        TEST_ASSERT_EQUAL_INT(sourceInt(Source::Battery), sourceInt(s.classification));
    }

    // A disagreeing pair straddling the wrap must still hold the previous value.
    now = 0xFFFFFFF0u;
    s.classification = Source::Battery;
    s.hasPendingVerdict = false;
    s.fastFollowUp = false;
    s.hasUsbCandidate = false;
    s.lastRoundMs = now;
    observe(s, now, usbRound(), kConfig);
    const Result wrapped = observe(s, 0x00000015u, batteryRound(), kConfig);
    TEST_ASSERT_FALSE(wrapped.changed);
    TEST_ASSERT_EQUAL_INT(sourceInt(Source::Battery), sourceInt(s.classification));
}

void test_usb_persistence_window_is_measured_across_a_rollover() {
    uint32_t settleAt = 10000;
    battery_source_policy::State s = settledBattery(settleAt);

    // Start the USB candidate close enough to the wrap that candidate + window
    // straddles zero.
    const uint32_t candidateStart = 0xFFFFFE00u;
    observe(s, candidateStart, usbRound(), kConfig);
    TEST_ASSERT_TRUE(s.hasUsbCandidate);
    TEST_ASSERT_EQUAL_UINT32(candidateStart, s.usbCandidateSinceMs);
    observe(s, candidateStart + kConfig.roundSpacingMs, usbRound(), kConfig);

    // Only 256 ms into the window and still pre-wrap. The naive
    // `now < start + window` form has already wrapped its right-hand side and
    // would accept USB here.
    const Result preWrap = observe(s, 0xFFFFFF00u, usbRound(), kConfig);
    TEST_ASSERT_EQUAL_INT(outcomeInt(Outcome::UsbHoldoff), outcomeInt(preWrap.outcome));
    TEST_ASSERT_EQUAL_INT(sourceInt(Source::Battery), sourceInt(s.classification));
    TEST_ASSERT_TRUE(preWrap.onBattery);

    // One millisecond short of the full window, now on the post-wrap side.
    const Result early = observe(s, candidateStart + kConfig.usbConfirmMs - 1, usbRound(), kConfig);
    TEST_ASSERT_EQUAL_INT(outcomeInt(Outcome::UsbHoldoff), outcomeInt(early.outcome));
    TEST_ASSERT_EQUAL_INT(sourceInt(Source::Battery), sourceInt(s.classification));

    const Result late = observe(s, candidateStart + kConfig.usbConfirmMs, usbRound(), kConfig);
    TEST_ASSERT_EQUAL_INT(outcomeInt(Outcome::Changed), outcomeInt(late.outcome));
    TEST_ASSERT_EQUAL_INT(sourceInt(Source::Usb), sourceInt(s.classification));
}

// ─── Majority vote ─────────────────────────────────────────────────────────

void test_majority_vote_boundaries() {
    Observation o;
    o.totalSamples = 5;
    o.highSamples = 5;
    TEST_ASSERT_EQUAL_INT(sourceInt(Source::Battery), sourceInt(classifyRound(o)));
    o.highSamples = 3;
    TEST_ASSERT_EQUAL_INT(sourceInt(Source::Battery), sourceInt(classifyRound(o)));
    o.highSamples = 2;
    TEST_ASSERT_EQUAL_INT(sourceInt(Source::Usb), sourceInt(classifyRound(o)));
    o.highSamples = 0;
    TEST_ASSERT_EQUAL_INT(sourceInt(Source::Usb), sourceInt(classifyRound(o)));

    // An exact tie resolves toward battery, never USB.
    o.totalSamples = 4;
    o.highSamples = 2;
    TEST_ASSERT_EQUAL_INT(sourceInt(Source::Battery), sourceInt(classifyRound(o)));

    o.totalSamples = 0;
    o.highSamples = 0;
    TEST_ASSERT_EQUAL_INT(sourceInt(Source::Unknown), sourceInt(classifyRound(o)));
}

void test_source_names_are_stable_for_logs() {
    TEST_ASSERT_EQUAL_STRING("unknown", sourceName(Source::Unknown));
    TEST_ASSERT_EQUAL_STRING("battery", sourceName(Source::Battery));
    TEST_ASSERT_EQUAL_STRING("usb", sourceName(Source::Usb));
}

// ─── Bounded source-evidence replay ───────────────────────────────────────

void test_source_evidence_replay_is_bounded_and_spaced() {
    constexpr EvidenceReplayConfig config{};
    TEST_ASSERT_EQUAL_UINT32(1000, config.intervalMs);
    TEST_ASSERT_EQUAL_UINT8(5, config.repetitions);
    EvidenceReplayState replay;
    armEvidenceReplay(replay, 1000, 3025, config);
    TEST_ASSERT_EQUAL_UINT32(3025, replay.usbConfirmationElapsedMs);

    TEST_ASSERT_FALSE(takeEvidenceReplay(replay, 1000, config));
    TEST_ASSERT_FALSE(takeEvidenceReplay(replay, 1000 + config.intervalMs - 1, config));
    for (uint8_t index = 0; index < config.repetitions; ++index) {
        const uint32_t dueAt = 1000 + static_cast<uint32_t>(index + 1) * config.intervalMs;
        TEST_ASSERT_TRUE(takeEvidenceReplay(replay, dueAt, config));
    }
    TEST_ASSERT_FALSE(
        takeEvidenceReplay(replay, 1000 + static_cast<uint32_t>(config.repetitions + 1) * config.intervalMs, config));
}

void test_source_evidence_replay_is_rollover_safe_and_rearms() {
    constexpr EvidenceReplayConfig config{};
    EvidenceReplayState replay;
    const uint32_t armedAt = 0xFFFFFF00u;
    armEvidenceReplay(replay, armedAt, 3000, config);
    TEST_ASSERT_FALSE(takeEvidenceReplay(replay, armedAt + config.intervalMs - 1, config));
    TEST_ASSERT_TRUE(takeEvidenceReplay(replay, armedAt + config.intervalMs, config));

    armEvidenceReplay(replay, 5000, 3025, config);
    TEST_ASSERT_EQUAL_UINT32(3025, replay.usbConfirmationElapsedMs);
    TEST_ASSERT_FALSE(takeEvidenceReplay(replay, 5000, config));
    TEST_ASSERT_TRUE(takeEvidenceReplay(replay, 5000 + config.intervalMs, config));
}

// ─── Power-button gating (defect C) ────────────────────────────────────────

void test_power_button_stays_enabled_when_the_adc_failed() {
    ButtonGateInputs in;
    in.managerInitialized = true;
    in.classification = Source::Battery;
    in.adcHealthy = false;
    in.batteryVoltageValid = false;
    TEST_ASSERT_TRUE_MESSAGE(powerButtonHandlingEnabled(in),
                             "a dead ADC must degrade voltage reporting only, never input handling");
}

void test_power_button_gate_ignores_adc_and_voltage_inputs_entirely() {
    const Source classifications[] = {Source::Unknown, Source::Battery, Source::Usb};
    for (Source classification : classifications) {
        for (int adc = 0; adc < 2; ++adc) {
            for (int voltage = 0; voltage < 2; ++voltage) {
                ButtonGateInputs in;
                in.managerInitialized = true;
                in.classification = classification;
                in.adcHealthy = adc != 0;
                in.batteryVoltageValid = voltage != 0;

                ButtonGateInputs reference = in;
                reference.adcHealthy = true;
                reference.batteryVoltageValid = true;

                TEST_ASSERT_EQUAL_MESSAGE(powerButtonHandlingEnabled(reference), powerButtonHandlingEnabled(in),
                                          "ADC health must not be an input to power-button gating");
            }
        }
    }
}

void test_power_button_is_enabled_while_the_source_is_still_unknown() {
    ButtonGateInputs in;
    in.managerInitialized = true;
    in.classification = Source::Unknown;
    TEST_ASSERT_TRUE_MESSAGE(powerButtonHandlingEnabled(in), "an unclassified boot must keep the button usable");
}

void test_power_button_is_disabled_only_by_confirmed_usb_or_no_init() {
    ButtonGateInputs usb;
    usb.managerInitialized = true;
    usb.classification = Source::Usb;
    usb.adcHealthy = true;
    usb.batteryVoltageValid = true;
    TEST_ASSERT_FALSE(powerButtonHandlingEnabled(usb));

    ButtonGateInputs uninitialised;
    uninitialised.managerInitialized = false;
    uninitialised.classification = Source::Battery;
    uninitialised.adcHealthy = true;
    uninitialised.batteryVoltageValid = true;
    TEST_ASSERT_FALSE(powerButtonHandlingEnabled(uninitialised));
}

// ─── Source contract over the untestable translation unit ──────────────────
//
// src/battery_manager.cpp cannot be compiled or linked natively, so the wiring
// between it and this policy is asserted textually. These fail loudly if the
// racy pattern is reintroduced or the policy is bypassed.

void test_battery_manager_source_is_readable() {
    const std::string source = readProjectFile("src/battery_manager.cpp");
    TEST_ASSERT_FALSE_MESSAGE(source.empty(), "failed to read src/battery_manager.cpp");
}

void test_battery_manager_delegates_classification_to_the_policy() {
    const std::string source = readProjectFile("src/battery_manager.cpp");
    TEST_ASSERT_TRUE(contains(source, "battery_source_policy.h"));
    TEST_ASSERT_TRUE(contains(source, "battery_source_policy::observe"));
    TEST_ASSERT_TRUE(contains(source, "battery_source_policy::roundDue"));
    TEST_ASSERT_TRUE(contains(source, "battery_source_policy::powerButtonHandlingEnabled"));
    TEST_ASSERT_TRUE(contains(source, "battery_source_policy::armEvidenceReplay"));
    TEST_ASSERT_TRUE(contains(source, "battery_source_policy::takeEvidenceReplay"));
    TEST_ASSERT_TRUE(contains(source, "[Battery] Power source stable: %s confirmation_ms=%lu"));
    // The single line that keeps isOnBattery() tied to the policy's answer.
    TEST_ASSERT_TRUE_MESSAGE(contains(source, "onBattery_ = result.onBattery;"),
                             "onBattery_ must be derived from the policy result, not computed locally");
}

void test_battery_manager_no_longer_carries_the_zero_spaced_sample_loop() {
    const std::string source = readProjectFile("src/battery_manager.cpp");
    TEST_ASSERT_FALSE_MESSAGE(contains(source, "bool detectedBattery = (highCount > samples / 2);"),
                              "the un-spaced majority vote must not be reintroduced");
    TEST_ASSERT_FALSE_MESSAGE(contains(source, "static uint32_t lastPowerCheckMs"),
                              "power-check timing belongs to the policy state, not a function static");
}

void test_battery_manager_power_button_is_not_gated_on_has_battery() {
    const std::string source = readProjectFile("src/battery_manager.cpp");
    const size_t start = source.find("bool BatteryManager::processPowerButton()");
    TEST_ASSERT_NOT_EQUAL_MESSAGE(std::string::npos, start, "processPowerButton() not found");
    const std::string body = source.substr(start);
    TEST_ASSERT_FALSE_MESSAGE(contains(body.substr(0, body.find("\n}")), "if (!hasBattery())"),
                              "defect C: the PWR button must not be gated on ADC-dependent hasBattery()");
}

int main() {
    UNITY_BEGIN();

    RUN_TEST(test_cold_start_has_no_classification_and_resolves_to_battery);
    RUN_TEST(test_cold_start_first_round_is_always_due);
    RUN_TEST(test_cold_start_low_pin_never_reports_usb_inside_the_holdoff);
    RUN_TEST(test_cold_start_usb_settles_only_after_the_persistence_window);
    RUN_TEST(test_pwr_wake_boot_reports_battery_throughout_and_confirms_battery);

    RUN_TEST(test_two_agreeing_rounds_change_the_classification);
    RUN_TEST(test_one_round_alone_never_changes_the_classification);
    RUN_TEST(test_two_disagreeing_rounds_hold_the_previous_classification);
    RUN_TEST(test_two_disagreeing_rounds_from_unknown_hold_unknown);
    RUN_TEST(test_alternating_transients_never_flip_the_classification);
    RUN_TEST(test_a_real_usb_attach_still_settles_when_no_button_is_involved);
    RUN_TEST(test_usb_to_battery_recovery_needs_no_holdoff);

    RUN_TEST(test_button_held_rounds_are_suppressed_and_change_nothing);
    RUN_TEST(test_long_button_hold_never_out_waits_the_usb_persistence_window);
    RUN_TEST(test_suppressed_round_cannot_be_half_of_an_agreement);
    RUN_TEST(test_suppressed_cold_start_stays_unknown_and_resolves_to_battery);
    RUN_TEST(test_empty_round_is_treated_as_suppressed);

    RUN_TEST(test_settled_rounds_run_at_the_cycle_interval);
    RUN_TEST(test_a_pending_change_schedules_a_spaced_confirmation_round);
    RUN_TEST(test_round_spacing_is_non_zero);

    RUN_TEST(test_round_due_is_rollover_safe);
    RUN_TEST(test_rollover_across_the_sampling_window_causes_no_spurious_change);
    RUN_TEST(test_usb_persistence_window_is_measured_across_a_rollover);

    RUN_TEST(test_majority_vote_boundaries);
    RUN_TEST(test_source_names_are_stable_for_logs);
    RUN_TEST(test_source_evidence_replay_is_bounded_and_spaced);
    RUN_TEST(test_source_evidence_replay_is_rollover_safe_and_rearms);

    RUN_TEST(test_power_button_stays_enabled_when_the_adc_failed);
    RUN_TEST(test_power_button_gate_ignores_adc_and_voltage_inputs_entirely);
    RUN_TEST(test_power_button_is_enabled_while_the_source_is_still_unknown);
    RUN_TEST(test_power_button_is_disabled_only_by_confirmed_usb_or_no_init);

    RUN_TEST(test_battery_manager_source_is_readable);
    RUN_TEST(test_battery_manager_delegates_classification_to_the_policy);
    RUN_TEST(test_battery_manager_no_longer_carries_the_zero_spaced_sample_loop);
    RUN_TEST(test_battery_manager_power_button_is_not_gated_on_has_battery);

    return UNITY_END();
}

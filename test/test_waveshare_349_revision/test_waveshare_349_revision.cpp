#include <unity.h>

#include "../../include/waveshare_349_revision.h"

namespace {

using waveshare_349::LevelSamples;
using waveshare_349::Revision;
using waveshare_349::RevisionEvidence;
using waveshare_349::ResetRoute;
using waveshare_349::classifyRevision;
using waveshare_349::routingFor;
using waveshare_349::withPinDirection;
using waveshare_349::withPinLevel;

constexpr uint16_t kMinimumSamples = 16;

LevelSamples stableLow(uint16_t samples = kMinimumSamples) {
    LevelSamples result;
    result.low = samples;
    return result;
}

LevelSamples stableHigh(uint16_t samples = kMinimumSamples) {
    LevelSamples result;
    result.high = samples;
    return result;
}

int revisionValue(Revision revision) {
    return static_cast<int>(revision);
}

void assertRevision(Revision expected, const RevisionEvidence& evidence) {
    TEST_ASSERT_EQUAL_INT(revisionValue(expected), revisionValue(classifyRevision(evidence, kMinimumSamples)));
}

} // namespace

void setUp() {}
void tearDown() {}

void test_v1_requires_gpio21_high_and_exio5_low() {
    assertRevision(Revision::V1, {stableHigh(), stableLow()});
}

void test_v2_requires_gpio21_low_and_exio5_high() {
    assertRevision(Revision::V2, {stableLow(), stableHigh()});
}

void test_all_low_and_all_high_are_unknown() {
    assertRevision(Revision::Unknown, {stableLow(), stableLow()});
    assertRevision(Revision::Unknown, {stableHigh(), stableHigh()});
}

void test_noisy_evidence_is_unknown() {
    LevelSamples noisyGpio = stableHigh();
    noisyGpio.low = 1;
    assertRevision(Revision::Unknown, {noisyGpio, stableLow()});
    assertRevision(Revision::Unknown, {stableLow(), noisyGpio});
}

void test_short_te_high_pulses_still_identify_the_pulled_high_reset_line() {
    LevelSamples pulsingTe;
    pulsingTe.low = 13;
    pulsingTe.high = 3;
    assertRevision(Revision::V1, {stableHigh(), pulsingTe});
    assertRevision(Revision::V2, {pulsingTe, stableHigh()});
}

void test_balanced_unstable_evidence_is_unknown() {
    LevelSamples unstable;
    unstable.low = 8;
    unstable.high = 8;
    assertRevision(Revision::Unknown, {stableHigh(), unstable});
    assertRevision(Revision::Unknown, {unstable, stableHigh()});
}

void test_missing_or_failed_reads_are_unknown() {
    LevelSamples missing;
    LevelSamples failed = stableHigh();
    failed.readFailures = 1;
    assertRevision(Revision::Unknown, {missing, stableLow()});
    assertRevision(Revision::Unknown, {stableLow(), missing});
    assertRevision(Revision::Unknown, {stableLow(), failed});
}

void test_too_few_samples_are_unknown() {
    assertRevision(Revision::Unknown, {stableHigh(kMinimumSamples - 1), stableLow(kMinimumSamples - 1)});
    assertRevision(Revision::Unknown, {stableLow(kMinimumSamples - 1), stableHigh(kMinimumSamples - 1)});
}

void test_revision_routes_only_the_matching_backlight_and_reset() {
    const auto v1 = routingFor(Revision::V1);
    TEST_ASSERT_EQUAL_INT(8, v1.backlightGpio);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ResetRoute::Gpio21), static_cast<int>(v1.resetRoute));

    const auto v2 = routingFor(Revision::V2);
    TEST_ASSERT_EQUAL_INT(42, v2.backlightGpio);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ResetRoute::Exio5), static_cast<int>(v2.resetRoute));

    const auto unknown = routingFor(Revision::Unknown);
    TEST_ASSERT_EQUAL_INT(-1, unknown.backlightGpio);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ResetRoute::None), static_cast<int>(unknown.resetRoute));
}

void test_expander_updates_preserve_power_latch_and_audio_bits() {
    constexpr uint8_t p6 = 1u << 6;
    constexpr uint8_t p7 = 1u << 7;
    const uint8_t initialOutput = static_cast<uint8_t>(p6 | p7);
    const uint8_t gateEnabled = withPinLevel(initialOutput, 1, true);
    const uint8_t resetHigh = withPinLevel(gateEnabled, 5, true);
    TEST_ASSERT_BITS_HIGH(static_cast<uint8_t>(p6 | p7), resetHigh);

    const uint8_t initialConfig = 0x3F; // P6/P7 already outputs.
    const uint8_t gateOutput = withPinDirection(initialConfig, 1, true);
    const uint8_t resetOutput = withPinDirection(gateOutput, 5, true);
    TEST_ASSERT_BITS_LOW(static_cast<uint8_t>(p6 | p7), resetOutput);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_v1_requires_gpio21_high_and_exio5_low);
    RUN_TEST(test_v2_requires_gpio21_low_and_exio5_high);
    RUN_TEST(test_all_low_and_all_high_are_unknown);
    RUN_TEST(test_noisy_evidence_is_unknown);
    RUN_TEST(test_short_te_high_pulses_still_identify_the_pulled_high_reset_line);
    RUN_TEST(test_balanced_unstable_evidence_is_unknown);
    RUN_TEST(test_missing_or_failed_reads_are_unknown);
    RUN_TEST(test_too_few_samples_are_unknown);
    RUN_TEST(test_revision_routes_only_the_matching_backlight_and_reset);
    RUN_TEST(test_expander_updates_preserve_power_latch_and_audio_bits);
    return UNITY_END();
}

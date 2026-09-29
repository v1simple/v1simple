#include <unity.h>
#include "../../src/modules/in_the_box/in_the_box_module.h"

namespace {
InTheBoxModule module;
V1InTheBoxSettings settings;

AlertData radar(Band band, uint32_t frequency) {
    return AlertData::create(band, DIR_FRONT, 3, 0, frequency);
}

InTheBoxDecision process(AlertData* alerts, size_t count, bool laser = false) {
    return module.process(settings, alerts, count, laser);
}

void enableActions() {
    for (auto& band : settings.bands) {
        band.muteOutside = true;
        band.unmuteInside = true;
    }
}
} // namespace

void setUp() {
    module.reset();
    settings = V1InTheBoxSettings{};
}
void tearDown() {}

void test_default_boxes_classify_all_bands_without_audio_actions() {
    AlertData alerts[] = {radar(BAND_X, 10525), radar(BAND_KU, 13450), radar(BAND_K, 24150),
                          radar(BAND_KA, 33800), radar(BAND_KA, 34700), radar(BAND_KA, 35500)};
    for (const auto& alert : alerts) {
        TEST_ASSERT_EQUAL(InTheBoxClassification::Inside, InTheBoxModule::classify(settings, alert));
    }
    const auto decision = process(alerts, 6);
    TEST_ASSERT_TRUE(decision.anyInside);
    TEST_ASSERT_FALSE(decision.allOutsideMuteEligible);
    TEST_ASSERT_FALSE(decision.newInsideUnmute);
    AlertData outside = radar(BAND_KA, 34000);
    TEST_ASSERT_FALSE(process(&outside, 1).allOutsideMuteEligible);
}

void test_every_box_uses_inclusive_edges_and_enabled_flag() {
    const Band bands[] = {BAND_X, BAND_KU, BAND_K, BAND_KA, BAND_KA, BAND_KA};
    for (size_t i = 0; i < settings.boxes.size(); ++i) {
        const auto& box = settings.boxes[i];
        TEST_ASSERT_EQUAL(InTheBoxClassification::Inside,
                          InTheBoxModule::classify(settings, radar(bands[i], box.lowerMHz)));
        TEST_ASSERT_EQUAL(InTheBoxClassification::Inside,
                          InTheBoxModule::classify(settings, radar(bands[i], box.upperMHz)));
        TEST_ASSERT_EQUAL(i < 2 ? InTheBoxClassification::Unknown : InTheBoxClassification::Outside,
                          InTheBoxModule::classify(settings, radar(bands[i], box.lowerMHz - 1)));
        TEST_ASSERT_EQUAL(i < 3 ? InTheBoxClassification::Unknown : InTheBoxClassification::Outside,
                          InTheBoxModule::classify(settings, radar(bands[i], box.upperMHz + 1)));
        settings.boxes[i].enabled = false;
        TEST_ASSERT_EQUAL(InTheBoxClassification::Outside,
                          InTheBoxModule::classify(settings, radar(bands[i], box.lowerMHz)));
    }
}

void test_muting_requires_every_row_outside_and_every_band_opted_in() {
    enableActions();
    AlertData alerts[] = {radar(BAND_K, 24040), radar(BAND_KA, 34000)};
    TEST_ASSERT_TRUE(process(alerts, 2).allOutsideMuteEligible);
    settings.bands[3].muteOutside = false;
    TEST_ASSERT_FALSE(process(alerts, 2).allOutsideMuteEligible);
    settings.bands[3].muteOutside = true;
    alerts[1].frequency = 34700;
    const auto mixed = process(alerts, 2);
    TEST_ASSERT_FALSE(mixed.allOutsideMuteEligible);
    TEST_ASSERT_TRUE(mixed.anyInside);
    TEST_ASSERT_TRUE(mixed.newInsideUnmute);
}

void test_disabled_boxes_allow_outside_policy_and_overlapping_boxes_form_union() {
    enableActions();
    for (auto& box : settings.boxes) box.enabled = false;
    AlertData alert = radar(BAND_KA, 34700);
    TEST_ASSERT_TRUE(process(&alert, 1).allOutsideMuteEligible);
    settings.boxes[3] = {true, 34000, 34800};
    settings.boxes[4] = {true, 34700, 35000};
    alert.frequency = 34900;
    const auto inside = process(&alert, 1);
    TEST_ASSERT_TRUE(inside.anyInside);
    TEST_ASSERT_TRUE(inside.newInsideUnmute);
    TEST_ASSERT_FALSE(inside.allOutsideMuteEligible);
}

void test_invalid_rows_unknown_bands_and_laser_prevent_muting() {
    enableActions();
    AlertData alerts[] = {radar(BAND_KA, 34000), radar(BAND_K, 24040)};
    TEST_ASSERT_FALSE(process(alerts, 2, true).allOutsideMuteEligible);
    alerts[1].isValid = false;
    TEST_ASSERT_FALSE(process(alerts, 2).allOutsideMuteEligible);
    alerts[1] = radar(BAND_NONE, 24040);
    TEST_ASSERT_FALSE(process(alerts, 2).allOutsideMuteEligible);
    alerts[1] = radar(static_cast<Band>(BAND_K | BAND_KA), 24040);
    TEST_ASSERT_FALSE(process(alerts, 2).allOutsideMuteEligible);
    alerts[1] = radar(BAND_LASER, 0);
    TEST_ASSERT_FALSE(process(alerts, 2).allOutsideMuteEligible);
    alerts[1] = radar(BAND_K, 0);
    TEST_ASSERT_FALSE(process(alerts, 2).allOutsideMuteEligible);
    alerts[1].frequency = 65536;
    TEST_ASSERT_FALSE(process(alerts, 2).allOutsideMuteEligible);
    TEST_ASSERT_FALSE(process(nullptr, 0).allOutsideMuteEligible);
    TEST_ASSERT_FALSE(process(nullptr, 1).allOutsideMuteEligible);
    TEST_ASSERT_FALSE(process(alerts, 16).allOutsideMuteEligible);
}

void test_invalid_settings_do_not_classify_or_request_audio_actions() {
    enableActions();
    settings.boxes[2].lowerMHz = settings.boxes[2].upperMHz + 1;
    AlertData alert = radar(BAND_KA, 34700);
    TEST_ASSERT_EQUAL(InTheBoxClassification::Unknown, InTheBoxModule::classify(settings, alert));
    const auto decision = process(&alert, 1);
    TEST_ASSERT_FALSE(decision.anyInside);
    TEST_ASSERT_FALSE(decision.newInsideUnmute);
    TEST_ASSERT_FALSE(decision.allOutsideMuteEligible);
}

void test_gen2_physical_band_edges_are_valid_and_other_frequencies_unknown() {
    const Band bands[] = {BAND_X, BAND_KU, BAND_K, BAND_KA};
    const uint32_t lower[] = {10500, 13400, 23900, 33400};
    const uint32_t upper[] = {10550, 13500, 24250, 36000};
    for (size_t i = 0; i < 4; ++i) {
        settings.bands[i].muteOutside = true;
        settings.boxes[i].enabled = false;
        AlertData alert = radar(bands[i], lower[i]);
        TEST_ASSERT_TRUE(process(&alert, 1).allOutsideMuteEligible);
        alert.frequency = upper[i];
        TEST_ASSERT_TRUE(process(&alert, 1).allOutsideMuteEligible);
        alert.frequency = lower[i] - 1;
        TEST_ASSERT_EQUAL(InTheBoxClassification::Unknown, InTheBoxModule::classify(settings, alert));
        TEST_ASSERT_FALSE(process(&alert, 1).allOutsideMuteEligible);
        alert.frequency = upper[i] + 1;
        TEST_ASSERT_EQUAL(InTheBoxClassification::Unknown, InTheBoxModule::classify(settings, alert));
        TEST_ASSERT_FALSE(process(&alert, 1).allOutsideMuteEligible);
    }
}

void test_photo_and_junk_classify_by_physical_band_without_modifying_rows() {
    enableActions();
    AlertData photo = radar(BAND_K, 24040);
    photo.photoType = 1;
    photo.isJunk = true;
    TEST_ASSERT_TRUE(process(&photo, 1).allOutsideMuteEligible);
    photo.frequency = 24150;
    const auto inside = process(&photo, 1);
    TEST_ASSERT_TRUE(inside.anyInside);
    TEST_ASSERT_TRUE(inside.newInsideUnmute);
    TEST_ASSERT_EQUAL_UINT8(1, photo.photoType);
    TEST_ASSERT_TRUE(photo.isJunk);
    TEST_ASSERT_TRUE(photo.isValid);
    TEST_ASSERT_EQUAL_UINT32(24150, photo.frequency);
}

void test_inside_unmute_is_once_per_encounter_and_per_band_option() {
    AlertData alert = radar(BAND_K, 24150);
    settings.bands[3].unmuteInside = true;
    TEST_ASSERT_FALSE(process(&alert, 1).newInsideUnmute);
    settings.bands[2].unmuteInside = true;
    TEST_ASSERT_FALSE(process(&alert, 1).newInsideUnmute);
    module.reset();
    TEST_ASSERT_TRUE(process(&alert, 1).newInsideUnmute);
    TEST_ASSERT_FALSE(process(&alert, 1).newInsideUnmute);
    process(nullptr, 0);
    TEST_ASSERT_TRUE(process(&alert, 1).newInsideUnmute);
}

void test_jitter_uses_last_frequency_and_does_not_retrigger_after_box_reentry() {
    enableActions();
    AlertData alert = radar(BAND_K, 24050);
    TEST_ASSERT_TRUE(process(&alert, 1).newInsideUnmute);
    alert.frequency = 24045;
    TEST_ASSERT_TRUE(process(&alert, 1).allOutsideMuteEligible);
    alert.frequency = 24040;
    TEST_ASSERT_FALSE(process(&alert, 1).newInsideUnmute);
    alert.frequency = 24045;
    TEST_ASSERT_FALSE(process(&alert, 1).newInsideUnmute);
    alert.frequency = 24050;
    TEST_ASSERT_FALSE(process(&alert, 1).newInsideUnmute);
    alert.frequency = 24056;
    TEST_ASSERT_TRUE(process(&alert, 1).newInsideUnmute);
}

void test_outside_encounter_first_entry_requests_unmute_once() {
    enableActions();
    AlertData alert = radar(BAND_K, 24048);
    TEST_ASSERT_FALSE(process(&alert, 1).newInsideUnmute);
    alert.frequency = 24050;
    TEST_ASSERT_TRUE(process(&alert, 1).newInsideUnmute);
    alert.frequency = 24049;
    TEST_ASSERT_FALSE(process(&alert, 1).newInsideUnmute);
    alert.frequency = 24050;
    TEST_ASSERT_FALSE(process(&alert, 1).newInsideUnmute);
}

void test_table_order_assignment_and_priority_changes_do_not_create_encounters() {
    enableActions();
    AlertData alerts[] = {radar(BAND_KA, 34700), radar(BAND_KA, 35500), radar(BAND_K, 24150)};
    TEST_ASSERT_TRUE(process(alerts, 3).newInsideUnmute);
    const AlertData first = alerts[0];
    alerts[0] = alerts[2];
    alerts[2] = first;
    for (size_t i = 0; i < 3; ++i) {
        alerts[i].frequency += 3;
        alerts[i].v1Index = static_cast<uint8_t>(3 - i);
        alerts[i].isPriority = i == 2;
        alerts[i].direction = DIR_REAR;
    }
    TEST_ASSERT_FALSE(process(alerts, 3).newInsideUnmute);
}

void test_one_to_one_matching_preserves_duplicate_count_and_new_arrivals() {
    enableActions();
    AlertData alerts[] = {radar(BAND_K, 24150), radar(BAND_K, 24150)};
    TEST_ASSERT_TRUE(process(alerts, 1).newInsideUnmute);
    TEST_ASSERT_TRUE(process(alerts, 2).newInsideUnmute);
    TEST_ASSERT_FALSE(process(alerts, 2).newInsideUnmute);
    TEST_ASSERT_FALSE(process(alerts, 1).newInsideUnmute);
    TEST_ASSERT_TRUE(process(alerts, 2).newInsideUnmute);
}

void test_nearest_matching_reassigns_instead_of_inventing_a_new_encounter() {
    enableActions();
    AlertData alerts[] = {radar(BAND_K, 24100), radar(BAND_K, 24105)};
    TEST_ASSERT_TRUE(process(alerts, 2).newInsideUnmute);
    alerts[0].frequency = 24104;
    alerts[1].frequency = 24110;
    TEST_ASSERT_FALSE(process(alerts, 2).newInsideUnmute);
}

void test_full_fifteen_alert_capacity_and_reset() {
    enableActions();
    AlertData alerts[InTheBoxModule::MAX_ALERTS];
    for (auto& alert : alerts) alert = radar(BAND_KA, 34700);
    TEST_ASSERT_TRUE(process(alerts, InTheBoxModule::MAX_ALERTS).newInsideUnmute);
    TEST_ASSERT_FALSE(process(alerts, InTheBoxModule::MAX_ALERTS).newInsideUnmute);
    module.reset();
    TEST_ASSERT_TRUE(process(alerts, InTheBoxModule::MAX_ALERTS).newInsideUnmute);
}

void test_unknown_tables_preserve_encounters_and_defer_new_inside_unmute() {
    enableActions();
    AlertData alert = radar(BAND_K, 24150);
    TEST_ASSERT_TRUE(process(&alert, 1).newInsideUnmute);
    alert.isValid = false;
    const auto invalid = process(&alert, 1);
    TEST_ASSERT_FALSE(invalid.allOutsideMuteEligible);
    TEST_ASSERT_FALSE(invalid.newInsideUnmute);
    alert.isValid = true;
    TEST_ASSERT_FALSE(process(&alert, 1).newInsideUnmute);

    AlertData mixed[] = {radar(BAND_KA, 34700), radar(BAND_NONE, 0)};
    const auto uncertain = process(mixed, 2);
    TEST_ASSERT_TRUE(uncertain.anyInside);
    TEST_ASSERT_FALSE(uncertain.allOutsideMuteEligible);
    TEST_ASSERT_FALSE(uncertain.newInsideUnmute);
    TEST_ASSERT_TRUE(process(mixed, 1).newInsideUnmute);
    process(nullptr, 1);
    TEST_ASSERT_FALSE(process(mixed, 1).newInsideUnmute);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_default_boxes_classify_all_bands_without_audio_actions);
    RUN_TEST(test_every_box_uses_inclusive_edges_and_enabled_flag);
    RUN_TEST(test_muting_requires_every_row_outside_and_every_band_opted_in);
    RUN_TEST(test_disabled_boxes_allow_outside_policy_and_overlapping_boxes_form_union);
    RUN_TEST(test_invalid_rows_unknown_bands_and_laser_prevent_muting);
    RUN_TEST(test_invalid_settings_do_not_classify_or_request_audio_actions);
    RUN_TEST(test_gen2_physical_band_edges_are_valid_and_other_frequencies_unknown);
    RUN_TEST(test_photo_and_junk_classify_by_physical_band_without_modifying_rows);
    RUN_TEST(test_inside_unmute_is_once_per_encounter_and_per_band_option);
    RUN_TEST(test_jitter_uses_last_frequency_and_does_not_retrigger_after_box_reentry);
    RUN_TEST(test_outside_encounter_first_entry_requests_unmute_once);
    RUN_TEST(test_table_order_assignment_and_priority_changes_do_not_create_encounters);
    RUN_TEST(test_one_to_one_matching_preserves_duplicate_count_and_new_arrivals);
    RUN_TEST(test_nearest_matching_reassigns_instead_of_inventing_a_new_encounter);
    RUN_TEST(test_full_fifteen_alert_capacity_and_reset);
    RUN_TEST(test_unknown_tables_preserve_encounters_and_defer_new_inside_unmute);
    return UNITY_END();
}

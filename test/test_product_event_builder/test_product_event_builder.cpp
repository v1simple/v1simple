#include <unity.h>

#include <vector>

#include "../../src/modules/event_log/product_event.h"

namespace {

std::vector<ProductEvent> captured;

bool captureEvent(const ProductEvent& event, void*) {
    captured.push_back(event);
    return true;
}

} // namespace

void setUp() { captured.clear(); }
void tearDown() {}

void test_v1_begin_change_end_and_dedup_use_full_normalized_table() {
    ProductEventBuilder builder;
    builder.begin(captureEvent, nullptr);
    AlertData alerts[2] = {
        AlertData::create(BAND_K, DIR_FRONT, 4, 0, 34567),
        AlertData::create(BAND_KA, DIR_REAR, 2, 1, 34700),
    };

    builder.observeV1Table(alerts, 2, 1, 100);
    builder.observeV1Table(alerts, 2, 1, 101);
    alerts[0].frontStrength = 6;
    builder.observeV1Table(alerts, 2, 0, 200);
    builder.observeV1Table(alerts, 0, 0, 300);

    TEST_ASSERT_EQUAL_UINT32(3, captured.size());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ProductEventKind::BEGIN), static_cast<uint8_t>(captured[0].kind));
    TEST_ASSERT_EQUAL_UINT8(2, captured[0].data.v1.count);
    TEST_ASSERT_EQUAL_UINT8(1, captured[0].data.v1.alerts[1].priority);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ProductEventKind::CHANGE_EVENT),
                            static_cast<uint8_t>(captured[1].kind));
    TEST_ASSERT_EQUAL_UINT8(6, captured[1].data.v1.alerts[0].frontStrength);
    TEST_ASSERT_EQUAL_UINT8(1, captured[1].data.v1.alerts[0].priority);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ProductEventKind::END), static_cast<uint8_t>(captured[2].kind));
}

void test_alp_sparse_events_deduplicate_equivalent_detection_and_state() {
    ProductEventBuilder builder;
    builder.begin(captureEvent, nullptr);
    AlpProductObservation observation{};
    observation.connected = true;
    observation.active = true;
    observation.state = ProductAlpState::TARGETED;
    observation.direction = 1;
    observation.gun = 2;
    observation.detectGeneration = 1;
    observation.detectRaw[0] = 0x98;
    observation.detectRaw[1] = 0x02;

    builder.observeAlp(observation, 1000);
    observation.detectGeneration = 2;
    builder.observeAlp(observation, 1010);
    observation.state = ProductAlpState::LID;
    builder.observeAlp(observation, 1100);
    observation.active = false;
    builder.observeAlp(observation, 1200);

    TEST_ASSERT_EQUAL_UINT32(5, captured.size());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ProductEventKind::BEGIN), static_cast<uint8_t>(captured[0].kind));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ProductEventKind::DETECT), static_cast<uint8_t>(captured[1].kind));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ProductEventKind::GUN), static_cast<uint8_t>(captured[2].kind));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ProductEventKind::STATE), static_cast<uint8_t>(captured[3].kind));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ProductEventKind::END), static_cast<uint8_t>(captured[4].kind));
}

void test_meaningful_link_loss_and_restoration_are_session_scoped() {
    ProductEventBuilder builder;
    builder.begin(captureEvent, nullptr);
    AlertData alert = AlertData::create(BAND_KA, DIR_FRONT, 8, 0, 34700);
    builder.observeV1Link(false, 1);
    builder.observeV1Table(&alert, 1, 0, 2);
    builder.observeV1Link(false, 3);
    builder.observeV1Link(false, 4);
    builder.observeV1Link(true, 5);

    TEST_ASSERT_EQUAL_UINT32(3, captured.size());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ProductEventKind::BEGIN), static_cast<uint8_t>(captured[0].kind));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ProductEventKind::LINK_LOST), static_cast<uint8_t>(captured[1].kind));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ProductEventKind::LINK_RESTORED),
                            static_cast<uint8_t>(captured[2].kind));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_v1_begin_change_end_and_dedup_use_full_normalized_table);
    RUN_TEST(test_alp_sparse_events_deduplicate_equivalent_detection_and_state);
    RUN_TEST(test_meaningful_link_loss_and_restoration_are_session_scoped);
    return UNITY_END();
}

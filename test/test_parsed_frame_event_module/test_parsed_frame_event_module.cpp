#include <unity.h>

#include "../../src/modules/system/parsed_frame_event_module.cpp"

static SystemEventBus bus;

void setUp() {
    bus.reset();
}

void tearDown() {}

void test_queue_signal_passthrough_when_bus_empty() {
    const ParsedFrameSignal signal = ParsedFrameEventModule::collect(true, 1234, bus);
    TEST_ASSERT_TRUE(signal.parsedReady);
    TEST_ASSERT_EQUAL_UINT32(1234, signal.parsedTsMs);
}

void test_bus_frame_makes_signal_ready_when_queue_false() {
    SystemEvent event;
    event.type = SystemEventType::BLE_FRAME_PARSED;
    event.tsMs = 2222;
    TEST_ASSERT_TRUE(bus.publish(event));

    const ParsedFrameSignal signal = ParsedFrameEventModule::collect(false, 0, bus);
    TEST_ASSERT_TRUE(signal.parsedReady);
    TEST_ASSERT_EQUAL_UINT32(2222, signal.parsedTsMs);
}

void test_zero_ts_frame_keeps_existing_timestamp() {
    SystemEvent event;
    event.type = SystemEventType::BLE_FRAME_PARSED;
    event.tsMs = 0;
    TEST_ASSERT_TRUE(bus.publish(event));

    const ParsedFrameSignal signal = ParsedFrameEventModule::collect(true, 777, bus);
    TEST_ASSERT_TRUE(signal.parsedReady);
    TEST_ASSERT_EQUAL_UINT32(777, signal.parsedTsMs);
}

void test_last_nonzero_frame_ts_wins() {
    SystemEvent a;
    a.type = SystemEventType::BLE_FRAME_PARSED;
    a.tsMs = 1000;
    TEST_ASSERT_TRUE(bus.publish(a));

    SystemEvent b;
    b.type = SystemEventType::BLE_FRAME_PARSED;
    b.tsMs = 0;
    TEST_ASSERT_TRUE(bus.publish(b));

    SystemEvent c;
    c.type = SystemEventType::BLE_FRAME_PARSED;
    c.tsMs = 3000;
    TEST_ASSERT_TRUE(bus.publish(c));

    const ParsedFrameSignal signal = ParsedFrameEventModule::collect(false, 555, bus);
    TEST_ASSERT_TRUE(signal.parsedReady);
    TEST_ASSERT_EQUAL_UINT32(3000, signal.parsedTsMs);
}

void test_non_frame_events_are_preserved() {
    SystemEvent control;
    control.type = SystemEventType::BLE_CONNECTED;
    control.tsMs = 42;
    control.seq = 7;
    TEST_ASSERT_TRUE(bus.publish(control));

    SystemEvent frame;
    frame.type = SystemEventType::BLE_FRAME_PARSED;
    frame.tsMs = 3141;
    TEST_ASSERT_TRUE(bus.publish(frame));

    const ParsedFrameSignal signal = ParsedFrameEventModule::collect(false, 0, bus);
    TEST_ASSERT_TRUE(signal.parsedReady);
    TEST_ASSERT_EQUAL_UINT32(3141, signal.parsedTsMs);

    SystemEvent out;
    TEST_ASSERT_TRUE(bus.consume(out));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SystemEventType::BLE_CONNECTED),
                            static_cast<uint8_t>(out.type));
    TEST_ASSERT_EQUAL_UINT32(42, out.tsMs);
    TEST_ASSERT_EQUAL_UINT32(7, out.seq);
}

void test_alp_state_changed_triggers_parsed_ready() {
    SystemEvent alpEvent;
    alpEvent.type = SystemEventType::ALP_STATE_CHANGED;
    alpEvent.tsMs = 5000;
    alpEvent.detail = 0x10;
    TEST_ASSERT_TRUE(bus.publish(alpEvent));

    const ParsedFrameSignal signal = ParsedFrameEventModule::collect(false, 0, bus);
    TEST_ASSERT_TRUE(signal.parsedReady);
    // ALP timestamps must not pollute V1 timing
    TEST_ASSERT_EQUAL_UINT32(0, signal.parsedTsMs);
}

void test_both_sources_drain() {
    SystemEvent bleFrame;
    bleFrame.type = SystemEventType::BLE_FRAME_PARSED;
    bleFrame.tsMs = 1000;
    TEST_ASSERT_TRUE(bus.publish(bleFrame));

    SystemEvent alpEvent;
    alpEvent.type = SystemEventType::ALP_STATE_CHANGED;
    alpEvent.tsMs = 2000;
    TEST_ASSERT_TRUE(bus.publish(alpEvent));

    const ParsedFrameSignal signal = ParsedFrameEventModule::collect(false, 0, bus);
    TEST_ASSERT_TRUE(signal.parsedReady);
    // BLE frame timestamp wins (it arrived first and was non-zero)
    TEST_ASSERT_EQUAL_UINT32(1000, signal.parsedTsMs);

    // Bus should be empty
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(bus.size()));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_queue_signal_passthrough_when_bus_empty);
    RUN_TEST(test_bus_frame_makes_signal_ready_when_queue_false);
    RUN_TEST(test_zero_ts_frame_keeps_existing_timestamp);
    RUN_TEST(test_last_nonzero_frame_ts_wins);
    RUN_TEST(test_non_frame_events_are_preserved);
    RUN_TEST(test_alp_state_changed_triggers_parsed_ready);
    RUN_TEST(test_both_sources_drain);
    return UNITY_END();
}

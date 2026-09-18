#include <unity.h>

#include <cstdint>
#include <vector>

#include "../mocks/Arduino.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

// Include the production parser before the queue's mock include path can
// substitute its test double. This suite proves the queue signal and parser
// recovery contract together.
#include "../../src/packet_parser.h"
#include "../../src/packet_parser.cpp"
#include "../../src/packet_parser_alerts.cpp"

#include "../mocks/ble_client.h"
#include "../mocks/modules/power/power_module.h"
#include "../mocks/v1_profiles.h"

class DisplayPreviewModule {
  public:
    bool isRunning() const { return running; }
    void cancel() { running = false; }
    bool running = false;
};

#define private public
#include "../../src/modules/ble/ble_queue_module.h"
#undef private
#include "../../src/modules/ble/ble_queue_module.cpp"

namespace {

constexpr uint32_t kSession = 9;
constexpr uint16_t kCharacteristic = 0xB2CE;

BleQueueModule queue;
PacketParser parser;
V1BLEClient client;
V1ProfileManager profiles;
DisplayPreviewModule preview;
PowerModule power;

std::vector<uint8_t> makeAlertFrame(uint8_t index, uint8_t count, uint16_t frequency,
                                    uint8_t bandArrow, uint8_t aux0) {
    std::vector<uint8_t> frame{
        ESP_PACKET_START, 0xD8, 0xEA, PACKET_ID_ALERT_DATA, 0x08,
        static_cast<uint8_t>(((index & 0x0F) << 4) | (count & 0x0F)),
        static_cast<uint8_t>(frequency >> 8), static_cast<uint8_t>(frequency),
        0x90, 0x00, bandArrow, aux0,
    };
    uint8_t checksum = 0;
    for (uint8_t value : frame) checksum = static_cast<uint8_t>(checksum + value);
    frame.push_back(checksum);
    frame.push_back(ESP_PACKET_END);
    return frame;
}

void deliver(const std::vector<uint8_t>& frame, uint32_t ingressSequence, uint32_t nowMs) {
    TEST_ASSERT_TRUE(queue.tryOnNotify(frame.data(), frame.size(), kCharacteristic, kSession,
                                      nowMs, ingressSequence));
    queue.process();
}

} // namespace

void setUp() {
    queue.end();
    parser = PacketParser{};
    client.reset();
    profiles.reset();
    preview = DisplayPreviewModule{};
    power.reset();
    HealthCounters::reset();
    BleQueueModule::Config config;
    TEST_ASSERT_TRUE(queue.begin(&client, &parser, &profiles, &preview, &power, config));
    queue.openSession(kSession);
}

void tearDown() {
    queue.end();
}

void test_queue_gap_prevents_cross_cycle_alert_table_publication() {
    const auto oldRow1 = makeAlertFrame(1, 2, 24150, 0x24, 0x80);
    const auto freshRow2 = makeAlertFrame(2, 2, 34700, 0x22, 0x00);
    const auto freshRow1 = makeAlertFrame(1, 2, 24200, 0x24, 0x80);

    deliver(oldRow1, 1, 100);
    TEST_ASSERT_EQUAL_UINT32(0, parser.getAlertCount());

    // Sequence 2 is absent. The queue marks the boundary before sequence 3 is
    // parsed, so row 2 cannot complete a table with row 1 from before the gap.
    deliver(freshRow2, 3, 101);
    deliver(freshRow1, 4, 102);
    TEST_ASSERT_EQUAL_UINT32(0, parser.getAlertCount());

    deliver(freshRow2, 5, 103);
    TEST_ASSERT_EQUAL_UINT32(2, parser.getAlertCount());
    TEST_ASSERT_EQUAL_UINT16(24200, parser.getAllAlerts()[0].frequency);
    TEST_ASSERT_EQUAL_UINT16(34700, parser.getAllAlerts()[1].frequency);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_queue_gap_prevents_cross_cycle_alert_table_publication);
    return UNITY_END();
}

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

void test_spec_busy_broadcast_reaches_request_owner_but_targeted_busy_does_not() {
    // ESP 3.016 p40 and p11: one pending ID, General Broadcast D8.
    // Fixed wire vectors include independently summed checksums.
    const std::vector<uint8_t> broadcast{0xAA, 0xD8, 0xEA, 0x66, 0x02, 0x19, 0xED, 0xAB};
    const std::vector<uint8_t> targeted{0xAA, 0xD6, 0xEA, 0x66, 0x02, 0x19, 0xEB, 0xAB};
    deliver(targeted, 1, 100);
    TEST_ASSERT_EQUAL_INT(0, client.onV1BusyCalls);
    TEST_ASSERT_FALSE(queue.consumeParsedFlag());

    deliver(broadcast, 2, 101);
    TEST_ASSERT_EQUAL_INT(1, client.onV1BusyCalls);
    TEST_ASSERT_TRUE(queue.consumeParsedFlag());
    TEST_ASSERT_EQUAL_UINT(1, client.lastBusyPacketIds.size());
    TEST_ASSERT_EQUAL_HEX8(PACKET_ID_REQ_MAX_SWEEP_INDEX, client.lastBusyPacketIds[0]);
}

void test_older_gen2_targeted_alerts_and_current_broadcasts_reach_the_alert_table() {
    // ESP 3.016 p37 note 2 and p54: respAlertData changed from requester
    // destination D6 to General Broadcast D8 at V1 version 4.1031.
    // Fixed wire vectors use independently summed checksums. Row 1/1 is a
    // 34,700 MHz Ka priority ahead; the all-zero row clears the table.
    const std::vector<uint8_t> version41030{0xAA, 0xD6, 0xEA, 0x02, 0x08,
                                           'v', '4', '.', '1', '0', '3', '0', 0x10, 0xAB};
    const std::vector<uint8_t> targeted{0xAA, 0xD6, 0xEA, 0x43, 0x08,
                                       0x11, 0x87, 0x8C, 0xB0, 0, 0x22, 0x80, 0x2B, 0xAB};
    const std::vector<uint8_t> clearTargeted{0xAA, 0xD6, 0xEA, 0x43, 0x08,
                                            0, 0, 0, 0, 0, 0, 0, 0xB5, 0xAB};
    const std::vector<uint8_t> broadcast{0xAA, 0xD8, 0xEA, 0x43, 0x08,
                                        0x11, 0x87, 0x8C, 0xB0, 0, 0x22, 0x80, 0x2D, 0xAB};
    deliver(version41030, 1, 100);
    TEST_ASSERT_EQUAL_UINT32(41030, parser.getDisplayState().v1FirmwareVersion);
    queue.consumeParsedFlag();
    deliver(targeted, 2, 101);
    TEST_ASSERT_TRUE(queue.consumeParsedFlag());
    AlertData priority;
    TEST_ASSERT_TRUE(parser.getRenderablePriorityAlert(priority));
    TEST_ASSERT_EQUAL_UINT16(34700, priority.frequency);
    TEST_ASSERT_EQUAL(BAND_KA, priority.band);
    TEST_ASSERT_EQUAL(DIR_FRONT, priority.direction);
    deliver(clearTargeted, 3, 102);
    TEST_ASSERT_TRUE(queue.consumeParsedFlag());
    TEST_ASSERT_EQUAL_UINT32(0, parser.getAlertCount());

    auto version41031 = version41030;
    version41031[11] = '1';
    ++version41031[12];
    deliver(version41031, 4, 103);
    TEST_ASSERT_EQUAL_UINT32(41031, parser.getDisplayState().v1FirmwareVersion);
    deliver(broadcast, 5, 104);
    TEST_ASSERT_TRUE(queue.consumeParsedFlag());
    TEST_ASSERT_TRUE(parser.getRenderablePriorityAlert(priority));
    TEST_ASSERT_EQUAL_UINT16(34700, priority.frequency);
}

void test_targeted_alerts_do_not_wait_for_firmware_version_discovery() {
    const std::vector<uint8_t> targeted{0xAA, 0xD6, 0xEA, 0x43, 0x08,
                                       0x11, 0x87, 0x8C, 0xB0, 0, 0x22, 0x80, 0x2B, 0xAB};
    TEST_ASSERT_FALSE(parser.getDisplayState().hasV1Version);
    deliver(targeted, 1, 100);
    TEST_ASSERT_TRUE(queue.consumeParsedFlag());
    AlertData priority;
    TEST_ASSERT_TRUE(parser.getRenderablePriorityAlert(priority));
    TEST_ASSERT_EQUAL_UINT16(34700, priority.frequency);
}

void test_alert_destination_compatibility_keeps_origin_checksum_and_shape_validation() {
    // Valid baseline, then rejected rows must neither replace nor clear it.
    const auto baseline = makeAlertFrame(1, 1, 34700, 0x22, 0x80);
    deliver(baseline, 1, 100);
    queue.consumeParsedFlag();
    const std::vector<std::vector<uint8_t>> rejected{
        // Foreign destination D5, with a valid checksum.
        {0xAA, 0xD5, 0xEA, 0x43, 0x08, 0, 0, 0, 0, 0, 0, 0, 0xB4, 0xAB},
        // Accessory origin E6, with a valid checksum.
        {0xAA, 0xD6, 0xE6, 0x43, 0x08, 0, 0, 0, 0, 0, 0, 0, 0xB1, 0xAB},
        // Corrupt checksum for D6 and D8.
        {0xAA, 0xD6, 0xEA, 0x43, 0x08, 0, 0, 0, 0, 0, 0, 0, 0xB4, 0xAB},
        {0xAA, 0xD8, 0xEA, 0x43, 0x08, 0, 0, 0, 0, 0, 0, 0, 0xB6, 0xAB},
        // Six data bytes instead of seven, with valid checksum and framing.
        {0xAA, 0xD6, 0xEA, 0x43, 0x07, 0, 0, 0, 0, 0, 0, 0xB4, 0xAB},
    };
    uint32_t ingress = 1;
    for (const auto& frame : rejected) {
        ++ingress;
        deliver(frame, ingress, 100 + ingress);
        TEST_ASSERT_FALSE(queue.consumeParsedFlag());
        TEST_ASSERT_EQUAL_UINT32(1, parser.getAlertCount());
        AlertData priority;
        TEST_ASSERT_TRUE(parser.getRenderablePriorityAlert(priority));
        TEST_ASSERT_EQUAL_UINT16(34700, priority.frequency);
    }
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_queue_gap_prevents_cross_cycle_alert_table_publication);
    RUN_TEST(test_spec_busy_broadcast_reaches_request_owner_but_targeted_busy_does_not);
    RUN_TEST(test_older_gen2_targeted_alerts_and_current_broadcasts_reach_the_alert_table);
    RUN_TEST(test_targeted_alerts_do_not_wait_for_firmware_version_discovery);
    RUN_TEST(test_alert_destination_compatibility_keeps_origin_checksum_and_shape_validation);
    return UNITY_END();
}

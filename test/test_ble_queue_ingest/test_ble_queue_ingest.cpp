#include <unity.h>

#include <array>
#include <cstdint>
#include <vector>

#include "../mocks/Arduino.h"
#include "../mocks/ble_client.h"
#include "../mocks/modules/power/power_module.h"
#include "../mocks/packet_parser.h"
#include "../mocks/v1_profiles.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

class DisplayPreviewModule {
  public:
    bool isRunning() const { return running; }
    void cancel() {
        running = false;
        cancelCalls++;
    }

    bool running = false;
    int cancelCalls = 0;
};

#define private public
#include "../../src/modules/ble/ble_queue_module.h"
#undef private
#include "../../src/modules/ble/ble_queue_module.cpp"

namespace {

constexpr uint32_t kSession = 7;
constexpr uint16_t kCharacteristic = 0xB2CE;

BleQueueModule queue;
PacketParser parser;
V1BLEClient client;
V1ProfileManager profiles;
DisplayPreviewModule preview;
PowerModule power;

std::vector<uint8_t> makeFrame(uint8_t packetId, size_t payloadLength, uint8_t fill) {
    TEST_ASSERT_LESS_OR_EQUAL_UINT8(255, payloadLength);
    std::vector<uint8_t> frame;
    frame.reserve(payloadLength + 6);
    frame.push_back(ESP_PACKET_START);
    frame.push_back(0xDA);
    frame.push_back(0xE4);
    frame.push_back(packetId);
    frame.push_back(static_cast<uint8_t>(payloadLength));
    frame.insert(frame.end(), payloadLength, fill);
    frame.push_back(ESP_PACKET_END);
    return frame;
}

void beginQueue(size_t queueDepth = 24) {
    BleQueueModule::Config config;
    config.queueDepth = queueDepth;
    config.rxBufferCap = 1024;
    TEST_ASSERT_TRUE(queue.begin(&client, &parser, &profiles, &preview, &power, config));
    queue.openSession(kSession);
}

void assertParsedPacket(size_t index, const std::vector<uint8_t>& expected) {
    TEST_ASSERT_GREATER_THAN_UINT(index, parser.parsedPackets.size());
    TEST_ASSERT_EQUAL_UINT(expected.size(), parser.parsedPackets[index].size());
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected.data(), parser.parsedPackets[index].data(), expected.size());
}

} // namespace

void setUp() {
    queue.end();
    parser.reset();
    client.reset();
    profiles.reset();
    preview = DisplayPreviewModule{};
    power.reset();
    HealthCounters::reset();
    mockMillis = 0;
    mockMicros = 0;
}

void tearDown() {
    queue.end();
}

void test_five_accepted_full_notifications_survive_staging_capacity() {
    beginQueue(8);
    std::array<std::vector<uint8_t>, 5> frames;
    for (size_t i = 0; i < frames.size(); ++i) {
        frames[i] = makeFrame(static_cast<uint8_t>(0x50 + i), 250, static_cast<uint8_t>(i + 1));
        TEST_ASSERT_EQUAL_UINT(256, frames[i].size());
        TEST_ASSERT_TRUE(queue.tryOnNotify(frames[i].data(), frames[i].size(), kCharacteristic, kSession,
                                          static_cast<uint32_t>(100 + i)));
    }

    queue.process();

    TEST_ASSERT_EQUAL_INT(4, parser.parseCalls);
    TEST_ASSERT_EQUAL_UINT32(1, uxQueueMessagesWaiting(queue.queueHandle_));
    TEST_ASSERT_EQUAL_UINT32(0, HealthCounters::inputDrops());

    queue.process();

    TEST_ASSERT_EQUAL_INT(5, parser.parseCalls);
    TEST_ASSERT_EQUAL_UINT32(0, uxQueueMessagesWaiting(queue.queueHandle_));
    TEST_ASSERT_EQUAL_UINT32(0, HealthCounters::inputDrops());
    for (size_t i = 0; i < frames.size(); ++i) {
        assertParsedPacket(i, frames[i]);
    }
}

void test_partial_frame_across_notifications_is_reassembled_once() {
    beginQueue();
    const std::vector<uint8_t> frame = makeFrame(0x55, 20, 0x31);

    TEST_ASSERT_TRUE(queue.tryOnNotify(frame.data(), 4, kCharacteristic, kSession, 200));
    queue.process();
    TEST_ASSERT_EQUAL_INT(0, parser.parseCalls);

    TEST_ASSERT_TRUE(queue.tryOnNotify(frame.data() + 4, 7, kCharacteristic, kSession, 201));
    queue.process();
    TEST_ASSERT_EQUAL_INT(0, parser.parseCalls);

    TEST_ASSERT_TRUE(queue.tryOnNotify(frame.data() + 11, frame.size() - 11, kCharacteristic, kSession, 202));
    queue.process();

    TEST_ASSERT_EQUAL_INT(1, parser.parseCalls);
    assertParsedPacket(0, frame);
    TEST_ASSERT_EQUAL_UINT32(202, parser.parseTimestamps[0]);
}

void test_multiple_frames_in_one_notification_are_all_parsed_in_order() {
    beginQueue();
    const std::vector<uint8_t> first = makeFrame(0x56, 4, 0x41);
    const std::vector<uint8_t> second = makeFrame(0x57, 9, 0x42);
    std::vector<uint8_t> notification = first;
    notification.insert(notification.end(), second.begin(), second.end());

    TEST_ASSERT_TRUE(queue.tryOnNotify(notification.data(), notification.size(), kCharacteristic, kSession, 300));
    queue.process();

    TEST_ASSERT_EQUAL_INT(2, parser.parseCalls);
    assertParsedPacket(0, first);
    assertParsedPacket(1, second);
}

void test_session_reset_discards_old_queue_and_partial_buffer() {
    beginQueue();
    const std::vector<uint8_t> oldFrame = makeFrame(0x58, 20, 0x51);
    const std::vector<uint8_t> queuedOldFrame = makeFrame(0x59, 8, 0x52);
    const std::vector<uint8_t> newFrame = makeFrame(0x5A, 6, 0x53);

    TEST_ASSERT_TRUE(queue.tryOnNotify(oldFrame.data(), 8, kCharacteristic, kSession, 400));
    queue.process();
    TEST_ASSERT_EQUAL_INT(0, parser.parseCalls);
    TEST_ASSERT_TRUE(queue.tryOnNotify(queuedOldFrame.data(), queuedOldFrame.size(), kCharacteristic, kSession, 401));

    queue.openSession(kSession + 1);

    TEST_ASSERT_FALSE(queue.tryOnNotify(oldFrame.data() + 8, oldFrame.size() - 8, kCharacteristic, kSession, 402));
    TEST_ASSERT_TRUE(queue.tryOnNotify(newFrame.data(), newFrame.size(), kCharacteristic, kSession + 1, 403));
    queue.process();

    TEST_ASSERT_EQUAL_INT(1, parser.parseCalls);
    assertParsedPacket(0, newFrame);
    TEST_ASSERT_EQUAL_UINT32(0, HealthCounters::inputDrops());
}

void test_queue_saturation_counts_only_rejected_admission_and_preserves_head() {
    beginQueue(2);
    const std::vector<uint8_t> first = makeFrame(0x5B, 3, 0x61);
    const std::vector<uint8_t> second = makeFrame(0x5C, 3, 0x62);
    const std::vector<uint8_t> rejected = makeFrame(0x5D, 3, 0x63);

    TEST_ASSERT_TRUE(queue.tryOnNotify(first.data(), first.size(), kCharacteristic, kSession, 500));
    TEST_ASSERT_TRUE(queue.tryOnNotify(second.data(), second.size(), kCharacteristic, kSession, 501));
    TEST_ASSERT_FALSE(queue.tryOnNotify(rejected.data(), rejected.size(), kCharacteristic, kSession, 502));
    TEST_ASSERT_EQUAL_UINT32(1, HealthCounters::inputDrops());

    queue.process();

    TEST_ASSERT_EQUAL_INT(2, parser.parseCalls);
    assertParsedPacket(0, first);
    assertParsedPacket(1, second);
    TEST_ASSERT_EQUAL_UINT32(1, HealthCounters::inputDrops());

    std::array<uint8_t, 257> oversized{};
    TEST_ASSERT_FALSE(queue.tryOnNotify(oversized.data(), oversized.size(), kCharacteristic, kSession, 503));
    TEST_ASSERT_FALSE(queue.tryOnNotify(first.data(), first.size(), kCharacteristic, kSession + 1, 504));
    TEST_ASSERT_EQUAL_UINT32(1, HealthCounters::inputDrops());
}

void test_malformed_input_resynchronizes_to_following_valid_frame() {
    beginQueue();
    std::vector<uint8_t> malformed = makeFrame(0x5E, 2, 0x71);
    malformed.back() = 0x00;
    const std::vector<uint8_t> valid = makeFrame(0x5F, 5, 0x72);
    std::vector<uint8_t> notification{0x01, 0x02, 0x03};
    notification.insert(notification.end(), malformed.begin(), malformed.end());
    notification.insert(notification.end(), valid.begin(), valid.end());

    TEST_ASSERT_TRUE(queue.tryOnNotify(notification.data(), notification.size(), kCharacteristic, kSession, 600));
    queue.process();

    TEST_ASSERT_EQUAL_INT(1, parser.parseCalls);
    assertParsedPacket(0, valid);
    TEST_ASSERT_EQUAL_UINT32(0, HealthCounters::inputDrops());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_five_accepted_full_notifications_survive_staging_capacity);
    RUN_TEST(test_partial_frame_across_notifications_is_reassembled_once);
    RUN_TEST(test_multiple_frames_in_one_notification_are_all_parsed_in_order);
    RUN_TEST(test_session_reset_discards_old_queue_and_partial_buffer);
    RUN_TEST(test_queue_saturation_counts_only_rejected_admission_and_preserves_head);
    RUN_TEST(test_malformed_input_resynchronizes_to_following_valid_frame);
    return UNITY_END();
}

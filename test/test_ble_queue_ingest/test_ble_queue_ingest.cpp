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
constexpr uint16_t kLongCharacteristic = 0xB4E0;

BleQueueModule queue;
PacketParser parser;
V1BLEClient client;
V1ProfileManager profiles;
DisplayPreviewModule preview;
PowerModule power;

std::vector<uint8_t> makeFrame(uint8_t packetId, size_t payloadLength, uint8_t fill,
                               uint8_t encodedOriginator = 0xEA) {
    TEST_ASSERT_LESS_OR_EQUAL_UINT8(255, payloadLength);
    std::vector<uint8_t> frame;
    frame.reserve(payloadLength + 6);
    frame.push_back(ESP_PACKET_START);
    frame.push_back(0xDA);
    frame.push_back(encodedOriginator);
    frame.push_back(packetId);
    frame.push_back(static_cast<uint8_t>(payloadLength));
    frame.insert(frame.end(), payloadLength, fill);
    frame.push_back(ESP_PACKET_END);
    return frame;
}

std::vector<std::vector<uint8_t>> makeLongChunks(const std::vector<uint8_t>& frame) {
    constexpr size_t kChunkPayload = 19;
    const size_t count = (frame.size() + kChunkPayload - 1u) / kChunkPayload;
    TEST_ASSERT_GREATER_THAN_UINT(0, count);
    TEST_ASSERT_LESS_OR_EQUAL_UINT(15, count);

    std::vector<std::vector<uint8_t>> chunks;
    chunks.reserve(count);
    for (size_t index = 0; index < count; ++index) {
        const size_t begin = index * kChunkPayload;
        const size_t length = std::min(kChunkPayload, frame.size() - begin);
        std::vector<uint8_t> chunk;
        chunk.reserve(length + 1u);
        chunk.push_back(static_cast<uint8_t>(((index + 1u) << 4) | count));
        chunk.insert(chunk.end(), frame.begin() + begin, frame.begin() + begin + length);
        chunks.push_back(std::move(chunk));
    }
    return chunks;
}

std::vector<uint8_t> makeCanonicalUserBytesFrame(uint8_t fill, uint8_t encodedOriginator = 0xEA,
                                                 uint8_t destination = 0xD6,
                                                 bool corruptChecksum = false) {
    const size_t dataLength = 6;
    const size_t payloadLength = dataLength + (encodedOriginator == 0xEA ? 1 : 0);
    std::vector<uint8_t> frame{ESP_PACKET_START, destination, encodedOriginator,
                               PACKET_ID_RESP_USER_BYTES, static_cast<uint8_t>(payloadLength)};
    frame.insert(frame.end(), dataLength, fill);
    if (encodedOriginator == 0xEA) {
        uint8_t checksum = 0;
        for (uint8_t value : frame) checksum = static_cast<uint8_t>(checksum + value);
        frame.push_back(corruptChecksum ? static_cast<uint8_t>(checksum ^ 0x01) : checksum);
    }
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

bool deliverRawNotify(const uint8_t* data, size_t length, uint16_t charUuid,
                      uint32_t sessionGeneration, uint32_t callbackMillis,
                      uint32_t ingressSequence = 0) {
    if (ingressSequence == 0) ingressSequence = client.noteV1NotificationIngress();
    return queue.tryOnNotify(data, length, charUuid, sessionGeneration, callbackMillis,
                             ingressSequence);
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
        TEST_ASSERT_TRUE(deliverRawNotify(frames[i].data(), frames[i].size(), kCharacteristic, kSession,
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

    TEST_ASSERT_TRUE(deliverRawNotify(frame.data(), 4, kCharacteristic, kSession, 200));
    queue.process();
    TEST_ASSERT_EQUAL_INT(0, parser.parseCalls);
    const uint32_t commandBoundary = client.latestV1NotificationIngressSequence();

    TEST_ASSERT_TRUE(deliverRawNotify(frame.data() + 4, 7, kCharacteristic, kSession, 201));
    queue.process();
    TEST_ASSERT_EQUAL_INT(0, parser.parseCalls);

    TEST_ASSERT_TRUE(deliverRawNotify(frame.data() + 11, frame.size() - 11, kCharacteristic, kSession, 202));
    queue.process();

    TEST_ASSERT_EQUAL_INT(1, parser.parseCalls);
    TEST_ASSERT_EQUAL_INT(0, parser.markAlertStreamDiscontinuousCalls);
    assertParsedPacket(0, frame);
    TEST_ASSERT_EQUAL_UINT32(202, parser.parseTimestamps[0]);
    // The frame began before the modeled command boundary even though its
    // final bytes arrived afterward. Its start-byte provenance must remain
    // pre-command so it cannot verify that command.
    TEST_ASSERT_EQUAL_UINT32(commandBoundary, parser.parseIngressSequences[0]);
}

void test_multiple_frames_in_one_notification_are_all_parsed_in_order() {
    beginQueue();
    const std::vector<uint8_t> first = makeFrame(0x56, 4, 0x41);
    const std::vector<uint8_t> second = makeFrame(0x57, 9, 0x42);
    std::vector<uint8_t> notification = first;
    notification.insert(notification.end(), second.begin(), second.end());

    TEST_ASSERT_TRUE(deliverRawNotify(notification.data(), notification.size(), kCharacteristic, kSession, 300));
    queue.process();

    TEST_ASSERT_EQUAL_INT(2, parser.parseCalls);
    TEST_ASSERT_EQUAL_INT(0, parser.markAlertStreamDiscontinuousCalls);
    assertParsedPacket(0, first);
    assertParsedPacket(1, second);
}

void test_long_characteristic_reassembles_out_of_order_chunks_once() {
    beginQueue();
    const std::vector<uint8_t> frame = makeFrame(PACKET_ID_RESP_SWEEP_SECTIONS, 16, 0x61);
    TEST_ASSERT_EQUAL_UINT(22, frame.size());
    const auto chunks = makeLongChunks(frame);
    TEST_ASSERT_EQUAL_UINT(2, chunks.size());

    const uint32_t firstIngress = client.noteV1NotificationIngress();
    TEST_ASSERT_TRUE(deliverRawNotify(chunks[1].data(), chunks[1].size(), kLongCharacteristic,
                                      kSession, 310, firstIngress));
    queue.process();
    TEST_ASSERT_EQUAL_INT(0, parser.parseCalls);

    const uint32_t secondIngress = client.noteV1NotificationIngress();
    TEST_ASSERT_TRUE(deliverRawNotify(chunks[0].data(), chunks[0].size(), kLongCharacteristic,
                                      kSession, 311, secondIngress));
    queue.process();

    TEST_ASSERT_EQUAL_INT(1, parser.parseCalls);
    assertParsedPacket(0, frame);
    TEST_ASSERT_EQUAL_UINT32(311, parser.parseTimestamps[0]);
    TEST_ASSERT_EQUAL_UINT32(firstIngress, parser.parseIngressSequences[0]);
}

void test_long_characteristic_never_splices_into_partial_short_frame() {
    beginQueue();
    const std::vector<uint8_t> shortFrame = makeFrame(0x58, 12, 0x41);
    const std::vector<uint8_t> longFrame = makeFrame(PACKET_ID_RESP_SWEEP_SECTIONS, 16, 0x62);
    const auto chunks = makeLongChunks(longFrame);

    TEST_ASSERT_TRUE(deliverRawNotify(shortFrame.data(), 4, kCharacteristic, kSession, 320));
    queue.process();
    TEST_ASSERT_EQUAL_INT(0, parser.parseCalls);

    TEST_ASSERT_TRUE(deliverRawNotify(chunks[0].data(), chunks[0].size(), kLongCharacteristic, kSession, 321));
    TEST_ASSERT_TRUE(deliverRawNotify(chunks[1].data(), chunks[1].size(), kLongCharacteristic, kSession, 322));
    TEST_ASSERT_TRUE(deliverRawNotify(shortFrame.data() + 4, shortFrame.size() - 4,
                                      kCharacteristic, kSession, 323));
    queue.process();

    TEST_ASSERT_EQUAL_INT(1, parser.parseCalls);
    assertParsedPacket(0, shortFrame);

    // The complete B4E0 candidate remains independently owned until the older
    // B2CE stream is consumed, then publishes on the following main-loop tick.
    queue.process();
    TEST_ASSERT_EQUAL_INT(2, parser.parseCalls);
    assertParsedPacket(1, longFrame);
}

void test_session_reset_discards_partial_long_packet() {
    beginQueue();
    const std::vector<uint8_t> oldFrame = makeFrame(PACKET_ID_RESP_SWEEP_SECTIONS, 16, 0x63);
    const auto oldChunks = makeLongChunks(oldFrame);

    TEST_ASSERT_TRUE(deliverRawNotify(oldChunks[0].data(), oldChunks[0].size(), kLongCharacteristic,
                                      kSession, 330));
    queue.process();
    TEST_ASSERT_EQUAL_INT(0, parser.parseCalls);

    queue.openSession(kSession + 1);
    TEST_ASSERT_FALSE(deliverRawNotify(oldChunks[1].data(), oldChunks[1].size(), kLongCharacteristic,
                                       kSession, 331));
    queue.process();
    TEST_ASSERT_EQUAL_INT(0, parser.parseCalls);

    const std::vector<uint8_t> newFrame = makeFrame(PACKET_ID_RESP_SWEEP_SECTIONS, 16, 0x64);
    const auto newChunks = makeLongChunks(newFrame);
    TEST_ASSERT_TRUE(deliverRawNotify(newChunks[0].data(), newChunks[0].size(), kLongCharacteristic,
                                      kSession + 1, 332));
    TEST_ASSERT_TRUE(deliverRawNotify(newChunks[1].data(), newChunks[1].size(), kLongCharacteristic,
                                      kSession + 1, 333));
    queue.process();

    TEST_ASSERT_EQUAL_INT(1, parser.parseCalls);
    assertParsedPacket(0, newFrame);
}

void test_invalid_long_chunk_is_dropped_without_poisoning_next_packet() {
    beginQueue();
    const uint8_t invalid[] = {0x02, 0xAA}; // index zero is outside the documented 1..count range
    TEST_ASSERT_TRUE(deliverRawNotify(invalid, sizeof(invalid), kLongCharacteristic, kSession, 340));

    const std::vector<uint8_t> frame = makeFrame(PACKET_ID_RESP_SWEEP_SECTIONS, 16, 0x65);
    const auto chunks = makeLongChunks(frame);
    TEST_ASSERT_TRUE(deliverRawNotify(chunks[0].data(), chunks[0].size(), kLongCharacteristic, kSession, 341));
    TEST_ASSERT_TRUE(deliverRawNotify(chunks[1].data(), chunks[1].size(), kLongCharacteristic, kSession, 342));
    queue.process();

    TEST_ASSERT_EQUAL_INT(1, parser.parseCalls);
    assertParsedPacket(0, frame);
}

void test_duplicate_long_index_restarts_candidate_without_cross_packet_splice() {
    beginQueue();
    const std::vector<uint8_t> oldFrame = makeFrame(PACKET_ID_RESP_SWEEP_SECTIONS, 16, 0x66);
    const std::vector<uint8_t> newFrame = makeFrame(PACKET_ID_RESP_SWEEP_SECTIONS, 16, 0x67);
    const auto oldChunks = makeLongChunks(oldFrame);
    const auto newChunks = makeLongChunks(newFrame);

    TEST_ASSERT_TRUE(deliverRawNotify(oldChunks[0].data(), oldChunks[0].size(),
                                      kLongCharacteristic, kSession, 350));
    TEST_ASSERT_TRUE(deliverRawNotify(newChunks[0].data(), newChunks[0].size(),
                                      kLongCharacteristic, kSession, 351));
    TEST_ASSERT_TRUE(deliverRawNotify(newChunks[1].data(), newChunks[1].size(),
                                      kLongCharacteristic, kSession, 352));
    queue.process();

    TEST_ASSERT_EQUAL_INT(1, parser.parseCalls);
    assertParsedPacket(0, newFrame);
}

void test_session_reset_discards_old_queue_and_partial_buffer() {
    beginQueue();
    const std::vector<uint8_t> oldFrame = makeFrame(0x58, 20, 0x51);
    const std::vector<uint8_t> queuedOldFrame = makeFrame(0x59, 8, 0x52);
    const std::vector<uint8_t> newFrame = makeFrame(0x5A, 6, 0x53);

    TEST_ASSERT_TRUE(deliverRawNotify(oldFrame.data(), 8, kCharacteristic, kSession, 400));
    queue.process();
    TEST_ASSERT_EQUAL_INT(0, parser.parseCalls);
    TEST_ASSERT_TRUE(deliverRawNotify(queuedOldFrame.data(), queuedOldFrame.size(), kCharacteristic, kSession, 401));

    queue.openSession(kSession + 1);

    TEST_ASSERT_FALSE(deliverRawNotify(oldFrame.data() + 8, oldFrame.size() - 8, kCharacteristic, kSession, 402));
    TEST_ASSERT_TRUE(deliverRawNotify(newFrame.data(), newFrame.size(), kCharacteristic, kSession + 1, 403));
    queue.process();

    TEST_ASSERT_EQUAL_INT(1, parser.parseCalls);
    assertParsedPacket(0, newFrame);
    TEST_ASSERT_EQUAL_UINT32(0, HealthCounters::inputDrops());
}

void test_stale_stamped_packets_cannot_trigger_downstream_effects() {
    beginQueue();
    queue.openSession(kSession + 1);
    preview.running = true;
    parser.hasAlertsFlag = true;
    parser.state.hasV1Version = true;
    parser.state.v1FirmwareVersion = 0x12345678;

    const std::vector<uint8_t> staleVersion = makeFrame(PACKET_ID_RESP_VERSION, 6, 0x41);
    const std::vector<uint8_t> staleUserBytes = makeFrame(PACKET_ID_RESP_USER_BYTES, 6, 0x42);
    TEST_ASSERT_TRUE(
        queue.enqueueStampedForTest(staleVersion.data(), staleVersion.size(), kCharacteristic, kSession));
    TEST_ASSERT_TRUE(
        queue.enqueueStampedForTest(staleUserBytes.data(), staleUserBytes.size(), kCharacteristic, kSession));

    queue.process();

    TEST_ASSERT_EQUAL_INT(0, parser.parseCalls);
    TEST_ASSERT_EQUAL_INT(0, profiles.setCurrentSettingsCalls);
    TEST_ASSERT_EQUAL_INT(0, client.onUserBytesReceivedCalls);
    TEST_ASSERT_EQUAL_UINT32(0, client.v1FirmwareVersion());
    TEST_ASSERT_EQUAL_INT(0, power.onV1DataReceivedCalls);
    TEST_ASSERT_EQUAL_INT(0, preview.cancelCalls);
    TEST_ASSERT_TRUE(preview.running);
    TEST_ASSERT_FALSE(queue.consumeParsedFlag());
}

void test_truncated_user_bytes_response_cannot_complete_capture() {
    beginQueue();
    client.beginSessionUserBytesCapture(client.latestV1NotificationIngressSequence());
    // PL=6 is five settings bytes plus checksum. The old >=12-byte check
    // copied the checksum into settings byte six and completed the capture.
    const std::vector<uint8_t> truncated = makeFrame(PACKET_ID_RESP_USER_BYTES, 6, 0x42);
    TEST_ASSERT_EQUAL_UINT(12, truncated.size());
    TEST_ASSERT_TRUE(deliverRawNotify(truncated.data(), truncated.size(), kCharacteristic, kSession, 425));
    queue.process();

    TEST_ASSERT_EQUAL_INT(0, client.onUserBytesReceivedCalls);
    TEST_ASSERT_EQUAL_INT(0, profiles.setCurrentSettingsCalls);
    TEST_ASSERT_FALSE(client.hasSessionUserBytes());

    const std::vector<uint8_t> canonical = makeCanonicalUserBytesFrame(0x43);
    TEST_ASSERT_EQUAL_UINT(13, canonical.size());
    TEST_ASSERT_TRUE(deliverRawNotify(canonical.data(), canonical.size(), kCharacteristic, kSession, 426));
    queue.process();
    TEST_ASSERT_EQUAL_INT(1, client.onUserBytesReceivedCalls);
    TEST_ASSERT_EQUAL_INT(1, profiles.setCurrentSettingsCalls);
    TEST_ASSERT_TRUE(client.hasSessionUserBytes());

    // The same PL=6 is complete only when the packet identifies a
    // no-checksum V1 (E9h): all six payload bytes are settings bytes.
    const std::vector<uint8_t> noChecksum = makeCanonicalUserBytesFrame(0x44, 0xE9);
    TEST_ASSERT_TRUE(deliverRawNotify(noChecksum.data(), noChecksum.size(), kCharacteristic, kSession, 427));
    queue.process();
    TEST_ASSERT_EQUAL_INT(2, client.onUserBytesReceivedCalls);
    TEST_ASSERT_EQUAL_INT(2, profiles.setCurrentSettingsCalls);

    const std::vector<uint8_t> wrongDestination = makeCanonicalUserBytesFrame(0x45, 0xEA, 0xDA);
    TEST_ASSERT_TRUE(deliverRawNotify(wrongDestination.data(), wrongDestination.size(), kCharacteristic,
                                      kSession, 428));
    queue.process();
    TEST_ASSERT_EQUAL_INT(2, client.onUserBytesReceivedCalls);
    TEST_ASSERT_EQUAL_INT(2, profiles.setCurrentSettingsCalls);

    const std::vector<uint8_t> corruptChecksum = makeCanonicalUserBytesFrame(0x46, 0xEA, 0xD6, true);
    TEST_ASSERT_TRUE(deliverRawNotify(corruptChecksum.data(), corruptChecksum.size(), kCharacteristic,
                                      kSession, 429));
    queue.process();
    TEST_ASSERT_EQUAL_INT(2, client.onUserBytesReceivedCalls);
    TEST_ASSERT_EQUAL_INT(2, profiles.setCurrentSettingsCalls);
}

void test_rejected_all_volume_response_cannot_complete_capture() {
    beginQueue();
    // Model the production parser rejecting a canonical-width response whose
    // first full-byte volume value is out of the 0..9 protocol range. Preserve
    // a prior valid parser observation to prove parse failure gates completion.
    parser.parseReturnValue = false;
    parser.state.hasSavedVolume = true;
    const std::vector<uint8_t> malformed = makeFrame(PACKET_ID_RESP_ALL_VOLUME, 5, 0x17);

    TEST_ASSERT_TRUE(deliverRawNotify(malformed.data(), malformed.size(), kCharacteristic, kSession, 428));
    queue.process();

    TEST_ASSERT_EQUAL_INT(1, parser.parseCalls);
    TEST_ASSERT_EQUAL_INT(0, client.onAllVolumeReceivedCalls);
    TEST_ASSERT_FALSE(client.hasSessionAllVolume());
}

void test_only_successfully_parsed_alert_packets_trigger_runtime_effects() {
    beginQueue();
    preview.running = true;
    parser.hasAlertsFlag = true;
    const std::vector<uint8_t> accepted = makeFrame(0x60, 4, 0x51);

    TEST_ASSERT_TRUE(deliverRawNotify(accepted.data(), accepted.size(), kCharacteristic, kSession, 450));
    queue.process();

    TEST_ASSERT_EQUAL_INT(1, parser.parseCalls);
    TEST_ASSERT_EQUAL_INT(1, power.onV1DataReceivedCalls);
    TEST_ASSERT_EQUAL_INT(1, preview.cancelCalls);
    TEST_ASSERT_FALSE(preview.running);
    TEST_ASSERT_TRUE(queue.consumeParsedFlag());

    parser.parseReturnValue = false;
    preview.running = true;
    const std::vector<uint8_t> rejected = makeFrame(0x61, 4, 0x52);
    TEST_ASSERT_TRUE(deliverRawNotify(rejected.data(), rejected.size(), kCharacteristic, kSession, 451));
    queue.process();

    TEST_ASSERT_EQUAL_INT(2, parser.parseCalls);
    TEST_ASSERT_EQUAL_INT(1, power.onV1DataReceivedCalls);
    TEST_ASSERT_EQUAL_INT(1, preview.cancelCalls);
    TEST_ASSERT_TRUE(preview.running);
    TEST_ASSERT_FALSE(queue.consumeParsedFlag());
}

void test_only_canonical_display_parses_update_time_slice_flow_control() {
    beginQueue();
    parser.state.timeSliceHoldoff = true;
    const std::vector<uint8_t> display = makeFrame(PACKET_ID_DISPLAY_DATA, 8, 0x00);

    TEST_ASSERT_TRUE(deliverRawNotify(display.data(), display.size(), kCharacteristic,
                                      kSession, 500));
    queue.process();
    TEST_ASSERT_EQUAL_INT(1, client.onV1DisplayFlowControlCalls);
    TEST_ASSERT_TRUE(client.lastTimeSliceHoldoff);

    parser.parseReturnValue = false;
    parser.state.timeSliceHoldoff = false;
    TEST_ASSERT_TRUE(deliverRawNotify(display.data(), display.size(), kCharacteristic,
                                      kSession, 501));
    queue.process();
    TEST_ASSERT_EQUAL_INT(1, client.onV1DisplayFlowControlCalls);
    TEST_ASSERT_TRUE(client.lastTimeSliceHoldoff);
}

void test_canonical_v1_flow_control_packets_reach_the_session_owner() {
    beginQueue();
    const std::vector<uint8_t> rejected = makeFrame(
        PACKET_ID_RESP_REQUEST_NOT_PROCESSED, 2, PACKET_ID_REQ_MAX_SWEEP_INDEX);
    TEST_ASSERT_TRUE(deliverRawNotify(rejected.data(), rejected.size(), kCharacteristic,
                                      kSession, 510));
    queue.process();
    TEST_ASSERT_EQUAL_INT(1, client.onV1RequestNotProcessedCalls);
    TEST_ASSERT_EQUAL_HEX8(PACKET_ID_REQ_MAX_SWEEP_INDEX,
                           client.lastNotProcessedPacketId);

    std::vector<uint8_t> busy = makeFrame(PACKET_ID_INF_V1_BUSY, 3, 0);
    busy[1] = 0xD8; // infV1Busy is General Broadcast, ESP 3.016 p40.
    busy[5] = PACKET_ID_REQ_MAX_SWEEP_INDEX;
    busy[6] = PACKET_ID_REQ_ALL_SWEEP_DEFINITIONS;
    TEST_ASSERT_TRUE(deliverRawNotify(busy.data(), busy.size(), kCharacteristic,
                                      kSession, 511));
    queue.process();
    TEST_ASSERT_EQUAL_INT(1, client.onV1BusyCalls);
    TEST_ASSERT_EQUAL_UINT(2, client.lastBusyPacketIds.size());
    TEST_ASSERT_EQUAL_HEX8(PACKET_ID_REQ_MAX_SWEEP_INDEX,
                           client.lastBusyPacketIds[0]);
    TEST_ASSERT_EQUAL_HEX8(PACKET_ID_REQ_ALL_SWEEP_DEFINITIONS,
                           client.lastBusyPacketIds[1]);

    parser.parseReturnValue = false;
    TEST_ASSERT_TRUE(deliverRawNotify(rejected.data(), rejected.size(), kCharacteristic,
                                      kSession, 512));
    queue.process();
    TEST_ASSERT_EQUAL_INT(1, client.onV1RequestNotProcessedCalls);
}

void test_queue_saturation_counts_only_rejected_admission_and_preserves_head() {
    beginQueue(2);
    const std::vector<uint8_t> first = makeFrame(0x5B, 3, 0x61);
    const std::vector<uint8_t> second = makeFrame(0x5C, 3, 0x62);
    const std::vector<uint8_t> rejected = makeFrame(0x5D, 3, 0x63);

    TEST_ASSERT_TRUE(deliverRawNotify(first.data(), first.size(), kCharacteristic, kSession, 500));
    TEST_ASSERT_TRUE(deliverRawNotify(second.data(), second.size(), kCharacteristic, kSession, 501));
    TEST_ASSERT_FALSE(deliverRawNotify(rejected.data(), rejected.size(), kCharacteristic, kSession, 502));
    TEST_ASSERT_EQUAL_UINT32(1, HealthCounters::inputDrops());

    queue.process();

    TEST_ASSERT_EQUAL_INT(2, parser.parseCalls);
    assertParsedPacket(0, first);
    assertParsedPacket(1, second);
    TEST_ASSERT_EQUAL_UINT32(1, HealthCounters::inputDrops());

    // The next admitted notification exposes the rejected sequence at the
    // exact stream boundary before its frame is parsed.
    const std::vector<uint8_t> afterDrop = makeFrame(0x5E, 3, 0x64);
    TEST_ASSERT_TRUE(deliverRawNotify(afterDrop.data(), afterDrop.size(), kCharacteristic, kSession, 502));
    queue.process();
    TEST_ASSERT_EQUAL_INT(3, parser.parseCalls);
    TEST_ASSERT_EQUAL_INT(1, parser.markAlertStreamDiscontinuousCalls);

    std::array<uint8_t, 257> oversized{};
    TEST_ASSERT_FALSE(deliverRawNotify(oversized.data(), oversized.size(), kCharacteristic, kSession, 503));
    TEST_ASSERT_FALSE(deliverRawNotify(first.data(), first.size(), kCharacteristic, kSession + 1, 504));
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

    TEST_ASSERT_TRUE(deliverRawNotify(notification.data(), notification.size(), kCharacteristic, kSession, 600));
    queue.process();

    TEST_ASSERT_EQUAL_INT(1, parser.parseCalls);
    assertParsedPacket(0, valid);
    TEST_ASSERT_TRUE(parser.markAlertStreamDiscontinuousCalls >= 1);
    TEST_ASSERT_EQUAL_UINT32(0, HealthCounters::inputDrops());
}

void test_notification_sequence_gap_marks_before_later_frame() {
    beginQueue();
    const std::vector<uint8_t> first = makeFrame(0x50, 3, 0x11);
    const std::vector<uint8_t> later = makeFrame(0x51, 3, 0x22);

    TEST_ASSERT_TRUE(deliverRawNotify(first.data(), first.size(), kCharacteristic, kSession, 610, 10));
    TEST_ASSERT_TRUE(deliverRawNotify(later.data(), later.size(), kCharacteristic, kSession, 611, 12));
    queue.process();

    TEST_ASSERT_EQUAL_INT(2, parser.parseCalls);
    TEST_ASSERT_EQUAL_INT(1, parser.markAlertStreamDiscontinuousCalls);
}

void test_notification_sequence_rollover_is_contiguous() {
    beginQueue();
    const std::vector<uint8_t> beforeWrap = makeFrame(0x50, 3, 0x11);
    const std::vector<uint8_t> afterWrap = makeFrame(0x51, 3, 0x22);

    TEST_ASSERT_TRUE(deliverRawNotify(beforeWrap.data(), beforeWrap.size(), kCharacteristic,
                                     kSession, 620, UINT32_MAX));
    TEST_ASSERT_TRUE(deliverRawNotify(afterWrap.data(), afterWrap.size(), kCharacteristic,
                                     kSession, 621, 1));
    queue.process();

    TEST_ASSERT_EQUAL_INT(2, parser.parseCalls);
    TEST_ASSERT_EQUAL_INT(0, parser.markAlertStreamDiscontinuousCalls);
}

void test_session_reset_clears_notification_continuity_baseline() {
    beginQueue();
    const std::vector<uint8_t> oldSession = makeFrame(0x50, 3, 0x11);
    const std::vector<uint8_t> newSession = makeFrame(0x51, 3, 0x22);
    TEST_ASSERT_TRUE(deliverRawNotify(oldSession.data(), oldSession.size(), kCharacteristic,
                                     kSession, 630, 10));
    queue.process();

    queue.openSession(kSession + 1);
    TEST_ASSERT_TRUE(deliverRawNotify(newSession.data(), newSession.size(), kCharacteristic,
                                     kSession + 1, 631, 50));
    queue.process();

    TEST_ASSERT_EQUAL_INT(2, parser.parseCalls);
    TEST_ASSERT_EQUAL_INT(0, parser.markAlertStreamDiscontinuousCalls);
}

void test_rejected_alert_frame_marks_stream_discontinuous() {
    beginQueue();
    parser.parseReturnValue = false;
    const std::vector<uint8_t> rejectedAlert = makeFrame(PACKET_ID_ALERT_DATA, 7, 0x11);
    TEST_ASSERT_TRUE(deliverRawNotify(rejectedAlert.data(), rejectedAlert.size(), kCharacteristic,
                                     kSession, 640));
    queue.process();

    TEST_ASSERT_EQUAL_INT(1, parser.parseCalls);
    TEST_ASSERT_EQUAL_INT(1, parser.markAlertStreamDiscontinuousCalls);
}

void test_parser_packet_queued_beyond_first_drain_retains_pre_command_ingress() {
    beginQueue();
    preview.running = true; // cap this cycle at sixteen parsed packets
    for (uint8_t i = 0; i < 16; ++i) {
        const std::vector<uint8_t> frame = makeFrame(static_cast<uint8_t>(0x50 + i), 3, i);
        TEST_ASSERT_TRUE(deliverRawNotify(frame.data(), frame.size(), kCharacteristic, kSession,
                                          static_cast<uint32_t>(700 + i)));
    }
    const std::vector<uint8_t> queuedDisplay = makeFrame(PACKET_ID_DISPLAY_DATA, 8, 0x00);
    // Model raw notifyCallback entry before a command, then delay delivery
    // through DriveRuntime into BleQueue until after that command boundary.
    const uint32_t queuedDisplayEntry = client.noteV1NotificationIngress();
    const uint32_t commandBoundary = client.latestV1NotificationIngressSequence();
    TEST_ASSERT_TRUE(deliverRawNotify(queuedDisplay.data(), queuedDisplay.size(), kCharacteristic,
                                     kSession, 720, queuedDisplayEntry));

    queue.process();
    TEST_ASSERT_EQUAL_INT(16, parser.parseCalls);

    queue.process();
    TEST_ASSERT_EQUAL_INT(17, parser.parseCalls);
    assertParsedPacket(16, queuedDisplay);
    TEST_ASSERT_EQUAL_UINT32(commandBoundary, parser.parseIngressSequences[16]);
}

void test_user_response_queued_beyond_first_drain_retains_pre_request_ingress() {
    beginQueue();
    preview.running = true;
    for (uint8_t i = 0; i < 16; ++i) {
        const std::vector<uint8_t> frame = makeFrame(static_cast<uint8_t>(0x50 + i), 3, i);
        TEST_ASSERT_TRUE(deliverRawNotify(frame.data(), frame.size(), kCharacteristic, kSession,
                                          static_cast<uint32_t>(800 + i)));
    }
    const std::vector<uint8_t> queuedUser = makeCanonicalUserBytesFrame(0x4A);
    const uint32_t queuedUserEntry = client.noteV1NotificationIngress();
    const uint32_t requestBoundary = client.latestV1NotificationIngressSequence();
    client.beginSessionUserBytesCapture(requestBoundary);
    TEST_ASSERT_TRUE(deliverRawNotify(queuedUser.data(), queuedUser.size(), kCharacteristic,
                                     kSession, 820, queuedUserEntry));

    queue.process();
    TEST_ASSERT_EQUAL_INT(16, parser.parseCalls);
    TEST_ASSERT_EQUAL_INT(0, client.onUserBytesReceivedCalls);

    queue.process();
    TEST_ASSERT_EQUAL_INT(1, client.onUserBytesReceivedCalls);
    TEST_ASSERT_FALSE(client.hasSessionUserBytes());
    TEST_ASSERT_EQUAL_UINT32(0u, client.sessionUserBytesIngressSequence());

    const std::vector<uint8_t> freshUser = makeCanonicalUserBytesFrame(0x4B);
    TEST_ASSERT_TRUE(deliverRawNotify(freshUser.data(), freshUser.size(), kCharacteristic,
                                     kSession, 821));
    queue.process();
    TEST_ASSERT_TRUE(client.hasSessionUserBytes());
    TEST_ASSERT_TRUE(client.sessionUserBytesIngressSequence() > requestBoundary);
}

void configureSyntheticSweepResponses() {
    parser.synthesizeSweepResponses = true;
    parser.synthesizedSweepSections.available = true;
    parser.synthesizedSweepSections.complete = true;
    parser.synthesizedSweepSections.count = 2;
    parser.synthesizedSweepSections.presentMask = 0x03;
    parser.synthesizedSweepMax.available = true;
    parser.synthesizedSweepMax.maxIndex = 1;
    parser.synthesizedSweepDefinitions.presentMask = 0x03;
}

void test_sweep_capture_completion_is_response_order_independent() {
    beginQueue();
    configureSyntheticSweepResponses();
    client.beginSessionSweepSectionsCapture(10);
    client.beginSessionSweepMaxCapture(20);
    client.beginSessionSweepDefinitionsCapture(30);
    const auto sections = makeFrame(PACKET_ID_RESP_SWEEP_SECTIONS, 1, 0);
    const auto maxIndex = makeFrame(PACKET_ID_RESP_MAX_SWEEP_INDEX, 1, 0);
    const auto definitions = makeFrame(PACKET_ID_RESP_SWEEP_DEFINITION, 1, 0);

    TEST_ASSERT_TRUE(deliverRawNotify(definitions.data(), definitions.size(), kCharacteristic,
                                      kSession, 900, 31));
    queue.process();
    TEST_ASSERT_FALSE(client.sessionSweepDefinitionsCaptured);
    TEST_ASSERT_TRUE(deliverRawNotify(sections.data(), sections.size(), kCharacteristic,
                                      kSession, 901, 32));
    queue.process();
    TEST_ASSERT_TRUE(client.sessionSweepSectionsCaptured);
    TEST_ASSERT_TRUE(deliverRawNotify(maxIndex.data(), maxIndex.size(), kCharacteristic,
                                      kSession, 902, 33));
    queue.process();
    TEST_ASSERT_TRUE(client.sessionSweepMaxCaptured);
    TEST_ASSERT_TRUE(client.sessionSweepDefinitionsCaptured); // max-last recomputes completeness

    setUp();
    beginQueue();
    configureSyntheticSweepResponses();
    client.beginSessionSweepSectionsCapture(40);
    client.beginSessionSweepMaxCapture(40);
    client.beginSessionSweepDefinitionsCapture(40);
    TEST_ASSERT_TRUE(deliverRawNotify(maxIndex.data(), maxIndex.size(), kCharacteristic,
                                      kSession, 903, 41));
    queue.process();
    TEST_ASSERT_TRUE(deliverRawNotify(definitions.data(), definitions.size(), kCharacteristic,
                                      kSession, 904, 42));
    queue.process();
    TEST_ASSERT_TRUE(client.sessionSweepMaxCaptured);
    TEST_ASSERT_TRUE(client.sessionSweepDefinitionsCaptured);
    TEST_ASSERT_FALSE(client.sessionSweepSectionsCaptured);
    TEST_ASSERT_TRUE(deliverRawNotify(sections.data(), sections.size(), kCharacteristic,
                                      kSession, 905, 43));
    queue.process();
    TEST_ASSERT_TRUE(client.sessionSweepSectionsCaptured); // section-last completes independently
}

void test_sweep_response_after_common_boundary_but_before_its_specific_request_is_ineligible() {
    beginQueue();
    configureSyntheticSweepResponses();
    const auto sections = makeFrame(PACKET_ID_RESP_SWEEP_SECTIONS, 1, 0);
    const auto maxIndex = makeFrame(PACKET_ID_RESP_MAX_SWEEP_INDEX, 1, 0);
    const auto definitions = makeFrame(PACKET_ID_RESP_SWEEP_DEFINITION, 1, 0);

    client.beginSessionSweepSectionsCapture(10);
    // This max response entered after the sections boundary but before the
    // max-specific request. Delay its processing until after all requests.
    TEST_ASSERT_TRUE(deliverRawNotify(maxIndex.data(), maxIndex.size(), kCharacteristic,
                                      kSession, 910, 11));
    client.beginSessionSweepMaxCapture(12);
    client.beginSessionSweepDefinitionsCapture(13);
    queue.process();
    TEST_ASSERT_FALSE(client.sessionSweepMaxCaptured);

    TEST_ASSERT_TRUE(deliverRawNotify(sections.data(), sections.size(), kCharacteristic,
                                      kSession, 911, 14));
    TEST_ASSERT_TRUE(deliverRawNotify(definitions.data(), definitions.size(), kCharacteristic,
                                      kSession, 912, 15));
    queue.process();
    TEST_ASSERT_TRUE(client.sessionSweepSectionsCaptured);
    TEST_ASSERT_FALSE(client.sessionSweepMaxCaptured);
    TEST_ASSERT_FALSE(client.sessionSweepDefinitionsCaptured);
}

void test_invalid_or_conflicting_max_response_revokes_capture_until_fresh_request_reset() {
    beginQueue();
    configureSyntheticSweepResponses();
    client.beginSessionSweepMaxCapture(10);
    const auto maxIndex = makeFrame(PACKET_ID_RESP_MAX_SWEEP_INDEX, 1, 0);
    TEST_ASSERT_TRUE(deliverRawNotify(maxIndex.data(), maxIndex.size(), kCharacteristic,
                                      kSession, 920, 11));
    queue.process();
    TEST_ASSERT_TRUE(client.sessionSweepMaxCaptured);

    parser.parseReturnValue = false;
    parser.sweepMaxObservationValue.available = false;
    parser.sweepMaxObservationValue.poisoned = true;
    TEST_ASSERT_TRUE(deliverRawNotify(maxIndex.data(), maxIndex.size(), kCharacteristic,
                                      kSession, 921, 12));
    queue.process();
    TEST_ASSERT_FALSE(client.sessionSweepMaxCaptured);
    TEST_ASSERT_FALSE(client.sessionSweepDefinitionsCaptured);

    parser.parseReturnValue = true;
    parser.synthesizedSweepMax.poisoned = false;
    parser.synthesizedSweepMax.available = true;
    client.beginSessionSweepMaxCapture(12);
    TEST_ASSERT_TRUE(deliverRawNotify(maxIndex.data(), maxIndex.size(), kCharacteristic,
                                      kSession, 922, 13));
    queue.process();
    TEST_ASSERT_TRUE(client.sessionSweepMaxCaptured);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_five_accepted_full_notifications_survive_staging_capacity);
    RUN_TEST(test_partial_frame_across_notifications_is_reassembled_once);
    RUN_TEST(test_multiple_frames_in_one_notification_are_all_parsed_in_order);
    RUN_TEST(test_long_characteristic_reassembles_out_of_order_chunks_once);
    RUN_TEST(test_long_characteristic_never_splices_into_partial_short_frame);
    RUN_TEST(test_session_reset_discards_partial_long_packet);
    RUN_TEST(test_invalid_long_chunk_is_dropped_without_poisoning_next_packet);
    RUN_TEST(test_duplicate_long_index_restarts_candidate_without_cross_packet_splice);
    RUN_TEST(test_session_reset_discards_old_queue_and_partial_buffer);
    RUN_TEST(test_stale_stamped_packets_cannot_trigger_downstream_effects);
    RUN_TEST(test_truncated_user_bytes_response_cannot_complete_capture);
    RUN_TEST(test_rejected_all_volume_response_cannot_complete_capture);
    RUN_TEST(test_only_successfully_parsed_alert_packets_trigger_runtime_effects);
    RUN_TEST(test_only_canonical_display_parses_update_time_slice_flow_control);
    RUN_TEST(test_canonical_v1_flow_control_packets_reach_the_session_owner);
    RUN_TEST(test_queue_saturation_counts_only_rejected_admission_and_preserves_head);
    RUN_TEST(test_malformed_input_resynchronizes_to_following_valid_frame);
    RUN_TEST(test_notification_sequence_gap_marks_before_later_frame);
    RUN_TEST(test_notification_sequence_rollover_is_contiguous);
    RUN_TEST(test_session_reset_clears_notification_continuity_baseline);
    RUN_TEST(test_rejected_alert_frame_marks_stream_discontinuous);
    RUN_TEST(test_parser_packet_queued_beyond_first_drain_retains_pre_command_ingress);
    RUN_TEST(test_user_response_queued_beyond_first_drain_retains_pre_request_ingress);
    RUN_TEST(test_sweep_capture_completion_is_response_order_independent);
    RUN_TEST(test_sweep_response_after_common_boundary_but_before_its_specific_request_is_ineligible);
    RUN_TEST(test_invalid_or_conflicting_max_response_revokes_capture_until_fresh_request_reset);
    return UNITY_END();
}

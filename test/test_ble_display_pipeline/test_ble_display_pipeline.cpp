#include <unity.h>

// Override esp_timer_get_time with mockMicros BEFORE Arduino.h defines its default.
// Forward-declare the variable so the function body can reference it.
extern unsigned long mockMicros;
#define ESP_TIMER_GET_TIME_DEFINED
extern "C" int64_t esp_timer_get_time(void) {
    return static_cast<int64_t>(mockMicros);
}

#include "../mocks/Arduino.h"
#include "../mocks/ble_client.h"
#include "../mocks/display.h"
#include "../mocks/settings.h"
#include "../mocks/v1_profiles.h"
#include "../mocks/modules/display/display_preview_module.h"
#include "../mocks/modules/power/power_module.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

static uint32_t g_millisCallCount = 0;
static unsigned long countedMillis() {
    g_millisCallCount++;
    return mockMillis;
}

struct CountingSerial {
    uint32_t printCount = 0;
    uint32_t printlnCount = 0;
    uint32_t printfCount = 0;

    void reset() {
        printCount = 0;
        printlnCount = 0;
        printfCount = 0;
    }

    uint32_t totalLogs() const {
        return printCount + printlnCount + printfCount;
    }

    void begin(unsigned long) {}
    void print(const char*) { printCount++; }
    void print(int) { printCount++; }
    void print(float, int = 2) { printCount++; }
    void println(const char* = "") { printlnCount++; }
    void println(int) { printlnCount++; }
    void println(float, int = 2) { printlnCount++; }
    void printf(const char*, ...) { printfCount++; }
};

static CountingSerial countedSerial;

#include "../../src/perf_metrics.h"
PerfCounters perfCounters;
PerfExtendedMetrics perfExtended;
void perfRecordBleTimelineEvent(PerfBleTimelineEvent /*event*/, uint32_t /*nowMs*/) {}
void perfRecordV1FirmwareVersion(uint32_t /*version*/) {}  // stub

// Use real parser implementation for BLE->parser integration coverage.
#define millis countedMillis
#define Serial countedSerial
#include "../../src/packet_parser.h"
#include "../../src/packet_parser.cpp"
#include "../../src/packet_parser_alerts.cpp"
#include "../../src/modules/system/system_event_bus.h"
#include "../../src/modules/ble/ble_queue_module.cpp"
#include "../../src/modules/alert_persistence/alert_persistence_module.cpp"
#include "../../src/modules/ble/connection_state_module.cpp"
#undef Serial
#undef millis

#include <vector>

static V1BLEClient ble;
static PacketParser parser;
static V1ProfileManager profiles;
static DisplayPreviewModule preview;
static PowerModule power;
static V1Display display;
static SettingsManager settings;
static AlertPersistenceModule alertPersistence;
static SystemEventBus eventBus;
static BleQueueModule bleQueue;
static ConnectionStateModule connectionState;
static constexpr uint32_t kTestSessionGeneration = 1;

static std::vector<uint8_t> makePacket(uint8_t packetId, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> packet;
    packet.reserve(6 + payload.size());
    packet.push_back(ESP_PACKET_START);
    packet.push_back(0xDA);  // Destination (not validated)
    packet.push_back(0xE4);  // Origin (not validated)
    packet.push_back(packetId);
    packet.push_back(static_cast<uint8_t>(payload.size()));
    packet.insert(packet.end(), payload.begin(), payload.end());
    packet.push_back(ESP_PACKET_END);
    return packet;
}

static std::vector<uint8_t> makeAlertPayload(uint8_t index,
                                             uint8_t count,
                                             uint16_t freqMHz,
                                             uint8_t frontRaw,
                                             uint8_t rearRaw,
                                             uint8_t bandArrow,
                                             uint8_t aux0) {
    const uint8_t indexCount = static_cast<uint8_t>(((index & 0x0F) << 4) | (count & 0x0F));
    return std::vector<uint8_t>{
        indexCount,
        static_cast<uint8_t>((freqMHz >> 8) & 0xFF),
        static_cast<uint8_t>(freqMHz & 0xFF),
        frontRaw,
        rearRaw,
        bandArrow,
        aux0
    };
}

static void appendPacket(std::vector<uint8_t>& out, const std::vector<uint8_t>& packet) {
    out.insert(out.end(), packet.begin(), packet.end());
}

static void appendCorruptMissingEndFrame(std::vector<uint8_t>& out) {
    out.push_back(ESP_PACKET_START);
    out.push_back(0xDA);
    out.push_back(0xE4);
    out.push_back(PACKET_ID_DISPLAY_DATA);
    out.push_back(0x02);
    out.push_back(0x00);
    out.push_back(0x00);
    out.push_back(0x00);  // bad end marker; forces resync warning
}

void setUp() {
    ble.reset();
    parser = PacketParser{};
    profiles.reset();
    preview = DisplayPreviewModule{};
    power.reset();
    display.reset();
    alertPersistence = AlertPersistenceModule{};
    alertPersistence.begin(&ble, &parser, &display, &settings);
    eventBus.reset();
    perfCounters.reset();
    perfExtended.reset();
    g_millisCallCount = 0;
    countedSerial.reset();
    bleQueue.begin(&ble, &parser, &profiles, &preview, &power, &eventBus);
    bleQueue.openSession(kTestSessionGeneration);
    connectionState = ConnectionStateModule{};
    connectionState.begin(&ble, &parser, &display, &power, &bleQueue, &alertPersistence, &eventBus);
}

void tearDown() {
    // Release the mock FreeRTOS queue created by begin(); without this the
    // per-test fixture re-construction orphans one queue per test case and
    // the native-sanitized (LeakSanitizer) lane flags the fixture leak.
    bleQueue.end();
}

void test_ble_queue_parses_display_packet_into_display_state() {
    // payload: bogey='5', source led bars=3 -> local bars=4,
    // K band+front arrow+mute, volume=5/2
    // V1 ESP protocol: last byte of payload region is the checksum.
    const std::vector<uint8_t> payload = {
        109,   // bogey counter byte ('5')
        0x00,  // image2 of bogey counter (unused)
        0x07,  // LED bar bitmap (3 visible source bars)
        0x34,  // image1: K + mute + front arrow
        0x34,  // image2: steady same bits
        0x04,  // aux0: systemStatus=1 (V1 actively searching, required for bands per VR spec)
        0x00,  // aux1
        0x52,  // aux2: main=5, mute=2
        0x00   // checksum (dummy)
    };
    const std::vector<uint8_t> packet = makePacket(PACKET_ID_DISPLAY_DATA, payload);

    mockMillis = 2500;
    mockMicros = 2500 * 1000UL;
    // Mute requires 2 consecutive display packets with bit set.
    bleQueue.onNotify(packet.data(), packet.size(), 0xB2CE, kTestSessionGeneration);
    bleQueue.process();
    bleQueue.onNotify(packet.data(), packet.size(), 0xB2CE, kTestSessionGeneration);
    bleQueue.process();

    const DisplayState& state = parser.getDisplayState();
    TEST_ASSERT_EQUAL_UINT8(BAND_K, state.activeBands);
    TEST_ASSERT_EQUAL_UINT8(DIR_FRONT, state.arrows);
    TEST_ASSERT_EQUAL_UINT8(4, state.signalBars);
    TEST_ASSERT_TRUE(state.muted);
    TEST_ASSERT_EQUAL_UINT8(5, state.mainVolume);
    TEST_ASSERT_EQUAL_UINT8(2, state.muteVolume);

    TEST_ASSERT_TRUE(bleQueue.consumeParsedFlag());
    TEST_ASSERT_EQUAL_UINT32(2500, bleQueue.getLastParsedTimestamp());
    TEST_ASSERT_EQUAL(2, power.onV1DataReceivedCalls);  // 2 packets for mute confirm
}

void test_ble_queue_publishes_parsed_event_to_bus() {
    const std::vector<uint8_t> payload = {
        0x3F, 0x00, 0x01, 0x02, 0x02, 0x00, 0x00, 0x41, 0x00
    };
    const std::vector<uint8_t> packet = makePacket(PACKET_ID_DISPLAY_DATA, payload);

    mockMillis = 1234;
    mockMicros = 1234 * 1000UL;
    bleQueue.onNotify(packet.data(), packet.size(), 0xB2CE, kTestSessionGeneration);
    bleQueue.process();

    SystemEvent event{};
    TEST_ASSERT_TRUE(eventBus.consumeByType(SystemEventType::BLE_FRAME_PARSED, event));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SystemEventType::BLE_FRAME_PARSED),
                            static_cast<uint8_t>(event.type));
    TEST_ASSERT_EQUAL_UINT32(1234, event.tsMs);
    TEST_ASSERT_EQUAL_UINT16(PACKET_ID_DISPLAY_DATA, event.detail);
    TEST_ASSERT_TRUE(event.seq > 0);
}

void test_ble_queue_coalesces_parsed_events_per_process_cycle() {
    const std::vector<uint8_t> payloadA = {
        109, 0x00, 0x07, 0x34, 0x34, 0x00, 0x00, 0x52, 0x00
    };
    const std::vector<uint8_t> payloadB = {
        0x3F, 0x00, 0x01, 0x02, 0x02, 0x00, 0x00, 0x41, 0x00
    };

    const std::vector<uint8_t> packetA = makePacket(PACKET_ID_DISPLAY_DATA, payloadA);
    const std::vector<uint8_t> packetB = makePacket(PACKET_ID_DISPLAY_DATA, payloadB);

    mockMillis = 7777;
    mockMicros = 7777 * 1000UL;
    bleQueue.onNotify(packetA.data(), packetA.size(), 0xB2CE, kTestSessionGeneration);
    bleQueue.onNotify(packetB.data(), packetB.size(), 0xB2CE, kTestSessionGeneration);
    bleQueue.process();

    SystemEvent event{};
    TEST_ASSERT_TRUE(eventBus.consumeByType(SystemEventType::BLE_FRAME_PARSED, event));
    TEST_ASSERT_FALSE(eventBus.consumeByType(SystemEventType::BLE_FRAME_PARSED, event));
    TEST_ASSERT_TRUE(bleQueue.consumeParsedFlag());
}

void test_ble_queue_alert_burst_uses_single_millis_sample() {
    const std::vector<uint8_t> row1 = makePacket(
        PACKET_ID_ALERT_DATA,
        makeAlertPayload(1, 3, 24150, 0x90, 0x00, 0x24, 0x80));
    const std::vector<uint8_t> row2 = makePacket(
        PACKET_ID_ALERT_DATA,
        makeAlertPayload(2, 3, 33800, 0xA0, 0x00, 0x22, 0x00));
    const std::vector<uint8_t> row3 = makePacket(
        PACKET_ID_ALERT_DATA,
        makeAlertPayload(3, 3, 34700, 0xB0, 0x00, 0x22, 0x00));

    std::vector<uint8_t> burst;
    appendPacket(burst, row1);
    appendPacket(burst, row2);
    appendPacket(burst, row3);

    mockMillis = 9000;
    mockMicros = 9000 * 1000UL;
    g_millisCallCount = 0;
    bleQueue.onNotify(burst.data(), burst.size(), 0xB2CE, kTestSessionGeneration);
    bleQueue.process();

    TEST_ASSERT_EQUAL_UINT32(1, g_millisCallCount);
    TEST_ASSERT_EQUAL_UINT32(3, static_cast<uint32_t>(parser.getAlertCount()));
    TEST_ASSERT_TRUE(parser.hasAlerts());
    TEST_ASSERT_EQUAL_UINT32(3, static_cast<uint32_t>(power.onV1DataReceivedCalls));
    TEST_ASSERT_TRUE(bleQueue.consumeParsedFlag());
    TEST_ASSERT_EQUAL_UINT32(9000, bleQueue.getLastParsedTimestamp());

    SystemEvent event{};
    TEST_ASSERT_TRUE(eventBus.consumeByType(SystemEventType::BLE_FRAME_PARSED, event));
    TEST_ASSERT_FALSE(eventBus.consumeByType(SystemEventType::BLE_FRAME_PARSED, event));
    TEST_ASSERT_EQUAL_UINT32(9000, event.tsMs);
    TEST_ASSERT_EQUAL_UINT16(PACKET_ID_ALERT_DATA, event.detail);
}

void test_ble_queue_corrupt_burst_throttles_resync_logs_and_recovers() {
    static constexpr uint32_t kCorruptFrames = 24;
    std::vector<uint8_t> burst;
    burst.reserve(kCorruptFrames * 8 + 32);
    for (uint32_t i = 0; i < kCorruptFrames; ++i) {
        appendCorruptMissingEndFrame(burst);
    }

    const std::vector<uint8_t> payload = {
        109, 0x00, 0x07, 0x34, 0x34, 0x04, 0x00, 0x52, 0x00
    };
    appendPacket(burst, makePacket(PACKET_ID_DISPLAY_DATA, payload));

    mockMillis = 5100;
    mockMicros = 5100 * 1000UL;
    countedSerial.reset();
    bleQueue.onNotify(burst.data(), burst.size(), 0xB2CE, kTestSessionGeneration);
    bleQueue.process();

    TEST_ASSERT_EQUAL_UINT32(1, countedSerial.totalLogs());
    TEST_ASSERT_EQUAL_UINT32(kCorruptFrames,
                             perfCounters.parseResyncs.load(std::memory_order_relaxed));
    TEST_ASSERT_TRUE(bleQueue.consumeParsedFlag());
    TEST_ASSERT_EQUAL_UINT32(5100, bleQueue.getLastParsedTimestamp());
    TEST_ASSERT_EQUAL(1, power.onV1DataReceivedCalls);
}

void test_ble_queue_corrupt_resync_logs_are_time_throttled_across_cycles() {
    std::vector<uint8_t> corrupt;
    appendCorruptMissingEndFrame(corrupt);

    countedSerial.reset();

    mockMillis = 7000;
    mockMicros = 7000 * 1000UL;
    bleQueue.onNotify(corrupt.data(), corrupt.size(), 0xB2CE, kTestSessionGeneration);
    bleQueue.process();
    TEST_ASSERT_EQUAL_UINT32(1, countedSerial.totalLogs());

    mockMillis = 7500;
    mockMicros = 7500 * 1000UL;
    bleQueue.onNotify(corrupt.data(), corrupt.size(), 0xB2CE, kTestSessionGeneration);
    bleQueue.process();
    TEST_ASSERT_EQUAL_UINT32(1, countedSerial.totalLogs());

    mockMillis = 8000;
    mockMicros = 8000 * 1000UL;
    bleQueue.onNotify(corrupt.data(), corrupt.size(), 0xB2CE, kTestSessionGeneration);
    bleQueue.process();
    TEST_ASSERT_EQUAL_UINT32(2, countedSerial.totalLogs());
}

void test_ble_queue_resyncs_after_corrupt_prefix_in_single_notify() {
    const std::vector<uint8_t> payload = {
        109, 0x00, 0x07, 0x34, 0x34, 0x04, 0x00, 0x52, 0x00  // aux0=0x04 (systemStatus on)
    };
    const std::vector<uint8_t> packet = makePacket(PACKET_ID_DISPLAY_DATA, payload);
    std::vector<uint8_t> bytes = {0x00, 0x55, 0x99, 0xAB};
    bytes.insert(bytes.end(), packet.begin(), packet.end());

    mockMillis = 4321;
    mockMicros = 4321 * 1000UL;
    bleQueue.onNotify(bytes.data(), bytes.size(), 0xB2CE, kTestSessionGeneration);
    bleQueue.process();

    const DisplayState& state = parser.getDisplayState();
    TEST_ASSERT_EQUAL_UINT8(BAND_K, state.activeBands);
    TEST_ASSERT_EQUAL_UINT8(DIR_FRONT, state.arrows);
    TEST_ASSERT_EQUAL_UINT8(4, state.signalBars);
    TEST_ASSERT_TRUE(bleQueue.consumeParsedFlag());
    TEST_ASSERT_EQUAL_UINT32(4321, bleQueue.getLastParsedTimestamp());
    TEST_ASSERT_EQUAL(1, power.onV1DataReceivedCalls);

    SystemEvent event{};
    TEST_ASSERT_TRUE(eventBus.consumeByType(SystemEventType::BLE_FRAME_PARSED, event));
    TEST_ASSERT_FALSE(eventBus.consumeByType(SystemEventType::BLE_FRAME_PARSED, event));
    TEST_ASSERT_EQUAL_UINT32(4321, event.tsMs);
    TEST_ASSERT_EQUAL_UINT16(PACKET_ID_DISPLAY_DATA, event.detail);
}

void test_ble_queue_recovers_after_buffer_without_start_marker() {
    const std::vector<uint8_t> payload = {
        0x3F, 0x00, 0x01, 0x02, 0x02, 0x00, 0x00, 0x41, 0x00
    };
    const std::vector<uint8_t> packet = makePacket(PACKET_ID_DISPLAY_DATA, payload);
    const std::vector<uint8_t> garbage = {0x01, 0x02, 0x03, 0x04, 0x05};

    mockMillis = 1500;
    mockMicros = 1500 * 1000UL;
    bleQueue.onNotify(garbage.data(), garbage.size(), 0xB2CE, kTestSessionGeneration);
    bleQueue.process();

    SystemEvent event{};
    TEST_ASSERT_FALSE(bleQueue.consumeParsedFlag());
    TEST_ASSERT_FALSE(eventBus.consumeByType(SystemEventType::BLE_FRAME_PARSED, event));
    TEST_ASSERT_EQUAL_UINT32(0, bleQueue.getLastParsedTimestamp());
    TEST_ASSERT_EQUAL(0, power.onV1DataReceivedCalls);

    mockMillis = 2600;
    mockMicros = 2600 * 1000UL;
    bleQueue.onNotify(packet.data(), packet.size(), 0xB2CE, kTestSessionGeneration);
    bleQueue.process();

    TEST_ASSERT_TRUE(bleQueue.consumeParsedFlag());
    TEST_ASSERT_EQUAL_UINT32(2600, bleQueue.getLastParsedTimestamp());
    TEST_ASSERT_EQUAL(1, power.onV1DataReceivedCalls);
    TEST_ASSERT_TRUE(eventBus.consumeByType(SystemEventType::BLE_FRAME_PARSED, event));
    TEST_ASSERT_FALSE(eventBus.consumeByType(SystemEventType::BLE_FRAME_PARSED, event));
    TEST_ASSERT_EQUAL_UINT32(2600, event.tsMs);
    TEST_ASSERT_EQUAL_UINT16(PACKET_ID_DISPLAY_DATA, event.detail);
}

void test_disconnect_purges_outgoing_ble_session_and_reconnects_without_ghost_alert() {
    ble.setSessionGeneration(1);
    connectionState.handleSessionOpened(1);
    connectionState.handleConnected(1000, 1);

    const std::vector<uint8_t> oldRow1 = makePacket(
        PACKET_ID_ALERT_DATA,
        makeAlertPayload(1, 2, 24150, 0x90, 0x00, 0x24, 0x00));
    const std::vector<uint8_t> oldRow2 = makePacket(
        PACKET_ID_ALERT_DATA,
        makeAlertPayload(2, 2, 13450, 0xA0, 0x00, 0x30, 0xC1));
    std::vector<uint8_t> oldTable;
    appendPacket(oldTable, oldRow1);
    appendPacket(oldTable, oldRow2);

    mockMillis = 1000;
    mockMicros = 1000 * 1000UL;
    bleQueue.onNotify(oldTable.data(), oldTable.size(), 0xB2CE, 1);
    bleQueue.process();
    TEST_ASSERT_TRUE(parser.hasAlerts());
    TEST_ASSERT_EQUAL_UINT32(24150, parser.getAllAlerts()[0].frequency);
    alertPersistence.setPersistedAlert(parser.getPriorityAlert());
    alertPersistence.startPersistence(1000);
    TEST_ASSERT_TRUE(alertPersistence.isPersistenceActive());

    // Leave outgoing data in both buffering layers: half a frame in the RX
    // accumulator, then a complete frame still waiting in the FreeRTOS queue.
    const std::vector<uint8_t> stalePartial = makePacket(
        PACKET_ID_ALERT_DATA,
        makeAlertPayload(1, 1, 10525, 0x88, 0x00, 0xA8, 0x80));
    const size_t split = stalePartial.size() / 2;
    bleQueue.onNotify(stalePartial.data(), split, 0xB2CE, 1);
    bleQueue.process();
    const std::vector<uint8_t> staleQueued = makePacket(
        PACKET_ID_ALERT_DATA,
        makeAlertPayload(1, 1, 34700, 0x90, 0x00, 0x24, 0x80));
    bleQueue.onNotify(staleQueued.data(), staleQueued.size(), 0xB2CE, 1);

    connectionState.handleSessionClosed(1100, 2);

    TEST_ASSERT_FALSE(parser.hasAlerts());
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(parser.getAlertCount()));
    for (const AlertData& alert : parser.getAllAlerts()) {
        TEST_ASSERT_FALSE(alert.isValid);
        TEST_ASSERT_EQUAL_UINT8(BAND_NONE, alert.band);
        TEST_ASSERT_EQUAL_UINT32(0, alert.frequency);
    }
    TEST_ASSERT_FALSE(alertPersistence.isPersistenceActive());
    TEST_ASSERT_FALSE(alertPersistence.getPersistedAlert().isValid);
    TEST_ASSERT_EQUAL_UINT8(BAND_NONE, alertPersistence.getPersistedAlert().band);
    TEST_ASSERT_EQUAL_UINT32(0, alertPersistence.getPersistedAlert().frequency);
    TEST_ASSERT_EQUAL_UINT32(0, bleQueue.getLastRxMillis());
    TEST_ASSERT_EQUAL_UINT32(0, bleQueue.getLastParsedTimestamp());
    TEST_ASSERT_FALSE(bleQueue.consumeParsedFlag());
    SystemEvent event{};
    TEST_ASSERT_FALSE(eventBus.consumeByType(SystemEventType::BLE_FRAME_PARSED, event));

    const int rxCallsAfterClose = power.onV1DataReceivedCalls;
    bleQueue.onNotify(staleQueued.data(), staleQueued.size(), 0xB2CE, 1);
    ble.setSessionGeneration(3);
    connectionState.handleSessionOpened(3);
    connectionState.handleConnected(1200, 3);
    bleQueue.process();
    TEST_ASSERT_FALSE(parser.hasAlerts());
    TEST_ASSERT_EQUAL(rxCallsAfterClose, power.onV1DataReceivedCalls);

    const std::vector<uint8_t> displayOnly = makePacket(
        PACKET_ID_DISPLAY_DATA,
        std::vector<uint8_t>{0x3F, 0x00, 0x01, 0x02, 0x02, 0x00, 0x00, 0x41, 0x00});
    bleQueue.onNotify(displayOnly.data(), displayOnly.size(), 0xB2CE, 3);
    bleQueue.process();
    TEST_ASSERT_FALSE(parser.hasAlerts());
    TEST_ASSERT_FALSE(alertPersistence.getPersistedAlert().isValid);

    const std::vector<uint8_t> freshAlert = makePacket(
        PACKET_ID_ALERT_DATA,
        makeAlertPayload(1, 1, 33800, 0x90, 0x00, 0x24, 0x80));
    bleQueue.onNotify(freshAlert.data(), freshAlert.size(), 0xB2CE, 3);
    bleQueue.process();
    TEST_ASSERT_TRUE(parser.hasAlerts());
    TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(parser.getAlertCount()));
    TEST_ASSERT_EQUAL_UINT32(33800, parser.getAllAlerts()[0].frequency);
}

void test_ble_queue_rejects_old_generation_item_committed_after_close_purge() {
    const std::vector<uint8_t> alert = makePacket(
        PACKET_ID_ALERT_DATA,
        makeAlertPayload(1, 1, 24150, 0x90, 0x00, 0x24, 0x80));

    bleQueue.closeSession();
    bleQueue.openSession(2);
    TEST_ASSERT_FALSE(bleQueue.tryOnNotify(alert.data(), alert.size(), 0xB2CE, 1));
    TEST_ASSERT_TRUE(bleQueue.enqueueStampedForTest(alert.data(), alert.size(), 0xB2CE, 1));
    bleQueue.process();

    TEST_ASSERT_FALSE(parser.hasAlerts());
    TEST_ASSERT_FALSE(bleQueue.consumeParsedFlag());
    TEST_ASSERT_EQUAL_UINT32(0, bleQueue.getLastParsedTimestamp());
    TEST_ASSERT_EQUAL(0, power.onV1DataReceivedCalls);
    SystemEvent event{};
    TEST_ASSERT_FALSE(eventBus.consumeByType(SystemEventType::BLE_FRAME_PARSED, event));

    TEST_ASSERT_TRUE(bleQueue.tryOnNotify(alert.data(), alert.size(), 0xB2CE, 2));
    bleQueue.process();
    TEST_ASSERT_TRUE(parser.hasAlerts());
    TEST_ASSERT_TRUE(bleQueue.consumeParsedFlag());
    TEST_ASSERT_EQUAL(1, power.onV1DataReceivedCalls);
}

void test_ble_queue_full_drops_incoming_without_evicting_valid_head() {
    bleQueue.end();
    BleQueueModule::Config config;
    config.queueDepth = 1;
    TEST_ASSERT_TRUE(bleQueue.begin(&ble, &parser, &profiles, &preview, &power, &eventBus, config));
    bleQueue.openSession(1);

    const std::vector<uint8_t> first = makePacket(
        PACKET_ID_ALERT_DATA,
        makeAlertPayload(1, 1, 24150, 0x90, 0x00, 0x24, 0x80));
    const std::vector<uint8_t> overflow = makePacket(
        PACKET_ID_ALERT_DATA,
        makeAlertPayload(1, 1, 34700, 0x90, 0x00, 0x24, 0x80));

    bleQueue.onNotify(first.data(), first.size(), 0xB2CE, 1);
    bleQueue.onNotify(overflow.data(), overflow.size(), 0xB2CE, 1);
    bleQueue.process();

    TEST_ASSERT_TRUE(parser.hasAlerts());
    TEST_ASSERT_EQUAL_UINT32(24150, parser.getPriorityAlert().frequency);
    TEST_ASSERT_EQUAL_UINT32(1, perfCounters.queueDrops.load(std::memory_order_relaxed));
}

void test_rx_append_admits_chunk_whole_or_not_at_all() {
    std::vector<uint8_t> rxBuffer;
    size_t readPos = 0;

    // Fill 1000 of the 1024-byte RX cap with whole 200-byte chunks.
    const std::vector<uint8_t> filler(200, 0x5A);
    for (int i = 0; i < 5; ++i) {
        TEST_ASSERT_EQUAL_UINT32(
            200, static_cast<uint32_t>(appendRxClamped(rxBuffer, readPos, filler.data(), filler.size())));
    }
    TEST_ASSERT_EQUAL_UINT32(1000, static_cast<uint32_t>(rxBuffer.size()));

    // 200 more overflows by 176. The old clamp copied the first 24 bytes and
    // silently dropped the newest 176, leaving a fragment that can only splice
    // against unrelated wire bytes. The chunk must be refused whole instead.
    const std::vector<uint8_t> overflow(200, 0xC3);
    TEST_ASSERT_EQUAL_UINT32(
        0, static_cast<uint32_t>(appendRxClamped(rxBuffer, readPos, overflow.data(), overflow.size())));
    TEST_ASSERT_EQUAL_UINT32(1000, static_cast<uint32_t>(rxBuffer.size()));
    for (uint8_t value : rxBuffer) {
        TEST_ASSERT_EQUAL_UINT8(0x5A, value);  // no partial tail spliced in
    }

    // A chunk that still fits whole is admitted.
    const std::vector<uint8_t> fits(24, 0x11);
    TEST_ASSERT_EQUAL_UINT32(
        24, static_cast<uint32_t>(appendRxClamped(rxBuffer, readPos, fits.data(), fits.size())));
    TEST_ASSERT_EQUAL_UINT32(1024, static_cast<uint32_t>(rxBuffer.size()));

    // Consuming the backlog reclaims room, so refusing input is self-limiting.
    readPos = rxBuffer.size();
    TEST_ASSERT_EQUAL_UINT32(
        200, static_cast<uint32_t>(appendRxClamped(rxBuffer, readPos, overflow.data(), overflow.size())));
    TEST_ASSERT_EQUAL_UINT32(200, static_cast<uint32_t>(rxBuffer.size()));
}

void test_ble_queue_rx_overflow_refuses_partial_chunk_and_keeps_room_for_whole_packet() {
    // process() empties the queue into the RX buffer before parsing anything, so
    // queueing more than the 1024-byte cap exercises the overflow branch with no
    // consumption in between.
    const std::vector<uint8_t> filler(250, 0x00);  // contains no ESP_PACKET_START
    for (int i = 0; i < 6; ++i) {
        bleQueue.onNotify(filler.data(), filler.size(), 0xB2CE, kTestSessionGeneration);
    }

    const std::vector<uint8_t> alert = makePacket(
        PACKET_ID_ALERT_DATA,
        makeAlertPayload(1, 1, 24150, 0x90, 0x00, 0x24, 0x80));
    bleQueue.onNotify(alert.data(), alert.size(), 0xB2CE, kTestSessionGeneration);
    bleQueue.process();

    // Four chunks (1000 bytes) are admitted whole; the 5th and 6th no longer fit
    // and are refused whole, leaving 24 bytes free. The 13-byte alert therefore
    // still lands intact and parses. Under the old clamp the 5th chunk's first 24
    // bytes were copied in, filling the buffer to the cap with a fragment that was
    // going to be lost anyway - and that fragment crowded out this real packet.
    TEST_ASSERT_TRUE(parser.hasAlerts());
    TEST_ASSERT_EQUAL_UINT32(24150, parser.getPriorityAlert().frequency);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_rx_append_admits_chunk_whole_or_not_at_all);
    RUN_TEST(test_ble_queue_rx_overflow_refuses_partial_chunk_and_keeps_room_for_whole_packet);
    RUN_TEST(test_ble_queue_parses_display_packet_into_display_state);
    RUN_TEST(test_ble_queue_publishes_parsed_event_to_bus);
    RUN_TEST(test_ble_queue_coalesces_parsed_events_per_process_cycle);
    RUN_TEST(test_ble_queue_alert_burst_uses_single_millis_sample);
    RUN_TEST(test_ble_queue_corrupt_burst_throttles_resync_logs_and_recovers);
    RUN_TEST(test_ble_queue_corrupt_resync_logs_are_time_throttled_across_cycles);
    RUN_TEST(test_ble_queue_resyncs_after_corrupt_prefix_in_single_notify);
    RUN_TEST(test_ble_queue_recovers_after_buffer_without_start_marker);
    RUN_TEST(test_disconnect_purges_outgoing_ble_session_and_reconnects_without_ghost_alert);
    RUN_TEST(test_ble_queue_rejects_old_generation_item_committed_after_close_purge);
    RUN_TEST(test_ble_queue_full_drops_incoming_without_evicting_valid_head);
    return UNITY_END();
}

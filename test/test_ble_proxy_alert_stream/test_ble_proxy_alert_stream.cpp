#include "../mocks/Arduino.h"
#include <atomic>
#include <unity.h>
#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif
#include "../mocks/esp_heap_caps.h"
#include "../mocks/freertos/FreeRTOS.h"
#include "../mocks/freertos/task.h"
#include "../mocks/mock_heap_caps_state.h"
#ifndef configASSERT
#define configASSERT(expr)                                                     \
  do {                                                                         \
    if (!(expr))                                                               \
      TEST_FAIL_MESSAGE("configASSERT failed");                                \
  } while (0)
#endif
#define private public
#include "../../src/ble_client.h"
#undef private

portMUX_TYPE pendingAddrMux = portMUX_INITIALIZER_UNLOCKED;

// Transport setup, connection observation, clock, locks and NimBLE are doubles.
// The phone callbacks, deferred consumer, flow control and final command send
// (including the newer local unmute) are the production owners.
V1BLEClient::V1BLEClient()
    : pClient_(nullptr), pRemoteService_(nullptr), pDisplayDataChar_(nullptr),
      pCommandChar_(nullptr), pCommandCharLong_(nullptr), pServer_(nullptr),
      pProxyService_(nullptr), pProxyNotifyChar_(nullptr),
      pProxyNotifyLongChar_(nullptr), pProxyWriteChar_(nullptr),
      proxyEnabled_(false), proxyServerInitialized_(false),
      proxyServerInitAttempted_(false), proxyName_("V1-Proxy"),
      proxyQueue_(nullptr), phone2v1Queue_(nullptr), proxyQueuesInPsram_(false),
      dataCallback_(nullptr), connectImmediateCallback_(nullptr),
      sessionOpenedCallback_(nullptr), sessionClosedCallback_(nullptr),
      connectStableCallback_(nullptr), targetAddress_(), lastScanStart_(0),
      pScanCallbacks_(nullptr), pClientCallbacks_(nullptr),
      pProxyServerCallbacks_(nullptr), pProxyWriteCallbacks_(nullptr) {}

V1BLEClient::~V1BLEClient() { releaseProxyQueues(); }

void V1BLEClient::setProxyClientConnected(bool connectedState) {
  proxyClientConnected_ = connectedState;
  if (connectedState) {
    proxyClientConnectedOnceThisBoot_ = true;
    proxyFastAdvertisingUntilMs_ = 0;
  }
}

bool V1BLEClient::isConnected() {
  return connected_.load(std::memory_order_acquire) && pClient_ &&
         pClient_->isConnected();
}
bool V1BLEClient::isProxyClientConnected() { return proxyClientConnected_; }
bool V1BLEClient::isConnectBurstSettling() const {
  return connectedFollowupStep_ != ConnectedFollowupStep::NONE;
}

#include "../../src/ble_commands.cpp"
#include "../../src/ble_proxy.cpp"

void setUp() {
  mock_reset_heap_caps();
  mock_reset_semaphore_state();
  mock_reset_nimble_state();
  static uint32_t testStartMs = 1000;
  testStartMs += 10000;
  mockMillis = testStartMs;
  mockMicros = 0;
}
void tearDown() {}

#include "../../src/packet_parser.cpp"
#include "../../src/packet_parser.h"
#include "../../src/packet_parser_alerts.cpp"
#include "../mocks/display.h"
#include "../mocks/settings.h"
#include "../mocks/modules/power/power_module.h"
#include "../mocks/v1_profiles.h"
#include "../../src/modules/alert_persistence/alert_persistence_module.cpp"
#include "../../src/modules/alp/alp_event_latch.cpp"
#include "../../src/modules/quiet/quiet_coordinator_module.cpp"
#include "../../src/modules/voice/voice_module.cpp"
#include "../../src/modules/display/render_frame_composer.cpp"
#include "../../src/modules/display/display_pipeline_module.cpp"

// Panel rendering and speaker acceptance are host substitutes. The production
// pipeline, composer, persistence and encounter/voice state still execute.
AudioPlaybackResult try_play_frequency_voice(AlertBand, uint16_t, AlertDirection,
                                             VoiceAlertMode, bool, uint8_t, AudioPlaybackReceipt*) {
  return AudioPlaybackResult::Unavailable;
}
AudioPlaybackResult try_play_direction_only(AlertDirection, uint8_t, AudioPlaybackReceipt*) {
  return AudioPlaybackResult::Unavailable;
}
AudioPlaybackResult try_play_threat_escalation(AlertBand, uint16_t, AlertDirection,
                                               uint8_t, uint8_t, uint8_t, uint8_t,
                                               VoiceAlertMode, bool, bool, AudioPlaybackReceipt*) {
  return AudioPlaybackResult::Unavailable;
}
class DisplayPreviewModule {
public:
  bool isRunning() const { return false; }
  void cancel() {}
};
#include "../../src/modules/ble/ble_queue_module.cpp"
#include "../../src/modules/ble/connection_state_module.cpp"

struct StreamFixture {
  V1BLEClient client;
  NimBLEClient link;
  NimBLEConnInfo phone;
  NimBLECharacteristic input{V1_COMMAND_WRITE_UUID};
  NimBLERemoteCharacteristic remote{V1_COMMAND_WRITE_UUID};
  V1BLEClient::ProxyServerCallbacks lifecycle{&client};
  V1BLEClient::ProxyWriteCallbacks writer{&client};
  BleQueueModule queue;
  PacketParser parser;
  V1ProfileManager profiles;
  DisplayPreviewModule preview;
  PowerModule power;
  V1Display display;
  AlertPersistenceModule persistence;
  SettingsManager settings;
  VoiceModule voice;
  DisplayMode mode = DisplayMode::IDLE;
  DisplayPipelineModule pipeline;
  ConnectionStateModule connection;
  bool detailedStream = false;
  bool writesSucceed = true;
  unsigned starts = 0, stops = 0;
  uint32_t ingress = 0;
  StreamFixture() {
    mockMillis += 10; // The production sender paces writes across fixtures too.
    TEST_ASSERT_TRUE(client.allocateProxyQueues());
    client.proxyEnabled_ = true;
    client.phoneCmdMutex_ = xSemaphoreCreateMutex();
    client.bleMutex_ = xSemaphoreCreateMutex();
    link.setConnected(true);
    client.pClient_ = &link;
    client.pCommandChar_ = &remote;
    client.connected_.store(true);
    client.sessionGeneration_.store(1);
    client.sessionPublicationGate_.open(1);
    client.bleState_ = BLEState::CONNECTED;
    client.connectedFollowupStep_ = V1BLEClient::ConnectedFollowupStep::NONE;
    TEST_ASSERT_TRUE(
        queue.begin(&client, &parser, &profiles, &preview, &power));
    connection.begin(&client, &parser, &display, &power, &queue, &persistence);
    voice.begin(&settings);
    DisplayPipelineDependencies dependencies;
    dependencies.displayMode = &mode;
    dependencies.display = &display;
    dependencies.parser = &parser;
    dependencies.settings = &settings;
    dependencies.ble = &client;
    dependencies.alertPersistence = &persistence;
    dependencies.voice = &voice;
    pipeline.begin(dependencies);
    connection.handleSessionOpened(1);
    connection.handleConnected(mockMillis, 1);
    remote.setWriteEntryHook([this]() { observeShortWrite(); });
    feedDisplay();
    client.onV1DisplayFlowControl(false);
    TEST_ASSERT_TRUE(client.requestAlertData());
    TEST_ASSERT_TRUE(detailedStream);
    lifecycle.onConnect(nullptr, phone);
    client.drainProxyCallbackEvents();
  }
  ~StreamFixture() { queue.end(); }
  void observeShortWrite() {
    if (!writesSucceed)
      return;
    const auto &p = remote.lastWriteValue();
    if (p.size() == 7 && p[1] == 0xDA && p[2] == 0xE6) {
      if (p[3] == 0x41) {
        detailedStream = true;
        ++starts;
      }
      if (p[3] == 0x42) {
        detailedStream = false;
        ++stops;
      }
    }
  }
  static std::vector<uint8_t> frame(uint8_t id, std::initializer_list<uint8_t> payload,
                                   uint8_t origin = 0xEA, uint8_t destination = 0) {
    if (!destination) destination = (id == 0x65 || id == PACKET_ID_RESP_VERSION) ? 0xD6 : 0xD8;
    std::vector<uint8_t> p{0xAA, destination, origin, id,
                           static_cast<uint8_t>(payload.size() + (origin == 0xEA ? 1 : 0))};
    p.insert(p.end(), payload);
    uint8_t sum = 0;
    for (uint8_t b : p)
      sum += b;
    if (origin == 0xEA) p.push_back(sum);
    p.push_back(0xAB);
    return p;
  }
  void enqueue(const std::vector<uint8_t>& p, uint16_t characteristic = 0xB2CE) {
    ingress = client.noteV1NotificationIngress();
    TEST_ASSERT_TRUE(queue.tryOnNotify(p.data(), p.size(), characteristic, client.sessionGeneration(),
                                       mockMillis, ingress));
  }
  void present(bool parsed = true) {
    queue.process();
    TEST_ASSERT_EQUAL(parsed, queue.consumeParsedFlag());
    pipeline.handleParsed(mockMillis);
  }
  void deliver(uint8_t id, std::initializer_list<uint8_t> payload) {
    enqueue(frame(id, payload));
    present();
  }
  void version(uint32_t value = 41039) {
    deliver(PACKET_ID_RESP_VERSION,
            {'V', static_cast<uint8_t>('0' + value / 10000), '.',
             static_cast<uint8_t>('0' + value / 1000 % 10),
             static_cast<uint8_t>('0' + value / 100 % 10),
             static_cast<uint8_t>('0' + value / 10 % 10),
             static_cast<uint8_t>('0' + value % 10)});
  }
  void alert(uint8_t indexCount = 0x11) {
    deliver(PACKET_ID_ALERT_DATA, {indexCount, 0x87, 0x8C, 0xAC, 0, 0x22, 0x80});
  }
  void idle(uint8_t origin = 0xEA) {
    enqueue(frame(PACKET_ID_DISPLAY_DATA, {0x77, 0x77, 0, 0, 0, 0x0C, 0x04, 0x62}, origin));
    present();
  }
  void assertLive() {
    TEST_ASSERT_EQUAL_UINT32(1, parser.getAlertCount());
    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::V1_LIVE, display.lastRenderFrame.primaryKind);
    TEST_ASSERT_EQUAL_UINT16(34700, display.lastRenderFrame.v1Priority.frequency);
  }
  void feedDisplay() {
    deliver(PACKET_ID_DISPLAY_DATA, {0x06, 0x06, 7, 0x22, 0x22, 0x0C, 0, 0x62});
  }
  void phoneStop() {
    mockMillis += 100;
    // reqStopAlertData to DA with V1connection LE origin E6, valid checksum.
    const uint8_t stop[] = {0xAA, 0xDA, 0xE6, 0x42, 1, 0xAD, 0xAB};
    input.setValue(stop, sizeof(stop));
    writer.onWrite(&input, phone);
    TEST_ASSERT_EQUAL_INT(1, client.processPhoneCommandQueue());
    TEST_ASSERT_EQUAL_UINT8_ARRAY(stop, remote.lastWriteValue().data(),
                                  sizeof(stop));
    TEST_ASSERT_FALSE(detailedStream);
  }
  void disconnectPhone() {
    lifecycle.onDisconnect(nullptr, phone, 0);
    client.drainProxyCallbackEvents();
  }
  void tick(bool displays) {
    mockMillis += 100;
    if (displays)
      feedDisplay();
    if (detailedStream)
      deliver(PACKET_ID_ALERT_DATA, {0x11, 0x87, 0x8C, 0xAC, 0, 0x22, 0x80});
    TEST_ASSERT_TRUE(connection.process(mockMillis));
  }
};

void test_phone_stop_then_disconnect_must_recover_detailed_alerts_despite_live_display() {
  StreamFixture f;
  f.phoneStop();
  f.disconnectPhone();
  for (int n = 0; n < 600; ++n)
    f.tick(true);
  printf("STOP_LIVE_DISPLAY starts=%u stops=%u stream=%d alertCount=%zu "
         "lastRxAge=%lu\n",
         f.starts, f.stops, f.detailedStream, f.parser.getAlertCount(),
         mockMillis - f.queue.getLastRxMillis());
  AlertData a;
  printf("RENDERABLE_PRIORITY=%d\n", f.parser.getRenderablePriorityAlert(a));
  TEST_ASSERT_TRUE_MESSAGE(
      f.detailedStream, "Phone stop persists after phone retirement: regular "
                        "display traffic prevents detailed stream recovery");
  TEST_ASSERT_EQUAL_UINT32(1, f.parser.getAlertCount());
}
void test_no_stop_phone_disconnect_retains_healthy_detailed_stream() {
  StreamFixture f;
  f.disconnectPhone();
  for (int n = 0; n < 600; ++n)
    f.tick(true);
  TEST_ASSERT_TRUE(f.detailedStream);
  TEST_ASSERT_EQUAL_UINT32(1, f.starts);
  TEST_ASSERT_EQUAL_UINT32(1, f.parser.getAlertCount());
}
void test_stop_recovers_when_all_transport_traffic_becomes_stale() {
  StreamFixture f;
  f.phoneStop();
  f.disconnectPhone();
  for (int n = 0; n < 60; ++n)
    f.tick(false);
  TEST_ASSERT_TRUE(f.detailedStream);
  TEST_ASSERT_GREATER_THAN(1, f.starts);
  TEST_ASSERT_EQUAL_UINT32(1, f.parser.getAlertCount());
}
void test_connected_phone_stop_remains_owned_until_departure() {
  StreamFixture f;
  f.phoneStop();
  for (int n = 0; n < 30; ++n)
    f.tick(true);
  TEST_ASSERT_FALSE(f.detailedStream);
  TEST_ASSERT_EQUAL_UINT32(1, f.starts);
  f.disconnectPhone();
  for (int n = 0; n < 30; ++n)
    f.tick(true);
  TEST_ASSERT_TRUE(f.detailedStream);
  TEST_ASSERT_EQUAL_UINT32(2, f.starts);
}
void test_stop_recovery_retries_after_busy_and_transient_write_failure() {
  StreamFixture f;
  f.phoneStop();
  f.disconnectPhone();
  const uint8_t busy[] = {0x41};
  f.client.onV1Busy(busy, 1);
  f.client.onV1DisplayFlowControl(true);
  mockMillis += 2000;
  TEST_ASSERT_TRUE(f.connection.process(mockMillis));
  TEST_ASSERT_FALSE(f.detailedStream);
  f.client.onV1DisplayFlowControl(false);
  f.writesSucceed = false;
  f.remote.setWriteValueResult(false);
  mockMillis += 2000;
  TEST_ASSERT_TRUE(f.connection.process(mockMillis));
  TEST_ASSERT_FALSE(f.detailedStream);
  f.writesSucceed = true;
  f.remote.setWriteValueResult(true);
  for (int n = 0; n < 30; ++n)
    f.tick(true);
  TEST_ASSERT_TRUE(f.detailedStream);
}
void test_long_characteristic_stop_recovers_after_complete_forwarding() {
  for (bool indexed : {false, true}) {
    StreamFixture f;
    NimBLECharacteristic longInput{"92A0B8D2-9E05-11E2-AA59-F23C91AEC05E"};
    NimBLERemoteCharacteristic longOutput{
        "92A0B8D2-9E05-11E2-AA59-F23C91AEC05E"};
    f.client.pCommandCharLong_ = &longOutput;
    const uint8_t raw[] = {0xAA, 0xDA, 0xE6, 0x42, 1, 0xAD, 0xAB};
    const uint8_t first[] = {0x12, 0xAA, 0xDA, 0xE6, 0x42};
    const uint8_t second[] = {0x22, 1, 0xAD, 0xAB};
    longInput.setValue(indexed ? first : raw,
                       indexed ? sizeof(first) : sizeof(raw));
    f.writer.onWrite(&longInput, f.phone);
    if (indexed) {
      longInput.setValue(second, sizeof(second));
      f.writer.onWrite(&longInput, f.phone);
    }
    mockMillis += 100;
    TEST_ASSERT_EQUAL_INT(1, f.client.processPhoneCommandQueue());
    if (indexed)
      TEST_ASSERT_EQUAL_INT(1, f.client.processPhoneCommandQueue());
    TEST_ASSERT_EQUAL_UINT32(indexed ? 2 : 1, longOutput.writeValueCalls());
    f.detailedStream =
        false; // The delivered complete stop ends the protocol stream.
    f.disconnectPhone();
    for (int n = 0; n < 30; ++n)
      f.tick(true);
    TEST_ASSERT_TRUE(f.detailedStream);
    TEST_ASSERT_EQUAL_UINT32(2, f.starts);
  }
}
void test_long_start_releases_phone_stop_and_rejected_start_recovers() {
  for (bool indexed : {false, true}) {
    StreamFixture f;
    f.phoneStop();
    NimBLECharacteristic longInput{"92A0B8D2-9E05-11E2-AA59-F23C91AEC05E"};
    NimBLERemoteCharacteristic longOutput{
        "92A0B8D2-9E05-11E2-AA59-F23C91AEC05E"};
    f.client.pCommandCharLong_ = &longOutput;
    const uint8_t raw[] = {0xAA, 0xDA, 0xE6, 0x41, 1, 0xAC, 0xAB};
    const uint8_t first[] = {0x12, 0xAA, 0xDA, 0xE6, 0x41};
    const uint8_t second[] = {0x22, 1, 0xAC, 0xAB};
    longInput.setValue(indexed ? first : raw,
                       indexed ? sizeof(first) : sizeof(raw));
    f.writer.onWrite(&longInput, f.phone);
    if (indexed) {
      longInput.setValue(second, sizeof(second));
      f.writer.onWrite(&longInput, f.phone);
    }
    mockMillis += 100;
    TEST_ASSERT_EQUAL_INT(1, f.client.processPhoneCommandQueue());
    if (indexed) {
      TEST_ASSERT_TRUE(f.client.phoneOwnsStoppedAlertStream());
      TEST_ASSERT_EQUAL_INT(1, f.client.processPhoneCommandQueue());
    }
    TEST_ASSERT_EQUAL_UINT32(indexed ? 2 : 1, longOutput.writeValueCalls());
    TEST_ASSERT_FALSE(f.client.phoneOwnsStoppedAlertStream());
    f.deliver(0x65, {0x41});
    for (int n = 0; n < 40; ++n)
      f.tick(true);
    TEST_ASSERT_TRUE(f.detailedStream);
    TEST_ASSERT_EQUAL_UINT32(2, f.starts);
  }
}
void test_new_phone_does_not_inherit_retired_phone_stop() {
  for (bool drainDisconnect : {false, true}) {
    StreamFixture f;
    f.phoneStop();
    f.lifecycle.onDisconnect(nullptr, f.phone, 0);
    if (drainDisconnect)
      f.client.drainProxyCallbackEvents();
    f.lifecycle.onConnect(nullptr, f.phone);
    f.client.drainProxyCallbackEvents();
    TEST_ASSERT_FALSE(f.client.phoneOwnsStoppedAlertStream());
    for (int n = 0; n < 40; ++n)
      f.tick(true);
    TEST_ASSERT_TRUE(f.detailedStream);
    TEST_ASSERT_EQUAL_UINT32(2, f.starts);
  }
}
void test_phone_replacement_during_stop_write_cannot_inherit_ownership() {
  for (unsigned handle = 0; handle < 3; ++handle) {
    StreamFixture f;
    NimBLECharacteristic longInput{"92A0B8D2-9E05-11E2-AA59-F23C91AEC05E"};
    NimBLERemoteCharacteristic longOutput{
        "92A0B8D2-9E05-11E2-AA59-F23C91AEC05E"};
    f.client.pCommandCharLong_ = &longOutput;
    const uint8_t raw[] = {0xAA, 0xDA, 0xE6, 0x42, 1, 0xAD, 0xAB};
    const uint8_t indexed[] = {0x11, 0xAA, 0xDA, 0xE6, 0x42, 1, 0xAD, 0xAB};
    auto replacePhone = [&]() {
      f.detailedStream = false; // The stop reached the detector.
      f.lifecycle.onDisconnect(nullptr, f.phone, 0);
      f.lifecycle.onConnect(nullptr, f.phone);
    };
    NimBLECharacteristic &input = handle == 0 ? f.input : longInput;
    NimBLERemoteCharacteristic &output = handle == 0 ? f.remote : longOutput;
    output.setWriteEntryHook(replacePhone);
    input.setValue(handle == 2 ? indexed : raw,
                   handle == 2 ? sizeof(indexed) : sizeof(raw));
    f.writer.onWrite(&input, f.phone);
    mockMillis += 100;
    TEST_ASSERT_EQUAL_INT(1, f.client.processPhoneCommandQueue());
    f.remote.setWriteEntryHook([&]() { f.observeShortWrite(); });
    f.client.drainProxyCallbackEvents();
    TEST_ASSERT_FALSE(f.client.phoneOwnsStoppedAlertStream());
    for (int n = 0; n < 40; ++n)
      f.tick(true);
    TEST_ASSERT_TRUE(f.detailedStream);
    TEST_ASSERT_EQUAL_UINT32(2, f.starts);
  }
}
void test_canonical_queue_rejection_reaches_the_actual_volume_write_receipt() {
  StreamFixture f;
  f.disconnectPhone();
  mockMillis += 100;
  TEST_ASSERT_EQUAL(SendResult::SENT, f.client.setVolumeResult(1, 2));
  const uint32_t revision = f.client.volumeWriteRevision();
  TEST_ASSERT_FALSE(f.client.volumeWriteRejected(revision));
  f.deliver(0x65, {0x39});
  TEST_ASSERT_TRUE(f.client.volumeWriteRejected(revision));
  f.feedDisplay();
  mockMillis += 100;
  TEST_ASSERT_EQUAL(SendResult::SENT, f.client.setVolumeResult(7, 3));
  TEST_ASSERT_FALSE(f.client.volumeWriteRejected(revision));
}
void test_connected_phone_without_stop_keeps_existing_start_and_silence_recovery() {
  for (bool missingStart : {false, true}) {
    StreamFixture f;
    if (missingStart)
      f.client.alertDataRequestGate_.invalidate(f.client.sessionGeneration());
    for (int n = 0; n < 40; ++n) {
      mockMillis += 100;
      if (missingStart)
        f.feedDisplay();
      TEST_ASSERT_TRUE(f.connection.process(mockMillis));
    }
    TEST_ASSERT_GREATER_THAN_UINT32(1, f.starts);
    TEST_ASSERT_TRUE(f.client.proxyClientConnected_.load());
  }
}

static void stopped_idle_persistence(uint8_t persistSec) {
  for (uint8_t origin : {0xE9, 0xEA}) {
    StreamFixture f;
    f.settings.slotAlertPersistSec[0] = persistSec;
    f.version();
    f.alert();
    f.assertLive();
    const uint32_t alertAt = mockMillis;
    const uint32_t lifetime = f.parser.alertLifetime();
    f.phoneStop();
    mockMillis = alertAt + 1500;
    f.idle(origin);
    f.assertLive(); // Strictly older than 1500 ms, never equal.
    mockMillis = alertAt + 1501;
    f.idle(origin);
    TEST_ASSERT_FALSE(f.parser.hasAlerts());
    TEST_ASSERT_EQUAL_UINT32(lifetime + 1, f.parser.alertLifetime());
    TEST_ASSERT_EQUAL_UINT32(mockMillis, f.parser.clearedAlert().clearedAtMs);
    TEST_ASSERT_EQUAL(persistSec ? RenderFramePrimaryKind::V1_PERSISTED : RenderFramePrimaryKind::IDLE,
                      f.display.lastRenderFrame.primaryKind);
    const uint32_t clearedAt = mockMillis;
    if (persistSec) {
      TEST_ASSERT_EQUAL_UINT16(34700, f.display.lastRenderFrame.v1Priority.frequency);
      f.pipeline.refreshBlinkTick(clearedAt + persistSec * 1000u - 1);
      TEST_ASSERT_EQUAL(RenderFramePrimaryKind::V1_PERSISTED, f.display.lastRenderFrame.primaryKind);
    }
    // Expiry is owned by the real pipeline, even without another packet.
    f.pipeline.refreshBlinkTick(clearedAt + persistSec * 1000u);
    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::IDLE, f.display.lastRenderFrame.primaryKind);
    TEST_ASSERT_EQUAL_CHAR('A', f.display.lastRenderFrame.primaryState.bogeyCounterChar);
    TEST_ASSERT_EQUAL_INT(0, f.display.lastRenderFrame.cardCount);
    TEST_ASSERT_FALSE(f.display.lastRenderFrame.v1Priority.isValid);
    mockMillis = clearedAt + 60000;
    f.idle(origin);
    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::IDLE, f.display.lastRenderFrame.primaryKind);
    TEST_ASSERT_EQUAL_UINT32(1, f.starts);
    TEST_ASSERT_EQUAL_UINT32(1, f.stops);
  }
}
void test_stopped_idle_persistence_zero() { stopped_idle_persistence(0); }
void test_stopped_idle_persistence_three() { stopped_idle_persistence(3); }
void test_stopped_idle_persistence_five() { stopped_idle_persistence(5); }

void test_stopped_idle_requires_successful_stop_and_no_silence_timeout() {
  StreamFixture f;
  f.version();
  f.alert();
  mockMillis += 60000;
  f.idle();
  f.assertLive(); // Ordinary stream: idle never replaces a count-zero table.
  f.remote.setWriteValueResult(false);
  f.writesSucceed = false;
  const uint8_t stop[] = {0xAA, 0xDA, 0xE6, 0x42, 1, 0xAD, 0xAB};
  f.input.setValue(stop, sizeof(stop));
  f.writer.onWrite(&f.input, f.phone);
  TEST_ASSERT_EQUAL_INT(0, f.client.processPhoneCommandQueue());
  f.idle();
  f.assertLive();
  f.remote.setWriteValueResult(true);
  f.writesSucceed = true;
  f.client.onV1DisplayFlowControl(true);
  TEST_ASSERT_EQUAL_INT(0, f.client.processPhoneCommandQueue());
  f.idle();
  f.assertLive(); // Held-off stop has still not transmitted.
  mockMillis += 10;
  TEST_ASSERT_EQUAL_INT(1, f.client.processPhoneCommandQueue());
  mockMillis += 60000;
  f.pipeline.refreshBlinkTick(mockMillis);
  f.assertLive(); // Successful stop plus silence is also insufficient.
  f.idle();
  TEST_ASSERT_FALSE(f.parser.hasAlerts());
}

void test_stopped_idle_rejects_ambiguous_raw_display_fields() {
  // Single-field variants of the qualified packet: digit, blink, blank, logic,
  // Euro, reserved glyph, dot, strength, both raw images, laser, dark, system
  // status, display-active, soft/logic/Auto Mute, missing/different mode.
  const uint8_t variants[][2] = {
      {0, 0x06}, {1, 0}, {0, 0}, {0, 0x18}, {0, 0x38}, {0, 0x39},
      {0, 0x3E}, {0, 0x1C}, {0, 0x58}, {0, 0x01}, {0, 0xF7},
      {2, 1}, {3, 0x22}, {4, 0x22}, {3, 0x21}, {4, 0x21},
      {5, 0x04}, {5, 0x08}, {5, 0x8C}, {5, 0x0D}, {5, 0x1C},
      {6, 0x06}, {6, 0x14}, {6, 0}, {6, 0x08}, {6, 0x0C}};
  for (const auto& variant : variants) {
    StreamFixture f;
    f.version();
    f.alert();
    f.phoneStop();
    mockMillis += 2000;
    auto packet = StreamFixture::frame(PACKET_ID_DISPLAY_DATA, {0x77, 0x77, 0, 0, 0, 0x0C, 4, 0x62});
    packet[5 + variant[0]] = variant[1];
    packet[packet.size() - 2] = 0;
    for (size_t i = 0; i < packet.size() - 2; ++i) packet[packet.size() - 2] += packet[i];
    f.enqueue(packet);
    f.present();
    TEST_ASSERT_EQUAL_UINT32(1, f.parser.getAlertCount());
    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::V1_LIVE, f.display.lastRenderFrame.primaryKind);
  }
}

void test_stopped_idle_requires_supported_version_capabilities() {
  for (uint32_t version : {0u, 38950u, 41018u, 41038u, 41039u, 41040u, 49999u, 50000u}) {
    StreamFixture f;
    if (version) f.version(version);
    f.alert();
    f.phoneStop();
    mockMillis += 2000;
    f.idle();
    TEST_ASSERT_EQUAL(version < 41039 || version >= 50000, f.parser.hasAlerts());
  }
}

void test_stopped_idle_requires_canonical_frame() {
  for (unsigned variant = 0; variant < 4; ++variant) {
    StreamFixture f;
    f.version();
    f.alert();
    f.phoneStop();
    mockMillis += 2000;
    auto packet = StreamFixture::frame(PACKET_ID_DISPLAY_DATA, {0x77, 0x77, 0, 0, 0, 0x0C, 4, 0x62});
    if (variant == 0) packet[packet.size() - 2] ^= 1;
    else if (variant == 1) { packet[1] = 0xD6; packet[packet.size() - 2] -= 2; }
    else if (variant == 2) { packet[2] = 0xE6; packet[packet.size() - 2] -= 4; }
    else packet = StreamFixture::frame(PACKET_ID_DISPLAY_DATA, {0x77, 0x77, 0, 0, 0, 0x0C, 4});
    f.enqueue(packet);
    f.present(false);
    f.assertLive();
  }
}

void test_stopped_idle_preserves_recent_complete_and_partial_positive_rows() {
  for (bool partial : {false, true}) {
    StreamFixture f;
    f.version();
    f.alert();
    f.phoneStop();
    mockMillis += 2000;
    f.alert(partial ? 0x12 : 0x11);
    const uint32_t positiveAt = mockMillis;
    mockMillis += 1500;
    f.idle();
    f.assertLive();
    if (partial) {
      // Idle did not discard the unfinished candidate. A timely second row
      // still publishes the complete table at the original receipt times.
      f.deliver(PACKET_ID_ALERT_DATA, {0x22, 0x5E, 0x56, 0x90, 0, 0x24, 0});
      TEST_ASSERT_EQUAL_UINT32(2, f.parser.getAlertCount());
    }
    mockMillis = positiveAt + (partial ? 3001 : 1501);
    f.idle();
    TEST_ASSERT_FALSE(f.parser.hasAlerts());
  }
}

void test_stopped_idle_receipt_age_survives_gap_and_dequeue_delay() {
  StreamFixture f;
  f.settings.slotAlertPersistSec[0] = 5;
  f.version();
  f.alert();
  const uint32_t alertAt = mockMillis;
  f.phoneStop();
  f.client.noteV1NotificationIngress(); // Known notification gap.
  mockMillis = alertAt + 500;
  f.idle();
  TEST_ASSERT_FALSE(f.parser.hasFreshAlertTable(mockMillis));
  f.assertLive(); // Invalidated freshness is not age >1500.
  mockMillis = alertAt + 1501;
  f.enqueue(StreamFixture::frame(PACKET_ID_DISPLAY_DATA, {0x77, 0x77, 0, 0, 0, 0x0C, 4, 0x62}));
  mockMillis += 6000;
  f.present();
  TEST_ASSERT_FALSE(f.parser.hasAlerts());
  TEST_ASSERT_EQUAL_UINT32(alertAt + 1501, f.parser.clearedAlert().clearedAtMs);
  TEST_ASSERT_EQUAL(RenderFramePrimaryKind::IDLE, f.display.lastRenderFrame.primaryKind);
}

void test_stopped_idle_rejects_pre_stop_and_during_write_receipts() {
  for (bool duringWrite : {false, true}) {
    StreamFixture f;
    f.version();
    f.alert();
    mockMillis += 2000;
    auto enqueueIdle = [&]() {
      f.enqueue(StreamFixture::frame(PACKET_ID_DISPLAY_DATA, {0x77, 0x77, 0, 0, 0, 0x0C, 4, 0x62}));
    };
    if (duringWrite) f.remote.setWriteEntryHook([&]() { f.observeShortWrite(); enqueueIdle(); });
    else enqueueIdle();
    f.phoneStop();
    f.present();
    f.assertLive();
    f.idle();
    TEST_ASSERT_FALSE(f.parser.hasAlerts());
  }
}

void test_stopped_idle_partial_frame_keeps_pre_stop_boundary() {
  for (bool indexed : {false, true}) {
    StreamFixture f;
    f.version();
    f.alert();
    mockMillis += 2000;
    const auto idle = StreamFixture::frame(PACKET_ID_DISPLAY_DATA, {0x77, 0x77, 0, 0, 0, 0x0C, 4, 0x62});
    std::vector<uint8_t> head, tail;
    if (indexed) { head.push_back(0x12); tail.push_back(0x22); }
    head.insert(head.end(), idle.begin(), idle.begin() + 7);
    tail.insert(tail.end(), idle.begin() + 7, idle.end());
    f.enqueue(head, indexed ? 0xB4E0 : 0xB2CE);
    f.present(false);
    f.phoneStop();
    f.enqueue(tail, indexed ? 0xB4E0 : 0xB2CE);
    f.present();
    f.assertLive();
    f.idle();
    TEST_ASSERT_FALSE(f.parser.hasAlerts());
  }
}

void test_stopped_idle_long_command_uses_final_successful_write_boundary() {
  for (bool indexed : {false, true}) {
    StreamFixture f;
    f.version();
    f.alert();
    mockMillis += 2000;
    NimBLECharacteristic input{"92A0B8D2-9E05-11E2-AA59-F23C91AEC05E"};
    NimBLERemoteCharacteristic output{"92A0B8D2-9E05-11E2-AA59-F23C91AEC05E"};
    f.client.pCommandCharLong_ = &output;
    const uint8_t raw[] = {0xAA, 0xDA, 0xE6, 0x42, 1, 0xAD, 0xAB};
    const uint8_t first[] = {0x12, 0xAA, 0xDA, 0xE6, 0x42};
    const uint8_t second[] = {0x22, 1, 0xAD, 0xAB};
    input.setValue(indexed ? first : raw, indexed ? sizeof(first) : sizeof(raw));
    f.writer.onWrite(&input, f.phone);
    if (indexed) {
      input.setValue(second, sizeof(second));
      f.writer.onWrite(&input, f.phone);
      TEST_ASSERT_EQUAL_INT(1, f.client.processPhoneCommandQueue());
      f.idle();
      f.assertLive(); // Incomplete indexed command cannot authorize retirement.
    }
    output.setWriteValueResult(false);
    TEST_ASSERT_EQUAL_INT(0, f.client.processPhoneCommandQueue());
    f.idle();
    f.assertLive();
    output.setWriteValueResult(true);
    output.setWriteEntryHook([&]() {
      f.enqueue(StreamFixture::frame(PACKET_ID_DISPLAY_DATA, {0x77, 0x77, 0, 0, 0, 0x0C, 4, 0x62}));
    });
    TEST_ASSERT_EQUAL_INT(1, f.client.processPhoneCommandQueue());
    f.present();
    f.assertLive(); // Notification during the final write is still ambiguous.
    f.idle();
    TEST_ASSERT_FALSE(f.parser.hasAlerts());
  }
}

void test_stopped_idle_empty_publication_does_not_issue_quiet_commands() {
  StreamFixture f;
  QuietCoordinatorModule quiet;
  quiet.begin(&f.client, &f.parser);
  V1InTheBoxSettings boxes;
  for (auto& band : boxes.bands) { band.unmuteInside = true; band.muteOutside = true; }
  quiet.setInTheBoxSettings(boxes, f.client.sessionGeneration());
  struct Observations { unsigned empty = 0; uint32_t emptyAt = 0; } observed;
  f.parser.setAlertTableObserver([](const AlertData*, size_t count, uint8_t, uint32_t at, void* context) {
    if (count == 0) {
      auto& observations = *static_cast<Observations*>(context);
      ++observations.empty;
      observations.emptyAt = at;
    }
  }, &observed);
  f.version();
  f.alert();
  quiet.processInTheBox(mockMillis);
  f.phoneStop();
  const auto writes = f.remote.writeValueCalls();
  mockMillis += 2000;
  f.idle();
  quiet.processInTheBox(mockMillis);
  TEST_ASSERT_EQUAL_UINT32(1, observed.empty);
  TEST_ASSERT_EQUAL_UINT32(mockMillis, observed.emptyAt);
  TEST_ASSERT_EQUAL_UINT32(writes, f.remote.writeValueCalls());
  TEST_ASSERT_FALSE(quiet.inTheBoxMuteActive());
  f.idle();
  TEST_ASSERT_EQUAL_UINT32(1, observed.empty);
  f.disconnectPhone();
  quiet.processInTheBox(mockMillis);
  TEST_ASSERT_EQUAL_UINT32(writes, f.remote.writeValueCalls());
  TEST_ASSERT_FALSE(quiet.inTheBoxMuteActive());
}

void test_stopped_idle_late_old_receipt_cannot_override_new_positive_row() {
  StreamFixture f;
  f.version();
  f.alert();
  f.phoneStop();
  mockMillis += 2000;
  // A held, older long frame is processed after the newer short row. The
  // first chunk receipt time cannot become the later completion time.
  auto idle = StreamFixture::frame(PACKET_ID_DISPLAY_DATA, {0x77, 0x77, 0, 0, 0, 0x0C, 4, 0x62});
  std::vector<uint8_t> head{0x12};
  head.insert(head.end(), idle.begin(), idle.begin() + 7);
  f.enqueue(head, 0xB4E0);
  f.present(false);
  mockMillis += 2000;
  f.alert(0x12); // Positive partial table; the old complete table is much older.
  std::vector<uint8_t> tail{0x22};
  tail.insert(tail.end(), idle.begin() + 7, idle.end());
  f.enqueue(tail, 0xB4E0);
  f.present();
  f.assertLive();
}

void test_stopped_idle_start_and_session_replacement_revoke_authority() {
  for (unsigned replacement = 0; replacement < 3; ++replacement) {
    StreamFixture f;
    f.version();
    f.alert();
    f.phoneStop();
    mockMillis += 2000;
    if (replacement == 0) {
      const uint8_t start[] = {0xAA, 0xDA, 0xE6, 0x41, 1, 0xAC, 0xAB};
      f.input.setValue(start, sizeof(start));
      f.writer.onWrite(&f.input, f.phone);
      TEST_ASSERT_EQUAL_INT(1, f.client.processPhoneCommandQueue());
    } else if (replacement == 1) {
      f.lifecycle.onDisconnect(nullptr, f.phone, 0);
      f.lifecycle.onConnect(nullptr, f.phone);
      f.client.drainProxyCallbackEvents();
    } else {
      f.connection.handleSessionClosed(mockMillis, 2);
      f.client.sessionGeneration_.store(2);
      f.client.sessionPublicationGate_.open(2);
      f.connection.handleSessionOpened(2);
      f.connection.handleConnected(mockMillis, 2);
      f.version();
      f.alert();
      mockMillis += 2000;
    }
    TEST_ASSERT_FALSE(f.client.phoneOwnsStoppedAlertStream());
    f.idle();
    f.assertLive();
    // The ordinary complete zero table still ends the encounter immediately.
    f.deliver(PACKET_ID_ALERT_DATA, {0, 0, 0, 0, 0, 0, 0});
    TEST_ASSERT_FALSE(f.parser.hasAlerts());
  }
}

void test_stopped_idle_receipt_and_ingress_rollover() {
  StreamFixture f;
  f.version();
  mockMillis = UINT32_MAX - 1000u;
  f.client.v1NotificationIngressSequence_.store(UINT32_MAX - 1u);
  f.alert();
  f.phoneStop();
  mockMillis = 501;
  f.idle();
  TEST_ASSERT_EQUAL_UINT32(1, f.ingress);
  TEST_ASSERT_FALSE(f.parser.hasAlerts());
  TEST_ASSERT_EQUAL_UINT32(501, f.parser.clearedAlert().clearedAtMs);
}
int main() {
  UNITY_BEGIN();
  RUN_TEST(test_stopped_idle_persistence_zero);
  RUN_TEST(test_stopped_idle_persistence_three);
  RUN_TEST(test_stopped_idle_persistence_five);
  RUN_TEST(test_stopped_idle_requires_successful_stop_and_no_silence_timeout);
  RUN_TEST(test_stopped_idle_rejects_ambiguous_raw_display_fields);
  RUN_TEST(test_stopped_idle_requires_supported_version_capabilities);
  RUN_TEST(test_stopped_idle_requires_canonical_frame);
  RUN_TEST(test_stopped_idle_preserves_recent_complete_and_partial_positive_rows);
  RUN_TEST(test_stopped_idle_receipt_age_survives_gap_and_dequeue_delay);
  RUN_TEST(test_stopped_idle_rejects_pre_stop_and_during_write_receipts);
  RUN_TEST(test_stopped_idle_partial_frame_keeps_pre_stop_boundary);
  RUN_TEST(test_stopped_idle_long_command_uses_final_successful_write_boundary);
  RUN_TEST(test_stopped_idle_empty_publication_does_not_issue_quiet_commands);
  RUN_TEST(test_stopped_idle_late_old_receipt_cannot_override_new_positive_row);
  RUN_TEST(test_stopped_idle_start_and_session_replacement_revoke_authority);
  RUN_TEST(test_stopped_idle_receipt_and_ingress_rollover);
  RUN_TEST(
      test_connected_phone_without_stop_keeps_existing_start_and_silence_recovery);
  RUN_TEST(test_long_characteristic_stop_recovers_after_complete_forwarding);
  RUN_TEST(test_long_start_releases_phone_stop_and_rejected_start_recovers);
  RUN_TEST(test_new_phone_does_not_inherit_retired_phone_stop);
  RUN_TEST(test_phone_replacement_during_stop_write_cannot_inherit_ownership);
  RUN_TEST(
      test_canonical_queue_rejection_reaches_the_actual_volume_write_receipt);
  RUN_TEST(test_connected_phone_stop_remains_owned_until_departure);
  RUN_TEST(test_stop_recovery_retries_after_busy_and_transient_write_failure);
  RUN_TEST(
      test_phone_stop_then_disconnect_must_recover_detailed_alerts_despite_live_display);
  RUN_TEST(test_no_stop_phone_disconnect_retains_healthy_detailed_stream);
  RUN_TEST(test_stop_recovers_when_all_transport_traffic_becomes_stale);
  return UNITY_END();
}

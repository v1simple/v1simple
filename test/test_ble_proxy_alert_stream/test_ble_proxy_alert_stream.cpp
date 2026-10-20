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
#include "../mocks/modules/alert_persistence/alert_persistence_module.h"
#include "../mocks/modules/power/power_module.h"
#include "../mocks/v1_profiles.h"
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
  ConnectionStateModule connection;
  bool detailedStream = false;
  bool writesSucceed = true;
  unsigned starts = 0, stops = 0;
  uint32_t ingress = 0;
  StreamFixture() {
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
  void deliver(uint8_t id, std::initializer_list<uint8_t> payload) {
    std::vector<uint8_t> p{0xAA, static_cast<uint8_t>(id == 0x65 ? 0xD6 : 0xD8),
                           0xEA, id, static_cast<uint8_t>(payload.size() + 1)};
    p.insert(p.end(), payload);
    uint8_t sum = 0;
    for (uint8_t b : p)
      sum += b;
    p.push_back(sum);
    p.push_back(0xAB);
    TEST_ASSERT_TRUE(queue.tryOnNotify(p.data(), p.size(), 0xB2CE, 1,
                                       mockMillis, ++ingress));
    client.v1NotificationIngressSequence_.store(ingress);
    queue.process();
    TEST_ASSERT_TRUE(queue.consumeParsedFlag());
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
int main() {
  UNITY_BEGIN();
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

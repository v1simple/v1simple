#include <unity.h>

#include <cstdint>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "../mocks/Arduino.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#define private public
#include "../../src/ble_client.h"
#undef private
#include "../../include/config.h"
#include "../../src/packet_parser.cpp"
#include "../../src/packet_parser_alerts.cpp"
#include "../mocks/modules/power/power_module.h"
#include "../mocks/v1_profiles.h"
class DisplayPreviewModule { public: bool isRunning() const { return false; } void cancel() {} };
#include "../../src/modules/ble/ble_queue_module.cpp"

namespace {

std::function<void(const uint8_t*, size_t, uint16_t, uint32_t, uint32_t, uint32_t)> notificationSink;

void captureNotification(const uint8_t* bytes, size_t length, uint16_t characteristic, uint32_t generation,
                         uint32_t now, uint32_t sequence) {
    if (notificationSink) notificationSink(bytes, length, characteristic, generation, now, sequence);
}

void completePendingTask() {
    const auto function = g_mock_task_create_state.lastTaskFunction;
    void* parameter = g_mock_task_create_state.lastTaskParameter;
    g_mock_task_create_state.lastTaskFunction = nullptr;
    if (function) function(parameter);
}

struct CommandFixture {
    V1BLEClient client;
    NimBLEClient link;
    V1BLEClient::ClientCallbacks callbacks;
    NimBLERemoteCharacteristic command{V1_COMMAND_WRITE_UUID};
    NimBLERemoteCharacteristic commandLong{V1_COMMAND_WRITE_LONG_UUID};
    NimBLERemoteCharacteristic display{V1_DISPLAY_DATA_UUID};
    CommandFixture() {
        client.pClient_ = &link;
        client.pCommandChar_ = &command;
        client.pCommandCharLong_ = &commandLong;
        client.bleState_ = BLEState::CONNECTED;
        client.connected_.store(true);
        client.sessionGeneration_.store(1);
        client.acceptClientCallbacks_.store(true);
        client.sessionPublicationGate_.open(1);
        client.dataCallback_ = captureNotification;
        client.onV1DisplayFlowControl(false);
        link.setConnected(true);
        link.setClientCallbacks(&callbacks);
        link.setDisconnectEntryHook([this]() { link.emitDisconnect(0); });
        client.activeConnectionHandle_.store(link.getConnHandle());
        command.setCanWrite(true);
        command.setCanWriteNoResponse(false);
        client.noteV1NotificationIngress();
    }
};

} // namespace

portMUX_TYPE pendingAddrMux = portMUX_INITIALIZER_UNLOCKED;
V1BLEClient* instancePtr = nullptr;

V1BLEClient::V1BLEClient()
    : pClient_(nullptr)
    , pRemoteService_(nullptr)
    , pDisplayDataChar_(nullptr)
    , pCommandChar_(nullptr)
    , pCommandCharLong_(nullptr)
    , pServer_(nullptr)
    , pProxyService_(nullptr)
    , pProxyNotifyChar_(nullptr)
    , pProxyNotifyLongChar_(nullptr)
    , pProxyWriteChar_(nullptr)
    , proxyEnabled_(false)
    , proxyServerInitialized_(false)
    , proxyServerInitAttempted_(false)
    , proxyName_("V1-Proxy")
    , proxyQueue_(nullptr)
    , phone2v1Queue_(nullptr)
    , proxyQueuesInPsram_(false)
    , dataCallback_(nullptr)
    , connectImmediateCallback_(nullptr)
    , sessionOpenedCallback_(nullptr)
    , sessionClosedCallback_(nullptr)
    , connectStableCallback_(nullptr)
    , targetAddress_()
    , lastScanStart_(0)
    , pScanCallbacks_(nullptr)
    , pClientCallbacks_(nullptr)
    , pProxyServerCallbacks_(nullptr)
    , pProxyWriteCallbacks_(nullptr) {
    instancePtr = this;
}

V1BLEClient::~V1BLEClient() {
    releaseProxyQueues();
}

void V1BLEClient::setBLEState(BLEState newState) {
    bleState_ = newState;
    stateEnteredMs_ = static_cast<uint32_t>(millis());
    scanStopResultsCleared_ = false;
}

void V1BLEClient::disconnect() {
    connected_.store(false, std::memory_order_relaxed);
}

void V1BLEClient::hardResetBLEClient() {
    connectInProgress_ = false;
    connectStartMs_ = 0;
    setBLEState(BLEState::DISCONNECTED);
}

void V1BLEClient::completeHardResetBLEClient() {}

void V1BLEClient::setProxyClientConnected(bool connectedState) {
    proxyClientConnected_ = connectedState;
    if (connectedState) {
        proxyClientConnectedOnceThisBoot_ = true;
        proxyFastAdvertisingUntilMs_ = 0;
    }
}

bool V1BLEClient::isConnected() { return connected_.load() && pClient_ && pClient_->isConnected(); }
int V1BLEClient::enqueueCurrentBondBackupSnapshot() { return 0; }
#include "../../src/ble_proxy.cpp"
#include "../../src/ble_connection.cpp"
#ifndef BLE_COMMAND_SOURCE
#define BLE_COMMAND_SOURCE "../../src/ble_commands.cpp"
#endif
#include BLE_COMMAND_SOURCE
#include "../../src/ble_connected_followup.cpp"

#ifndef BLE_SUBSCRIPTION_SOURCE
#define BLE_SUBSCRIPTION_SOURCE "../../src/ble_subscription.cpp"
#endif
#include BLE_SUBSCRIPTION_SOURCE
#include "../mocks/settings.h"
#include "../../src/modules/speed_mute/speed_mute_module.cpp"
#include "../../src/modules/volume_fade/volume_fade_module.cpp"
#include "../../src/modules/quiet/quiet_coordinator_module.cpp"
#include "../../src/modules/quiet/quiet_coordinator_templates.h"

bool V1BLEClient::isProxyClientConnected() { return proxyClientConnected_.load(); }

void setUp() {
    mockMillis = 100;
    mockMicros = 0;
    mock_reset_task_create_state();
    mock_reset_task_delete_state();
    mock_reset_nimble_state();
    mock_reset_semaphore_state();
    notificationSink = nullptr;
}
void tearDown() {}

namespace {
class HeldCommand {
  public:
    explicit HeldCommand(NimBLERemoteCharacteristic& characteristic) {
        characteristic.setWriteEntryHook([this]() {
            std::unique_lock<std::mutex> lock(mutex_);
            entered_ = true;
            condition_.notify_all();
            expired_ = !condition_.wait_for(lock, std::chrono::seconds(2), [this]() { return released_; });
        });
    }

    bool start() {
        const auto function = g_mock_task_create_state.lastTaskFunction;
        void* parameter = g_mock_task_create_state.lastTaskParameter;
        g_mock_task_create_state.lastTaskFunction = nullptr;
        if (!function) return false;
        worker_ = std::thread([function, parameter]() { function(parameter); });
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(lock, std::chrono::seconds(2), [this]() { return entered_; });
    }

    void release() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            released_ = true;
        }
        condition_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

    bool expired() const { return expired_; } // Read only after join.
    ~HeldCommand() { release(); }

  private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool entered_ = false;
    bool released_ = false;
    bool expired_ = false;
    std::thread worker_;
};

const uint8_t muteOn[] = {0xAA, 0xDA, 0xE6, 0x34, 0x01, 0x9F, 0xAB};
const uint8_t muteOff[] = {0xAA, 0xDA, 0xE6, 0x35, 0x01, 0xA0, 0xAB};
SendResult sendMute(CommandFixture& f) {
    V1BLEClient::CommandScope scope(f.client, V1CommandOwner::TapGesture);
    return f.client.sendCommandWithResult(muteOn, sizeof(muteOn));
}
}

void test_delayed_ack_keeps_notifications_and_loop_work_responsive() {
    CommandFixture f;
    HeldCommand held(f.command);
    const auto begin = std::chrono::steady_clock::now();
    const auto admitted = sendMute(f);
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    // An old inline writer is released by the bounded latch timeout. Release
    // subsequent calls immediately so regression-before runs cannot hang.
    if (elapsed >= std::chrono::milliseconds(100)) held.release();
    const bool started = held.start();
    PacketParser parser;
    BleQueueModule queue;
    const bool queueReady = queue.begin(&f.client, &parser, nullptr, nullptr, nullptr);
    queue.openSession(1);
    // Real notification callback -> ingress queue -> production parser continues
    // while the actual command task is parked inside writeValue().
    f.client.notifyShortChar_.store(&f.display);
    f.client.notifyShortCharId_.store(0xB2CE);
    notificationSink = [&](const uint8_t* data, size_t length, uint16_t id, uint32_t generation,
                           uint32_t now, uint32_t ingress) {
        queue.onNotify(data, length, id, generation, now, ingress);
    };
    uint8_t version[] = {0xAA,0xD6,0xEA,0x02,0x08,'v','4','.','1','0','3','9',0,0xAB};
    for (size_t i=0;i<sizeof(version)-2;++i) version[sizeof(version)-2] += version[i];
    bool stayedPending = true;
    for (unsigned i=0;i<8;++i) {
        mockMillis += 10;
        V1BLEClient::notifyCallback(&f.display, version, sizeof(version), true);
        queue.process();
        f.client.serviceCommandWrite();
        stayedPending &= sendMute(f) == SendResult::IN_FLIGHT;
    }
    held.release();
    const auto completed = sendMute(f);
    notificationSink = nullptr;
    queue.end();
    TEST_ASSERT_TRUE(queueReady);
    const auto elapsedMicros = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
    printf("COMMAND_ADMISSION_US=%lld\n", static_cast<long long>(elapsedMicros));
    TEST_ASSERT_LESS_THAN_INT64(100000, elapsedMicros);
    TEST_ASSERT_TRUE(started);
    TEST_ASSERT_FALSE(held.expired());
    TEST_ASSERT_EQUAL(SendResult::IN_FLIGHT, admitted);
    TEST_ASSERT_TRUE(stayedPending);
    TEST_ASSERT_EQUAL_UINT32(8, f.client.latestV1NotificationIngressSequence()-1);
    TEST_ASSERT_TRUE(f.client.hasV1FirmwareVersion());
    TEST_ASSERT_EQUAL(SendResult::SENT, completed);
    TEST_ASSERT_TRUE(f.command.lastWriteResponse());
    TEST_ASSERT_EQUAL_UINT32(1, f.command.writeValueCalls());
    TEST_ASSERT_FALSE(f.client.commandInFlight(V1CommandOwner::TapGesture));
    TEST_ASSERT_EQUAL_UINT32(1, f.client.commandIngressBoundary(V1CommandOwner::TapGesture));
}

void test_immediate_worker_success_still_requires_one_owner_harvest() {
    CommandFixture f;
    TEST_ASSERT_EQUAL(SendResult::IN_FLIGHT, sendMute(f));
    completePendingTask();
    TEST_ASSERT_TRUE(f.client.commandInFlight(V1CommandOwner::TapGesture));
    { V1BLEClient::CommandScope scope(f.client, V1CommandOwner::AutoPush);
      TEST_ASSERT_EQUAL(SendResult::NOT_YET, f.client.sendCommandWithResult(muteOff,sizeof(muteOff))); }
    TEST_ASSERT_EQUAL(SendResult::SENT, sendMute(f));
    TEST_ASSERT_EQUAL_UINT32(1, f.command.writeValueCalls());
}

void test_timeout_pins_attributes_until_worker_exits_and_rejects_late_success() {
    CommandFixture f;
    HeldCommand held(f.command);
    const auto admitted = sendMute(f);
    const bool started = held.start();
    mockMillis += 1000;
    f.client.serviceCommandWrite();
    f.client.processClientQuiesce();
    const bool pinned = f.client.bleState_ == BLEState::QUIESCING && f.client.pCommandChar_ == &f.command;
    const auto retired = sendMute(f);
    held.release();
    f.client.processClientQuiesce();
    TEST_ASSERT_EQUAL(SendResult::IN_FLIGHT, admitted);
    TEST_ASSERT_TRUE(started);
    TEST_ASSERT_TRUE(pinned);
    TEST_ASSERT_EQUAL(SendResult::FAILED, retired);
    TEST_ASSERT_EQUAL(BLEState::DISCONNECTED, f.client.bleState_);
    TEST_ASSERT_NULL(f.client.pCommandChar_);
    TEST_ASSERT_FALSE(f.client.commandInFlight(V1CommandOwner::TapGesture));
    TEST_ASSERT_EQUAL_UINT32(0, f.client.commandIngressBoundary(V1CommandOwner::TapGesture));
    TEST_ASSERT_EQUAL_UINT32(1, f.command.writeValueCalls());
}

void test_cancel_before_worker_start_prevents_wire_write() {
    CommandFixture f;
    TEST_ASSERT_EQUAL(SendResult::IN_FLIGHT, sendMute(f));
    f.client.cancelCommand(V1CommandOwner::TapGesture);
    f.client.processClientQuiesce();
    TEST_ASSERT_NOT_NULL(f.client.pCommandChar_);
    completePendingTask();
    f.client.processClientQuiesce();
    TEST_ASSERT_EQUAL_UINT32(0, f.command.writeValueCalls());
    TEST_ASSERT_NULL(f.client.pCommandChar_);
}

void test_changed_same_owner_intent_retires_admitted_command() {
    CommandFixture f;
    TEST_ASSERT_EQUAL(SendResult::IN_FLIGHT, sendMute(f));
    { V1BLEClient::CommandScope scope(f.client, V1CommandOwner::TapGesture);
      TEST_ASSERT_EQUAL(SendResult::FAILED, f.client.sendCommandWithResult(muteOff,sizeof(muteOff))); }
    completePendingTask();
    TEST_ASSERT_EQUAL_UINT32(0, f.command.writeValueCalls());
    TEST_ASSERT_EQUAL(BLEState::QUIESCING,f.client.bleState_);
}

void test_failure_retires_session_without_reissuing_ambiguous_command() {
    CommandFixture f;
    f.command.setWriteValueResult(false);
    TEST_ASSERT_EQUAL(SendResult::IN_FLIGHT, sendMute(f));
    completePendingTask();
    TEST_ASSERT_EQUAL(SendResult::FAILED, sendMute(f));
    TEST_ASSERT_EQUAL(SendResult::FAILED, sendMute(f));
    TEST_ASSERT_EQUAL_UINT32(1,f.command.writeValueCalls());
    TEST_ASSERT_EQUAL(BLEState::QUIESCING,f.client.bleState_);
}

void test_task_launch_failure_does_not_pin_or_write() {
    CommandFixture f;
    g_mock_task_create_state.failCaps = true;
    TEST_ASSERT_EQUAL(SendResult::FAILED, sendMute(f));
    TEST_ASSERT_FALSE(f.client.discoveryTaskRunning_.load());
    TEST_ASSERT_FALSE(f.client.commandInFlight(V1CommandOwner::TapGesture));
    TEST_ASSERT_EQUAL_UINT32(0,f.command.writeValueCalls());
    TEST_ASSERT_TRUE(f.client.isConnected());
}

void test_dual_mode_preserves_ack_and_small_no_response_stays_immediate() {
    CommandFixture f;
    f.command.setCanWriteNoResponse(true);
    TEST_ASSERT_EQUAL(SendResult::IN_FLIGHT,sendMute(f));
    completePendingTask();
    TEST_ASSERT_EQUAL(SendResult::SENT,sendMute(f));
    TEST_ASSERT_TRUE(f.command.lastWriteResponse());
    f.command.setCanWrite(false);
    mockMillis += 5;
    TEST_ASSERT_EQUAL(SendResult::SENT,sendMute(f));
    TEST_ASSERT_FALSE(f.command.lastWriteResponse());
    TEST_ASSERT_EQUAL_UINT32(1,g_mock_task_create_state.capsCalls);
}

void test_oversize_no_response_uses_worker_at_negotiated_mtu_boundary() {
    CommandFixture f;
    f.command.setCanWrite(false);
    f.command.setCanWriteNoResponse(true);
    uint8_t bytes[21] = {};
    TEST_ASSERT_EQUAL(SendResult::SENT,f.client.sendCommandWithResult(bytes,20));
    mockMillis += 5;
    TEST_ASSERT_EQUAL(SendResult::IN_FLIGHT,f.client.sendCommandWithResult(bytes,21));
    completePendingTask();
    TEST_ASSERT_EQUAL(SendResult::SENT,f.client.sendCommandWithResult(bytes,21));
    TEST_ASSERT_FALSE(f.command.lastWriteResponse());
    mockMillis += 5;
    f.link.setMTU(24);
    TEST_ASSERT_EQUAL(SendResult::SENT,f.client.sendCommandWithResult(bytes,21));
    TEST_ASSERT_EQUAL_UINT32(1,g_mock_task_create_state.capsCalls);
}

void test_early_volume_rejection_survives_ack_receipt_publication() {
    CommandFixture f;
    HeldCommand held(f.command);
    V1BLEClient::CommandScope scope(f.client,V1CommandOwner::VolumeFade);
    const auto admitted=f.client.setVolumeResult(7,2);
    const bool started=held.start();
    f.client.onV1RequestNotProcessed(PACKET_ID_REQ_WRITE_VOLUME,f.client.noteV1NotificationIngress());
    const uint32_t before=f.client.volumeWriteRevision();
    held.release();
    const auto completed=f.client.setVolumeResult(7,2);
    TEST_ASSERT_EQUAL(SendResult::IN_FLIGHT,admitted);
    TEST_ASSERT_TRUE(started);
    TEST_ASSERT_EQUAL_UINT32(0,before);
    TEST_ASSERT_EQUAL(SendResult::SENT,completed);
    TEST_ASSERT_TRUE(f.client.volumeWriteRejected(f.client.volumeWriteRevision()));
    TEST_ASSERT_EQUAL_UINT32(1,f.command.writeValueCalls());
}

void test_followup_accepts_early_reply_only_after_actual_worker_boundary() {
    CommandFixture f;
    f.client.connectedFollowupStep_=V1BLEClient::ConnectedFollowupStep::REQUEST_USER_BYTES;
    f.client.connectedFollowupSendDeadlineMs_=1600;
    HeldCommand held(f.command);
    f.client.processConnectedFollowup();
    const uint8_t bytes[]={1,2,3,4,5,6};
    const uint32_t stale=f.client.noteV1NotificationIngress();
    f.client.onUserBytesReceived(bytes,stale);
    const bool acceptedBeforeStart=f.client.hasSessionUserBytes_;
    const bool started=held.start();
    f.client.onUserBytesReceived(bytes,stale);
    const bool acceptedAtBoundary=f.client.hasSessionUserBytes_;
    f.client.onUserBytesReceived(bytes,f.client.noteV1NotificationIngress());
    f.client.processConnectedFollowup();
    held.release();
    f.client.processConnectedFollowup();
    TEST_ASSERT_FALSE(acceptedBeforeStart);
    TEST_ASSERT_FALSE(acceptedAtBoundary);
    TEST_ASSERT_TRUE(started);
    TEST_ASSERT_TRUE(f.client.hasSessionUserBytes_);
    TEST_ASSERT_EQUAL_UINT32(1,f.client.sessionUserBytesRevision_);
    TEST_ASSERT_EQUAL_UINT32(stale,f.client.sessionUserBytesIngressBoundary_);
    TEST_ASSERT_EQUAL(V1BLEClient::ConnectedFollowupStep::WAIT_SETTINGS_SNAPSHOT,f.client.connectedFollowupStep_);
    TEST_ASSERT_EQUAL_UINT32(1,f.command.writeValueCalls());
}


void test_completed_but_abandoned_command_has_bounded_ownership() {
    CommandFixture f;
    TEST_ASSERT_EQUAL(SendResult::IN_FLIGHT,sendMute(f));
    completePendingTask();
    mockMillis+=1000;
    f.client.serviceCommandWrite();
    TEST_ASSERT_EQUAL(BLEState::QUIESCING,f.client.bleState_);
    TEST_ASSERT_EQUAL(SendResult::FAILED,sendMute(f));
    TEST_ASSERT_EQUAL_UINT32(1,f.command.writeValueCalls());
}

void test_stale_completed_volume_and_start_do_not_publish_new_session_receipts() {
    for(bool volume : {true,false}) {
        CommandFixture f;
        HeldCommand held(f.command);
        V1BLEClient::CommandScope scope(f.client,V1CommandOwner::AutoPush);
        const bool admitted=volume ? f.client.setVolumeResult(7,2)==SendResult::IN_FLIGHT : !f.client.requestAlertData();
        const bool started=held.start();
        f.client.beginClientQuiesce();
        const uint32_t retiredGeneration=f.client.sessionGeneration();
        held.release();
        const bool sent=volume ? f.client.setVolumeResult(7,2)==SendResult::SENT : f.client.requestAlertData();
        TEST_ASSERT_TRUE(admitted);
        TEST_ASSERT_TRUE(started);
        TEST_ASSERT_FALSE(sent);
        TEST_ASSERT_EQUAL_UINT32(0,f.client.volumeWriteRevision());
        TEST_ASSERT_FALSE(f.client.alertDataRequestGate_.hasSuccessfulSend(retiredGeneration));
        TEST_ASSERT_EQUAL_UINT32(1,f.command.writeValueCalls());
    }
}

void test_early_start_rejection_is_not_erased_by_late_ack() {
    CommandFixture f;
    HeldCommand held(f.command);
    V1BLEClient::CommandScope scope(f.client,V1CommandOwner::AlertStreamRecovery);
    const bool initial=f.client.requestAlertData();
    const bool started=held.start();
    f.client.onV1RequestNotProcessed(PACKET_ID_REQ_START_ALERT,f.client.noteV1NotificationIngress());
    held.release();
    const bool completed=f.client.requestAlertData();
    TEST_ASSERT_FALSE(initial);
    TEST_ASSERT_TRUE(started);
    TEST_ASSERT_TRUE(completed);
    TEST_ASSERT_FALSE(f.client.alertDataRequestGate_.hasSuccessfulSend(1));
    TEST_ASSERT_EQUAL_UINT32(1,f.command.writeValueCalls());
}

void test_sweep_capture_uses_worker_boundary_and_survives_ack_harvest() {
    CommandFixture f;
    V1BLEClient::CommandScope scope(f.client,V1CommandOwner::AutoPush);
    HeldCommand held(f.command);
    const auto result=f.client.requestSweepSectionsResult();
    f.client.beginSessionSweepSectionsCapture(f.client.latestV1NotificationIngressSequence());
    const uint32_t stale=f.client.noteV1NotificationIngress();
    const bool beforeStart=f.client.sessionSweepResponseEligible(0x23,stale);
    const bool started=held.start();
    const bool atBoundary=f.client.sessionSweepResponseEligible(0x23,stale);
    const uint32_t fresh=f.client.noteV1NotificationIngress();
    const bool beforeAck=f.client.sessionSweepResponseEligible(0x23,fresh);
    f.client.hasSessionSweepSections_=true;
    held.release();
    const auto sent=f.client.requestSweepSectionsResult();
    TEST_ASSERT_EQUAL(SendResult::IN_FLIGHT,result);
    TEST_ASSERT_TRUE(started);
    TEST_ASSERT_FALSE(beforeStart);
    TEST_ASSERT_FALSE(atBoundary);
    TEST_ASSERT_TRUE(beforeAck);
    TEST_ASSERT_EQUAL(SendResult::SENT,sent);
    TEST_ASSERT_TRUE(f.client.hasSessionSweepSections_);
    TEST_ASSERT_EQUAL_UINT32(stale,f.client.sessionSweepIngressBoundary(0x23));
    TEST_ASSERT_FALSE(f.client.sessionSweepResponseEligible(0x23,stale));
    TEST_ASSERT_TRUE(f.client.sessionSweepResponseEligible(0x23,fresh));
}

namespace {
void preparePhone(CommandFixture& f) {
    TEST_ASSERT_TRUE(f.client.allocateProxyQueues());
    f.client.phoneCmdMutex_=xSemaphoreCreateMutex();
    f.client.proxyPhoneSessionGeneration_.store(1);
    f.client.phoneCmdPendingClear_.store(true);
}
std::vector<uint8_t> rawLongCommand() {
    std::vector<uint8_t> frame(21,0);
    frame[0]=0xAA; frame[1]=0xDA; frame[2]=0xE6; frame[3]=0x55; frame[4]=15;
    frame.back()=0xAB;
    for(size_t i=0;i<frame.size()-2;++i) frame[frame.size()-2]+=frame[i];
    return frame;
}
}

void test_phone_raw_long_frame_waits_without_reorder_or_duplicate() {
    CommandFixture f;
    preparePhone(f);
    auto frame=rawLongCommand();
    TEST_ASSERT_TRUE(f.client.enqueuePhoneCommand(frame.data(),frame.size(),V1_SHORT_UUID_COMMAND_LONG));
    TEST_ASSERT_TRUE(f.client.enqueuePhoneCommand(muteOn,sizeof(muteOn),0xB6D4));
    HeldCommand held(f.commandLong);
    const int admitted=f.client.processPhoneCommandQueue();
    const bool started=held.start();
    for(int i=0;i<5;++i) f.client.processPhoneCommandQueue();
    const uint32_t shortWrites=f.command.writeValueCalls();
    held.release();
    const int longSent=f.client.processPhoneCommandQueue();
    mockMillis+=5;
    const int shortAdmitted=f.client.processPhoneCommandQueue();
    completePendingTask();
    const int shortSent=f.client.processPhoneCommandQueue();
    TEST_ASSERT_EQUAL_INT(0,admitted);
    TEST_ASSERT_TRUE(started);
    TEST_ASSERT_EQUAL_UINT32(0,shortWrites);
    TEST_ASSERT_EQUAL_INT(1,longSent);
    TEST_ASSERT_EQUAL_INT(0,shortAdmitted);
    TEST_ASSERT_EQUAL_INT(1,shortSent);
    TEST_ASSERT_EQUAL_UINT32(1,f.commandLong.writeValueCalls());
    TEST_ASSERT_EQUAL_UINT32(1,f.command.writeValueCalls());
    TEST_ASSERT_FALSE(f.commandLong.lastWriteResponse());
    TEST_ASSERT_TRUE(f.command.lastWriteResponse());
}

void test_phone_epoch_retirement_prevents_pending_worker_write() {
    CommandFixture f;
    preparePhone(f);
    auto frame=rawLongCommand();
    TEST_ASSERT_TRUE(f.client.enqueuePhoneCommand(frame.data(),frame.size(),V1_SHORT_UUID_COMMAND_LONG));
    TEST_ASSERT_EQUAL_INT(0,f.client.processPhoneCommandQueue());
    f.client.proxyPhoneSessionGeneration_.store(2);
    f.client.serviceCommandWrite();
    completePendingTask();
    TEST_ASSERT_EQUAL_UINT32(0,f.commandLong.writeValueCalls());
    TEST_ASSERT_EQUAL(BLEState::QUIESCING,f.client.bleState_);
}

void test_indexed_phone_chunks_keep_exact_order_on_safe_no_response_path() {
    CommandFixture f;
    preparePhone(f);
    const uint8_t first[]={0x12,0xAA,0xDA,0xE6,0x34};
    const uint8_t second[]={0x22,0x01,0x9F,0xAB};
    TEST_ASSERT_TRUE(f.client.enqueuePhoneCommand(first,sizeof(first),V1_SHORT_UUID_COMMAND_LONG));
    TEST_ASSERT_TRUE(f.client.enqueuePhoneCommand(second,sizeof(second),V1_SHORT_UUID_COMMAND_LONG));
    TEST_ASSERT_EQUAL_INT(1,f.client.processPhoneCommandQueue());
    TEST_ASSERT_EQUAL_UINT8_ARRAY(first,f.commandLong.lastWriteValue().data(),sizeof(first));
    TEST_ASSERT_EQUAL_INT(1,f.client.processPhoneCommandQueue());
    TEST_ASSERT_EQUAL_UINT8_ARRAY(second,f.commandLong.lastWriteValue().data(),sizeof(second));
    TEST_ASSERT_EQUAL_UINT32(2,f.commandLong.writeValueCalls());
    TEST_ASSERT_EQUAL_UINT32(0,g_mock_task_create_state.capsCalls);
}

// Real speed/quiet/parser/command/quiesce owners. Only radio, worker scheduling,
// clocks and settings storage are modeled. A 20 -> 30 mph threshold crossing
// restores the captured 6/2 pair; overlapping identical retries must be harvested.
struct SpeedTransport {
    CommandFixture transport;
    PacketParser parser;
    SettingsManager settings;
    SpeedMuteModule speed;
    VolumeFadeModule fade;
    QuietCoordinatorModule quiet;
    SpeedTransport() {
        settings.settings.alertVolumeFadeEnabled = false;
        fade.begin(&settings);
        quiet.begin(&transport.client, &parser);
        quiet.beginDetectorVolumeSession("AA:BB:CC:DD:EE:01", &fade);
        speed.begin(true, 25, 3, 0, true);
    }
    void volume(uint8_t main, uint8_t muted, uint32_t at) {
        mockMillis = at;
        uint8_t packet[] = {0xAA,0xD6,0xEA,0x3D,5,main,muted,main,muted,0,0xAB};
        for (size_t i=0;i<sizeof(packet)-2;++i) packet[sizeof(packet)-2] += packet[i];
        TEST_ASSERT_TRUE(parser.parse(packet,sizeof(packet),at,transport.client.noteV1NotificationIngress()));
    }
    void tick(uint32_t at, float mph) {
        mockMillis = at;
        transport.client.serviceCommandWrite();
        speed.update(mph,true,at);
        quiet.processSpeedVolume(at,speed,&fade);
    }
    void finishWorker(uint32_t at) {
        mockMillis = at;
        TEST_ASSERT_NOT_NULL(g_mock_task_create_state.lastTaskFunction);
        completePendingTask();
        TEST_ASSERT_TRUE(transport.client.commandWorkerResult_.load());
        TEST_ASSERT_FALSE(transport.client.discoveryTaskRunning_.load());
    }
    void initialRestore() {
        volume(6,2,10000);
        tick(10000,20);
        TEST_ASSERT_TRUE(transport.client.commandInFlight(V1CommandOwner::SpeedVolume));
        finishWorker(10001);
        volume(0,2,10050);
        tick(10075,20);
        TEST_ASSERT_FALSE(transport.client.commandInFlight(V1CommandOwner::SpeedVolume));
        tick(12000,30);
        TEST_ASSERT_TRUE(transport.client.commandInFlight(V1CommandOwner::SpeedVolume));
        finishWorker(12001);
        tick(12075,30);
        TEST_ASSERT_FALSE(transport.client.commandInFlight(V1CommandOwner::SpeedVolume));
        TEST_ASSERT_TRUE(transport.client.isConnected());
        TEST_ASSERT_EQUAL_UINT8(6,transport.command.lastWriteValue()[5]);
        TEST_ASSERT_EQUAL_UINT8(2,transport.command.lastWriteValue()[6]);
    }
};

void test_restore_echo_before_retry_keeps_connection() {
    SpeedTransport f;
    f.initialRestore();
    f.volume(6,2,12090);
    f.tick(12090,30);
    TEST_ASSERT_TRUE(f.transport.client.isConnected());
    TEST_ASSERT_TRUE(f.quiet.canApplyAutoPushVolumeExactly());
}
void test_restore_echo_after_retry_harvest_keeps_connection() {
    SpeedTransport f;
    f.initialRestore();
    f.tick(12150,30);
    f.finishWorker(12151);
    f.tick(12225,30);
    f.volume(6,2,12240);
    f.tick(12240,30);
    TEST_ASSERT_TRUE(f.transport.client.isConnected());
    TEST_ASSERT_TRUE(f.quiet.canApplyAutoPushVolumeExactly());
}
void test_first_restore_echo_before_ack_harvest_keeps_connection() {
    SpeedTransport f;
    f.volume(6,2,10000);
    f.tick(10000,20);
    f.finishWorker(10001);
    f.volume(0,2,10050);
    f.tick(10075,20);
    f.tick(12000,30);
    f.finishWorker(12001);
    f.volume(6,2,12010);
    f.tick(12010,30);
    TEST_ASSERT_TRUE(f.transport.client.isConnected());
    TEST_ASSERT_TRUE(f.transport.client.commandInFlight(V1CommandOwner::SpeedVolume));
    f.tick(12075,30);
    f.tick(12076,30);
    TEST_ASSERT_TRUE(f.transport.client.isConnected());
    TEST_ASSERT_FALSE(f.transport.client.commandInFlight(V1CommandOwner::SpeedVolume));
    TEST_ASSERT_TRUE(f.quiet.canApplyAutoPushVolumeExactly());
    TEST_ASSERT_EQUAL_UINT32(2,f.transport.command.writeValueCalls());
}
enum class RetryState { Pending, Running, Completed };
void assertOverlappingEcho(RetryState state) {
    SpeedTransport f;
    f.initialRestore();
    HeldCommand held(f.transport.command);
    f.tick(12150,30);
    TEST_ASSERT_TRUE(f.transport.client.commandInFlight(V1CommandOwner::SpeedVolume));
    bool started = true;
    if (state == RetryState::Running) started = held.start();
    if (state == RetryState::Completed) { held.release(); f.finishWorker(12151); }
    f.volume(6,2,12160);
    f.tick(12160,30);
    const bool connectedAtEcho = f.transport.client.isConnected();
    const uint32_t sessionAtEcho = f.transport.client.sessionGeneration();
    // Release before any assertion: Unity longjmp must never strand a worker.
    held.release();
    TEST_ASSERT_TRUE(started);
    printf("RETRY=%u CONNECTED=%d SESSION=%u BLE_STATE=%u WRITES=%u\n", (unsigned)state,
        connectedAtEcho,sessionAtEcho,(unsigned)f.transport.client.getBLEState(),
        (unsigned)f.transport.command.writeValueCalls());
    TEST_ASSERT_TRUE_MESSAGE(connectedAtEcho, "Canonical confirmation of successful same-target restoration disconnected detector");
    TEST_ASSERT_EQUAL_UINT32(1, sessionAtEcho);
    TEST_ASSERT_TRUE(f.transport.client.commandInFlight(V1CommandOwner::SpeedVolume));
    TEST_ASSERT_FALSE(f.quiet.canApplyAutoPushVolumeExactly());
    if (state == RetryState::Pending) f.finishWorker(12161);
    f.tick(12225,30); // Harvest the exact admitted retry.
    f.tick(12226,30); // Then retire the confirmed restore obligation.
    TEST_ASSERT_TRUE(f.transport.client.isConnected());
    TEST_ASSERT_FALSE(f.transport.client.commandInFlight(V1CommandOwner::SpeedVolume));
    TEST_ASSERT_TRUE(f.quiet.canApplyAutoPushVolumeExactly());
    TEST_ASSERT_EQUAL_UINT32(3, f.transport.command.writeValueCalls());
    TEST_ASSERT_EQUAL_UINT8(6,f.transport.command.lastWriteValue()[5]);
    TEST_ASSERT_EQUAL_UINT8(2,f.transport.command.lastWriteValue()[6]);
    f.tick(12400,30);
    TEST_ASSERT_EQUAL_UINT32(3, f.transport.command.writeValueCalls());
    V1BLEClient::CommandScope next(f.transport.client,V1CommandOwner::AutoPush);
    TEST_ASSERT_EQUAL(SendResult::IN_FLIGHT, f.transport.client.requestAllVolumeResult());
    f.finishWorker(12401);
    TEST_ASSERT_EQUAL(SendResult::SENT, f.transport.client.requestAllVolumeResult());
}
void test_target_echo_with_pending_identical_retry_keeps_connection() { assertOverlappingEcho(RetryState::Pending); }
void test_target_echo_with_running_identical_retry_keeps_connection() { assertOverlappingEcho(RetryState::Running); }
void test_target_echo_with_completed_identical_retry_keeps_connection() { assertOverlappingEcho(RetryState::Completed); }
void test_new_manual_pair_still_cancels_pending_restore_retry() {
    SpeedTransport f;
    f.initialRestore();
    f.tick(12150,30);
    f.volume(3,4,12160);
    f.tick(12160,30);
    TEST_ASSERT_FALSE(f.transport.client.isConnected());
    TEST_ASSERT_EQUAL(BLEState::QUIESCING, f.transport.client.getBLEState());
    TEST_ASSERT_EQUAL_UINT32(2, f.transport.command.writeValueCalls());
}
void test_same_target_confirmation_does_not_hide_blocked_retry_timeout() {
    SpeedTransport f;
    f.initialRestore();
    HeldCommand held(f.transport.command);
    f.tick(12150,30);
    const bool started=held.start();
    f.volume(6,2,12160);
    f.tick(12160,30);
    f.tick(13149,30);
    const bool connectedBeforeTimeout = f.transport.client.isConnected();
    f.tick(13150,30);
    const bool disconnected = !f.transport.client.isConnected();
    const auto state = f.transport.client.getBLEState();
    held.release();
    TEST_ASSERT_TRUE(started);
    TEST_ASSERT_TRUE(connectedBeforeTimeout);
    TEST_ASSERT_TRUE(disconnected);
    TEST_ASSERT_EQUAL(BLEState::QUIESCING, state);
}

// Exercise the other automatic audio owners against the same real transport.
// A receipt may precede a retry's start, arrive during its ACK wait, or arrive
// after its worker completed. In each case, the owner must still harvest it.
struct QuietPolicyTransport : SpeedTransport {
    void packet(uint8_t id, std::initializer_list<uint8_t> payload, uint32_t at) {
        mockMillis = at;
        std::vector<uint8_t> bytes{0xAA, 0xD8, 0xEA, id, static_cast<uint8_t>(payload.size() + 1)};
        bytes.insert(bytes.end(), payload.begin(), payload.end());
        uint8_t sum = 0;
        for (uint8_t b : bytes) sum += b;
        bytes.push_back(sum);
        bytes.push_back(0xAB);
        TEST_ASSERT_TRUE(parser.parse(bytes.data(), bytes.size(), at, transport.client.noteV1NotificationIngress()));
    }
    void alert(bool live, uint32_t at, uint16_t frequency = 34700, uint8_t band = 0x22) {
        if (live) packet(0x43, {0x11, static_cast<uint8_t>(frequency >> 8), static_cast<uint8_t>(frequency),
                               0xAC, 0, band, 0x80}, at);
        else packet(0x43, {0, 0, 0, 0, 0, 0, 0}, at);
        TEST_ASSERT_EQUAL(live, parser.hasAlerts());
    }
    void displayMute(bool muted, uint32_t at) {
        packet(0x31, {0x06, 0x06, 4, 0x24, 0x24, static_cast<uint8_t>(0x0C | (muted ? 1 : 0)), 1, 0x62}, at);
        TEST_ASSERT_EQUAL(muted, parser.getDisplayState().softMuted);
    }
    void fadeTick(uint32_t at) {
        mockMillis = at;
        transport.client.serviceCommandWrite();
        quiet.executeVolumeFade(at, &fade);
    }
    void boxTick(uint32_t at) {
        mockMillis = at;
        transport.client.serviceCommandWrite();
        quiet.processInTheBox(at);
    }
    void beginFadeRestore(bool newThreat) {
        settings.settings.alertVolumeFadeEnabled = true;
        settings.settings.alertVolumeFadeDelaySec = 2;
        settings.settings.alertVolumeFadeVolume = 1;
        volume(6, 2, 10000);
        alert(true, 10000);
        fadeTick(10000);
        alert(true, 12000);
        fadeTick(12000);
        finishWorker(12001);
        volume(1, 2, 12050);
        fadeTick(12075);
        TEST_ASSERT_FALSE(transport.client.commandInFlight(V1CommandOwner::VolumeFade));
        alert(newThreat, 14000, 35500);
        fadeTick(14000);
        finishWorker(14001);
        fadeTick(14075);
        TEST_ASSERT_FALSE(transport.client.commandInFlight(V1CommandOwner::VolumeFade));
        TEST_ASSERT_TRUE(transport.client.isConnected());
        TEST_ASSERT_EQUAL_UINT8(6, transport.command.lastWriteValue()[5]);
    }
    uint32_t beginBoxRetry(bool unmute) {
        V1InTheBoxSettings policy;
        policy.bands[2].muteOutside = true;
        quiet.setInTheBoxSettings(policy, 1);
        displayMute(false, 10000);
        alert(true, 10000, 24040, 0x24);
        boxTick(10000);
        finishWorker(10001);
        boxTick(10100);
        TEST_ASSERT_FALSE(transport.client.commandInFlight(V1CommandOwner::InTheBox));
        TEST_ASSERT_EQUAL_UINT8(0x34, transport.command.lastWriteValue()[3]);
        if (!unmute) return 10200;
        displayMute(true, 10110);
        boxTick(10110);
        alert(true, 10300, 24150, 0x24);
        boxTick(10300);
        finishWorker(10301);
        boxTick(10400);
        TEST_ASSERT_FALSE(transport.client.commandInFlight(V1CommandOwner::InTheBox));
        TEST_ASSERT_EQUAL_UINT8(0x35, transport.command.lastWriteValue()[3]);
        return 10500;
    }
    void assertNextOwnerCanSend(uint32_t at) {
        mockMillis = at;
        V1BLEClient::CommandScope scope(transport.client, V1CommandOwner::AutoPush);
        TEST_ASSERT_EQUAL(SendResult::IN_FLIGHT, transport.client.requestAllVolumeResult());
        finishWorker(at + 1);
        TEST_ASSERT_EQUAL(SendResult::SENT, transport.client.requestAllVolumeResult());
    }
};

void assertFadeConfirmationOverlap(bool newThreat, RetryState state) {
    QuietPolicyTransport f;
    f.beginFadeRestore(newThreat);
    HeldCommand held(f.transport.command);
    f.fadeTick(14150);
    TEST_ASSERT_TRUE(f.transport.client.commandInFlight(V1CommandOwner::VolumeFade));
    bool started = true;
    if (state == RetryState::Running) started = held.start();
    if (state == RetryState::Completed) { held.release(); f.finishWorker(14151); }
    f.volume(6, 2, 14160);
    f.fadeTick(14160);
    const bool connectedAtEcho = f.transport.client.isConnected();
    held.release();
    TEST_ASSERT_TRUE(started);
    TEST_ASSERT_TRUE_MESSAGE(connectedAtEcho, "Fade restore confirmation disconnected the detector");
    if (state == RetryState::Pending) f.finishWorker(14161);
    f.fadeTick(14225);
    f.fadeTick(14226);
    f.fadeTick(15150);
    TEST_ASSERT_TRUE_MESSAGE(f.transport.client.isConnected(), "Fade abandoned its identical retry until timeout");
    TEST_ASSERT_EQUAL_UINT32(1, f.transport.client.sessionGeneration());
    TEST_ASSERT_FALSE(f.transport.client.commandInFlight(V1CommandOwner::VolumeFade));
    TEST_ASSERT_EQUAL_UINT32(3, f.transport.command.writeValueCalls());
    TEST_ASSERT_EQUAL_UINT8(6, f.transport.command.lastWriteValue()[5]);
    TEST_ASSERT_EQUAL_UINT8(2, f.transport.command.lastWriteValue()[6]);
    f.assertNextOwnerCanSend(15160);
}
void test_fade_clear_confirmation_with_pending_retry() { assertFadeConfirmationOverlap(false, RetryState::Pending); }
void test_fade_clear_confirmation_with_running_retry() { assertFadeConfirmationOverlap(false, RetryState::Running); }
void test_fade_clear_confirmation_with_completed_retry() { assertFadeConfirmationOverlap(false, RetryState::Completed); }
void test_fade_new_threat_confirmation_with_pending_retry() { assertFadeConfirmationOverlap(true, RetryState::Pending); }
void test_fade_new_threat_confirmation_with_running_retry() { assertFadeConfirmationOverlap(true, RetryState::Running); }
void test_fade_new_threat_confirmation_with_completed_retry() { assertFadeConfirmationOverlap(true, RetryState::Completed); }
void test_fade_confirmation_without_overlap() {
    QuietPolicyTransport f;
    f.beginFadeRestore(false);
    f.volume(6, 2, 14090);
    f.fadeTick(14090);
    f.fadeTick(15150);
    TEST_ASSERT_TRUE(f.transport.client.isConnected());
    TEST_ASSERT_FALSE(f.transport.client.commandInFlight(V1CommandOwner::VolumeFade));
    TEST_ASSERT_EQUAL_UINT32(2, f.transport.command.writeValueCalls());
    f.assertNextOwnerCanSend(15160);
}

void assertBoxConfirmationOverlap(bool unmute, RetryState state) {
    QuietPolicyTransport f;
    const uint32_t retryAt = f.beginBoxRetry(unmute);
    HeldCommand held(f.transport.command);
    f.boxTick(retryAt);
    TEST_ASSERT_TRUE(f.transport.client.commandInFlight(V1CommandOwner::InTheBox));
    bool started = true;
    if (state == RetryState::Running) started = held.start();
    if (state == RetryState::Completed) { held.release(); f.finishWorker(retryAt + 1); }
    f.displayMute(!unmute, retryAt + 10);
    f.boxTick(retryAt + 10);
    const bool connectedAtEcho = f.transport.client.isConnected();
    held.release();
    TEST_ASSERT_TRUE(started);
    TEST_ASSERT_TRUE_MESSAGE(connectedAtEcho, "Box confirmation disconnected the detector");
    if (state == RetryState::Pending) f.finishWorker(retryAt + 11);
    f.boxTick(retryAt + 100);
    // The normal display stream continues after the retry's actual wire boundary.
    f.displayMute(!unmute, retryAt + 110);
    f.boxTick(retryAt + 110);
    f.boxTick(retryAt + 1000);
    TEST_ASSERT_TRUE_MESSAGE(f.transport.client.isConnected(), "Box policy abandoned its identical retry until timeout");
    TEST_ASSERT_EQUAL_UINT32(1, f.transport.client.sessionGeneration());
    TEST_ASSERT_FALSE(f.transport.client.commandInFlight(V1CommandOwner::InTheBox));
    TEST_ASSERT_EQUAL_UINT32(unmute ? 3 : 2, f.transport.command.writeValueCalls());
    TEST_ASSERT_EQUAL_UINT8(unmute ? 0x35 : 0x34, f.transport.command.lastWriteValue()[3]);
    f.assertNextOwnerCanSend(retryAt + 1010);
}
void test_box_mute_confirmation_with_pending_retry() { assertBoxConfirmationOverlap(false, RetryState::Pending); }
void test_box_mute_confirmation_with_running_retry() { assertBoxConfirmationOverlap(false, RetryState::Running); }
void test_box_mute_confirmation_with_completed_retry() { assertBoxConfirmationOverlap(false, RetryState::Completed); }
void test_box_unmute_confirmation_with_pending_retry() { assertBoxConfirmationOverlap(true, RetryState::Pending); }
void test_box_unmute_confirmation_with_running_retry() { assertBoxConfirmationOverlap(true, RetryState::Running); }
void test_box_unmute_confirmation_with_completed_retry() { assertBoxConfirmationOverlap(true, RetryState::Completed); }
void test_box_confirmation_without_overlap() {
    QuietPolicyTransport f;
    f.beginBoxRetry(false);
    f.displayMute(true, 10110);
    f.boxTick(10110);
    f.boxTick(11100);
    TEST_ASSERT_TRUE(f.transport.client.isConnected());
    TEST_ASSERT_FALSE(f.transport.client.commandInFlight(V1CommandOwner::InTheBox));
    TEST_ASSERT_EQUAL_UINT32(1, f.transport.command.writeValueCalls());
    f.assertNextOwnerCanSend(11110);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_delayed_ack_keeps_notifications_and_loop_work_responsive);
#ifdef BLE_COMMAND_LATENCY_ONLY
    return UNITY_END();
#endif
    RUN_TEST(test_first_restore_echo_before_ack_harvest_keeps_connection);
    RUN_TEST(test_restore_echo_before_retry_keeps_connection);
    RUN_TEST(test_restore_echo_after_retry_harvest_keeps_connection);
    RUN_TEST(test_target_echo_with_pending_identical_retry_keeps_connection);
    RUN_TEST(test_target_echo_with_running_identical_retry_keeps_connection);
    RUN_TEST(test_target_echo_with_completed_identical_retry_keeps_connection);
    RUN_TEST(test_new_manual_pair_still_cancels_pending_restore_retry);
    RUN_TEST(test_same_target_confirmation_does_not_hide_blocked_retry_timeout);
    RUN_TEST(test_fade_clear_confirmation_with_pending_retry);
    RUN_TEST(test_fade_clear_confirmation_with_running_retry);
    RUN_TEST(test_fade_clear_confirmation_with_completed_retry);
    RUN_TEST(test_fade_new_threat_confirmation_with_pending_retry);
    RUN_TEST(test_fade_new_threat_confirmation_with_running_retry);
    RUN_TEST(test_fade_new_threat_confirmation_with_completed_retry);
    RUN_TEST(test_fade_confirmation_without_overlap);
    RUN_TEST(test_box_mute_confirmation_with_pending_retry);
    RUN_TEST(test_box_mute_confirmation_with_running_retry);
    RUN_TEST(test_box_mute_confirmation_with_completed_retry);
    RUN_TEST(test_box_unmute_confirmation_with_pending_retry);
    RUN_TEST(test_box_unmute_confirmation_with_running_retry);
    RUN_TEST(test_box_unmute_confirmation_with_completed_retry);
    RUN_TEST(test_box_confirmation_without_overlap);
    RUN_TEST(test_immediate_worker_success_still_requires_one_owner_harvest);
    RUN_TEST(test_timeout_pins_attributes_until_worker_exits_and_rejects_late_success);
    RUN_TEST(test_cancel_before_worker_start_prevents_wire_write);
    RUN_TEST(test_changed_same_owner_intent_retires_admitted_command);
    RUN_TEST(test_failure_retires_session_without_reissuing_ambiguous_command);
    RUN_TEST(test_task_launch_failure_does_not_pin_or_write);
    RUN_TEST(test_dual_mode_preserves_ack_and_small_no_response_stays_immediate);
    RUN_TEST(test_oversize_no_response_uses_worker_at_negotiated_mtu_boundary);
    RUN_TEST(test_early_volume_rejection_survives_ack_receipt_publication);
    RUN_TEST(test_followup_accepts_early_reply_only_after_actual_worker_boundary);
    RUN_TEST(test_completed_but_abandoned_command_has_bounded_ownership);
    RUN_TEST(test_stale_completed_volume_and_start_do_not_publish_new_session_receipts);
    RUN_TEST(test_early_start_rejection_is_not_erased_by_late_ack);
    RUN_TEST(test_sweep_capture_uses_worker_boundary_and_survives_ack_harvest);
    RUN_TEST(test_phone_raw_long_frame_waits_without_reorder_or_duplicate);
    RUN_TEST(test_phone_epoch_retirement_prevents_pending_worker_write);
    RUN_TEST(test_indexed_phone_chunks_keep_exact_order_on_safe_no_response_path);
    return UNITY_END();
}

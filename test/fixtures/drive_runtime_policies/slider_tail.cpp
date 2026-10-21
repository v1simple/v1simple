#include <unity.h>

#include <cstdint>
#include <vector>

#include "test/mocks/Arduino.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

// Include the production parser before the queue's mock include path can
// substitute its test double. This suite proves the queue signal and parser
// recovery contract together.
#include "src/packet_parser.cpp"
#include "src/packet_parser.h"
#include "src/packet_parser_alerts.cpp"

#include "test/mocks/ble_client.h"
#include "test/mocks/modules/power/power_module.h"
#include "test/mocks/v1_profiles.h"

class DisplayPreviewModule {
  public:
    bool isRunning() const { return running; }
    void cancel() { running = false; }
    bool running = false;
};

#define private public
#include "src/modules/ble/ble_queue_module.h"
#undef private
#include "src/modules/ble/ble_queue_module.cpp"

namespace {

constexpr uint32_t kSession = 9;
constexpr uint16_t kCharacteristic = 0xB2CE;

BleQueueModule queue;
PacketParser parser;
V1BLEClient client;
V1ProfileManager profiles;
DisplayPreviewModule preview;
PowerModule power;

std::vector<uint8_t> makeAlertFrame(uint8_t index, uint8_t count,
                                    uint16_t frequency, uint8_t bandArrow,
                                    uint8_t aux0) {
    std::vector<uint8_t> frame{
        ESP_PACKET_START,
        0xD8,
        0xEA,
        PACKET_ID_ALERT_DATA,
        0x08,
        static_cast<uint8_t>(((index & 0x0F) << 4) | (count & 0x0F)),
        static_cast<uint8_t>(frequency >> 8),
        static_cast<uint8_t>(frequency),
        0x90,
        0x00,
        bandArrow,
        aux0,
    };
    uint8_t checksum = 0;
    for (uint8_t value : frame)
        checksum = static_cast<uint8_t>(checksum + value);
    frame.push_back(checksum);
    frame.push_back(ESP_PACKET_END);
    return frame;
}

void deliver(const std::vector<uint8_t> &frame, uint32_t ingressSequence,
             uint32_t nowMs) {
    TEST_ASSERT_TRUE(queue.tryOnNotify(frame.data(), frame.size(),
                                       kCharacteristic, kSession, nowMs,
                                       ingressSequence));
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
    TEST_ASSERT_TRUE(
        queue.begin(&client, &parser, &profiles, &preview, &power, config));
    queue.openSession(kSession);
}

void tearDown() { queue.end(); }

#include "test/mocks/display.h"
#include "test/mocks/settings.h"

#include "src/touch_handler.cpp"
#include "src/modules/touch/touch_ui_module.cpp"
#include "src/runtime_coordinator.h"
void audio_set_volume(uint8_t) {}
void play_test_voice() {}
struct Runtime {
    V1Display display_;
    SettingsManager settings_;
    TouchHandler touch_;
    TouchUiModule touchUi_;
    PacketParser &parser_ = parser;
    struct {
        bool ownsLaserDisplay() const { return false; }
        AlpLaserEvent currentEvent() const { return {}; }
    } alp_;
    bool boot = false;
    unsigned drains = 0, presentations = 0;
    Runtime() {
        touch_.begin();
        TouchUiModule::Callbacks callbacks{};
        touchUi_.begin(&display_, &touch_, &settings_, callbacks);
    }
    bool active() { return true; }
    DriveLoopTiming beginDriveLoop() {
        return {0, static_cast<uint32_t>(mockMillis)};
    }
    struct Snapshot {
        bool connected = true, backpressured = false, overloaded = false,
             requestShowInitialScanning = false;
    };
    Snapshot processConnectionRuntime(uint32_t) { return {}; }
    void acceptConnectionSnapshot(Snapshot) {}
    void markInitialScanningScreenHandled() {}
    void showInitialScanningScreen() {}
    bool powerOwnsPresentation() { return false; }
    void presentConnectionState(Snapshot) {}
    void processPower(uint32_t) {}
    bool processTouch(uint32_t now) { return touchUi_.process(now, boot); }
    void servicePowerDisplayOwnership(uint32_t) {}
    void processAlp(uint32_t) {}
    bool preemptSettingsForLiveAlert();
    bool hasLiveAlertPresentation() const;
    void openBootReadyGate(uint32_t) {}
    void processBleRuntime() {}
    void processBleQueue() {
        ++drains;
        queue.process();
    }
    bool bleQueueBackpressured() { return queue.isBackpressured(); }
    void processConnectionCycle(uint32_t, bool) {}
    void processObd(uint32_t, bool) {}
    void observeAlpProductState(uint32_t) {}
    void processAlpPresentationAndPower(uint32_t) {}
    void processGps(uint32_t) {}
    void processSpeed(uint32_t) {}
    void processSpeedAlert(uint32_t) {}
    void servicePowerAlertPresentation(uint32_t, bool) {}
    bool powerAlertPresentationNeedsRestore() { return false; }
    void processTapGesture(uint32_t) {}
    struct Edges {
        bool parsedReady;
    };
    Edges consumeDisplayEdges() { return {queue.consumeParsedFlag()}; }
    bool processParsedPolicies(Edges e) { return e.parsedReady; }
    void presentDisplay(Edges e, bool) {
        if (e.parsedReady)
            ++presentations;
    }
    DriveLoopDispatch processConnectionDispatch(bool) {
        return {static_cast<uint32_t>(mockMillis), true};
    }
    void processPeriodicMaintenance(uint32_t, bool, bool, bool, bool) {}
    BleQueueModule &bleQueue_ = queue;
    struct {
        uint32_t lastLoopUs = 0;
    } state_;
    uint32_t finishLoop(bool, uint32_t, bool);
    void finishDriveLoop(bool, uint32_t, bool);
    void tick(uint32_t now) {
        mockMillis = now;
        DriveLoopCoordinator::tick(*this);
    }
};
#include "slider_tail.inc"

void test_fresh_v1_queue_drains_at_slider_tail_and_preempts_next_tick() {
    Runtime r;
    r.boot = true;
    r.tick(1000);
    r.boot = false;
    r.tick(1400);
    TEST_ASSERT_EQUAL_INT(1, r.display_.showSettingsSlidersCalls);
    const auto packet = makeAlertFrame(1, 1, 34700, 0x22, 0x80);
    TEST_ASSERT_TRUE(queue.tryOnNotify(packet.data(), packet.size(),
                                       kCharacteristic, kSession, 1500, 1));
    r.tick(1500);
    TEST_ASSERT_TRUE(parser.hasAlerts());
    TEST_ASSERT_EQUAL_UINT(0, uxQueueMessagesWaiting(queue.queueHandle_));
    TEST_ASSERT_EQUAL_INT(0, r.display_.hideBrightnessSliderCalls);
    TEST_ASSERT_EQUAL_UINT(0, r.presentations);
    r.tick(1525);
    TEST_ASSERT_EQUAL_INT(1, r.display_.hideBrightnessSliderCalls);
    TEST_ASSERT_EQUAL_UINT(1, r.presentations);
}
void test_normal_loop_drains_the_same_valid_notification() {
    Runtime r;
    const auto packet = makeAlertFrame(1, 1, 34700, 0x22, 0x80);
    TEST_ASSERT_TRUE(queue.tryOnNotify(packet.data(), packet.size(),
                                       kCharacteristic, kSession, 1500, 1));
    r.tick(1500);
    TEST_ASSERT_TRUE(parser.hasAlerts());
    TEST_ASSERT_EQUAL_UINT(1, r.presentations);
}
void test_already_parsed_alert_preempts_real_slider_owner() {
    Runtime r;
    r.boot = true;
    r.tick(1000);
    r.boot = false;
    r.tick(1400);
    deliver(makeAlertFrame(1, 1, 34700, 0x22, 0x80), 1, 1500);
    r.tick(1500);
    TEST_ASSERT_EQUAL_INT(1, r.display_.hideBrightnessSliderCalls);
    TEST_ASSERT_EQUAL_INT(1, r.settings_.requestDeferredPersistCalls);
    TEST_ASSERT_EQUAL_UINT(1, r.presentations);
}
int main() {
    UNITY_BEGIN();
    RUN_TEST(test_fresh_v1_queue_drains_at_slider_tail_and_preempts_next_tick);
    RUN_TEST(test_normal_loop_drains_the_same_valid_notification);
    RUN_TEST(test_already_parsed_alert_preempts_real_slider_owner);
    return UNITY_END();
}

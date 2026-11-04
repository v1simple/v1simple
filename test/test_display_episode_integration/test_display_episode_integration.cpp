// Real BLE queue -> parser -> display persistence owner.
// Completed/coalesced episodes retain the same timestamps and presentation.
#include <unity.h>

#include <initializer_list>
#include <vector>

#include "../mocks/Arduino.h"
#include "../mocks/ble_client.h"
#include "../mocks/settings.h"
#include "../mocks/modules/power/power_module.h"

SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;

// Include the real parser before any module can select its test double.
#include "../../src/packet_parser.h"
#include "../../src/packet_parser.cpp"
#include "../../src/packet_parser_alerts.cpp"
#include "../mocks/display.h"
#include "../mocks/runtime_presentation_source.h"
#include "../../src/modules/alert_persistence/alert_persistence_module.cpp"
#include "../../src/modules/alp/alp_event_latch.cpp"
#include "../../src/modules/display/render_frame_composer.cpp"
#include "../../src/modules/display/display_pipeline_module.cpp"
#include "../support/display_pipeline_fixture.h"
void QuietCoordinatorModule::syncCommittedState() { TEST_FAIL_MESSAGE("unexpected quiet owner"); }

class DisplayPreviewModule {
  public:
    bool isRunning() const { return false; }
    void cancel() {}
};
#include "../../src/modules/ble/ble_queue_module.cpp"

namespace {
struct DisplayRuntime {
    BleQueueModule queue;
    PacketParser parser;
    V1BLEClient ble;
    DisplayPreviewModule preview;
    PowerModule power;
    SettingsManager settings;
    RuntimePresentationSource presentation{settings};
    V1Display display;
    AlertPersistenceModule persistence;
    DisplayMode mode = DisplayMode::IDLE;
    DisplayPipelineModule pipeline;
    uint32_t ingressSequence = 0;
    uint32_t generation = 9;

    DisplayRuntime() {
        DisplayPipelineDependencies dependencies;
        dependencies.displayMode = &mode;
        dependencies.display = &display;
        dependencies.parser = &parser;
        dependencies.settings = &settings;
        dependencies.presentation = &presentation;
        dependencies.ble = &ble;
        dependencies.alertPersistence = &persistence;
        pipeline.begin(dependencies);
        TEST_ASSERT_TRUE(queue.begin(&ble, &parser, &preview, &power));
        queue.openSession(generation);
        enqueue(PACKET_ID_RESP_ALL_VOLUME, {6, 2, 6, 2}, 9000);
        present(9000);
    }

    ~DisplayRuntime() { queue.end(); }

    void enqueue(uint8_t id, std::initializer_list<uint8_t> payload, uint32_t nowMs) {
        const uint8_t destination = (id == PACKET_ID_ALERT_DATA || id == PACKET_ID_DISPLAY_DATA) ? 0xD8 : 0xD6;
        std::vector<uint8_t> frame{0xAA, destination, 0xEA, id, static_cast<uint8_t>(payload.size() + 1)};
        frame.insert(frame.end(), payload);
        uint8_t checksum = 0;
        for (uint8_t value : frame) checksum = static_cast<uint8_t>(checksum + value);
        frame.push_back(checksum);
        frame.push_back(0xAB);
        TEST_ASSERT_TRUE(queue.tryOnNotify(frame.data(), frame.size(), 0xB2CE, generation, nowMs, ++ingressSequence));
    }

    void alert(uint32_t nowMs, uint16_t frequency = 34700, bool secondary = false) {
        enqueue(PACKET_ID_ALERT_DATA,
                {static_cast<uint8_t>(secondary ? 0x12 : 0x11), static_cast<uint8_t>(frequency >> 8),
                 static_cast<uint8_t>(frequency), 0xAC, 0, 0x22, 0x80}, nowMs);
        if (secondary) {
            enqueue(PACKET_ID_ALERT_DATA, {0x22, 0x5E, 0x56, 0x90, 0, 0x24, 0}, nowMs);
        }
    }

    void clear(uint32_t nowMs) { enqueue(PACKET_ID_ALERT_DATA, {0, 0, 0, 0, 0, 0, 0}, nowMs); }

    void admit(uint32_t nowMs) {
        presentParsedFrame(pipeline, nowMs);
    }

    void present(uint32_t nowMs) {
        mockMillis = nowMs;
        queue.process();
        TEST_ASSERT_TRUE(queue.consumeParsedFlag());
        admit(nowMs);
        TEST_ASSERT_EQUAL_UINT32(0, HealthCounters::inputDrops());
    }
};
} // namespace

void setUp() {
    mockMillis = mockMicros = 0;
    HealthCounters::reset();
}
void tearDown() {}

void test_batched_completed_encounter_retains_priority_until_original_expiry() {
    DisplayRuntime runtime;
    runtime.settings.slotAlertPersistSec[0] = 3;
    runtime.alert(10000, 34700, true);
    runtime.clear(10100);
    runtime.present(11500);
    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::V1_PERSISTED, runtime.display.lastRenderFrame.primaryKind);
    TEST_ASSERT_EQUAL_UINT32(34700, runtime.display.lastRenderFrame.v1Priority.frequency);
    runtime.pipeline.refreshBlinkTick(13099);
    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::V1_PERSISTED, runtime.display.lastRenderFrame.primaryKind);
    TEST_ASSERT_TRUE(runtime.pipeline.v1PersistenceRefreshDue(13100));
    runtime.pipeline.refreshBlinkTick(13100);
    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::IDLE, runtime.display.lastRenderFrame.primaryKind);
    runtime.admit(13200);
    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::IDLE, runtime.display.lastRenderFrame.primaryKind);
}

void test_delayed_completed_encounter_cannot_renew_expired_persistence() {
    DisplayRuntime runtime;
    runtime.settings.slotAlertPersistSec[0] = 3;
    runtime.alert(10000);
    runtime.clear(10100);
    runtime.present(13100);
    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::IDLE, runtime.display.lastRenderFrame.primaryKind);
    TEST_ASSERT_FALSE(runtime.persistence.getPersistedAlert().isValid);
}

void test_completed_encounter_timestamp_wrap_and_zero_keep_original_expiry() {
    for (uint32_t clearedAt : {uint32_t{0}, UINT32_MAX - 999u}) {
        DisplayRuntime runtime;
        runtime.settings.slotAlertPersistSec[0] = 3;
        runtime.alert(clearedAt - 100u);
        runtime.clear(clearedAt);
        runtime.present(clearedAt + 2000u);
        TEST_ASSERT_EQUAL(RenderFramePrimaryKind::V1_PERSISTED, runtime.display.lastRenderFrame.primaryKind);
        runtime.pipeline.refreshBlinkTick(clearedAt + 2999u);
        TEST_ASSERT_EQUAL(RenderFramePrimaryKind::V1_PERSISTED, runtime.display.lastRenderFrame.primaryKind);
        runtime.pipeline.refreshBlinkTick(clearedAt + 3000u);
        TEST_ASSERT_EQUAL(RenderFramePrimaryKind::IDLE, runtime.display.lastRenderFrame.primaryKind);
    }
}

void test_completed_encounter_does_not_replace_new_live_or_survive_session_reset() {
    DisplayRuntime runtime;
    runtime.settings.slotAlertPersistSec[0] = 3;
    runtime.alert(10000);
    runtime.clear(10100);
    runtime.alert(10200, 35500);
    runtime.present(10200);
    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::V1_LIVE, runtime.display.lastRenderFrame.primaryKind);
    TEST_ASSERT_EQUAL_UINT32(35500, runtime.display.lastRenderFrame.v1Priority.frequency);
    runtime.clear(10300);
    runtime.present(10300);
    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::V1_PERSISTED, runtime.display.lastRenderFrame.primaryKind);
    runtime.queue.openSession(10);
    runtime.parser.resetAlertState(); // Connection owner resets parser session state.
    runtime.admit(10400);
    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::IDLE, runtime.display.lastRenderFrame.primaryKind);
}

void test_slot_switch_cannot_resurrect_unpresented_or_expired_encounter() {
    DisplayRuntime runtime;
    runtime.settings.slotAlertPersistSec[0] = 0;
    runtime.settings.slotAlertPersistSec[1] = 3;
    runtime.alert(10000);
    runtime.clear(10100);
    runtime.present(10100);
    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::IDLE, runtime.display.lastRenderFrame.primaryKind);
    runtime.settings.settings.activeSlot = 1;
    runtime.admit(10200);
    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::IDLE, runtime.display.lastRenderFrame.primaryKind);
}

void test_batched_display_laser_clear_retains_laser_without_radar_rows() {
    DisplayRuntime runtime;
    runtime.settings.slotAlertPersistSec[0] = 3;
    runtime.enqueue(PACKET_ID_DISPLAY_DATA, {0x49, 0, 0xFF, 0x21, 0, 0x0C, 0, 0x62}, 10000);
    runtime.enqueue(PACKET_ID_DISPLAY_DATA, {0x3F, 0, 0, 0, 0, 0x0C, 0, 0x62}, 10100);
    runtime.present(10100);
    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::V1_PERSISTED, runtime.display.lastRenderFrame.primaryKind);
    TEST_ASSERT_EQUAL(BAND_LASER, runtime.display.lastRenderFrame.v1Priority.band);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_batched_completed_encounter_retains_priority_until_original_expiry);
    RUN_TEST(test_delayed_completed_encounter_cannot_renew_expired_persistence);
    RUN_TEST(test_completed_encounter_timestamp_wrap_and_zero_keep_original_expiry);
    RUN_TEST(test_completed_encounter_does_not_replace_new_live_or_survive_session_reset);
    RUN_TEST(test_slot_switch_cannot_resurrect_unpresented_or_expired_encounter);
    RUN_TEST(test_batched_display_laser_clear_retains_laser_without_radar_rows);
    return UNITY_END();
}

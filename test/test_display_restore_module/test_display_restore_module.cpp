#include <unity.h>

#include "../mocks/Arduino.h"
#include "../mocks/display.h"
#include "../mocks/packet_parser.h"
#include "../mocks/ble_client.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../src/perf_metrics.h"
#include "../../src/modules/display/display_pipeline_module.h"
#include "../../src/modules/display/display_preview_module.h"

PerfCounters perfCounters;
PerfExtendedMetrics perfExtended;
void perfRecordDisplayScenarioRenderUs(uint32_t /*us*/) {}
void perfSetDisplayRenderScenario(PerfDisplayRenderScenario /*scenario*/) {}
void perfClearDisplayRenderScenario() {}

static int g_restoreCurrentOwnerCalls = 0;
static uint32_t g_lastRestoreNowMs = 0;

void DisplayPipelineModule::restoreCurrentOwner(uint32_t nowMs) {
    ++g_restoreCurrentOwnerCalls;
    g_lastRestoreNowMs = nowMs;
}

DisplayPreviewModule::DisplayPreviewModule() = default;

void DisplayPreviewModule::begin(V1Display* displayPtr) {
    display_ = displayPtr;
}

void DisplayPreviewModule::requestHold(uint32_t durationMs) {
    previewActive_ = (durationMs != 0);
}

void DisplayPreviewModule::cancel() {
    previewActive_ = false;
    previewEnded_ = true;
}

bool DisplayPreviewModule::consumeEnded() {
    const bool ended = previewEnded_;
    previewEnded_ = false;
    return ended;
}

void DisplayPreviewModule::update() {}

#include "../../src/modules/display/display_restore_module.cpp"

static V1Display display;
static PacketParser parser;
static V1BLEClient ble;
static DisplayPreviewModule preview;
static DisplayPipelineModule pipeline;
static DisplayRestoreModule module;

void setUp() {
    display.reset();
    parser.reset();
    ble.reset();
    preview = DisplayPreviewModule{};
    g_restoreCurrentOwnerCalls = 0;
    g_lastRestoreNowMs = 0;
    module.begin(&display, &parser, &ble, &preview, &pipeline);
}

void tearDown() {}

void test_process_is_noop_when_preview_has_not_ended() {
    TEST_ASSERT_FALSE(module.process());
    TEST_ASSERT_EQUAL(0, g_restoreCurrentOwnerCalls);
}

void test_process_restores_via_pipeline_when_preview_ends() {
    mockMillis = 4321;
    preview.cancel();

    TEST_ASSERT_TRUE(module.process());
    TEST_ASSERT_EQUAL(1, g_restoreCurrentOwnerCalls);
    TEST_ASSERT_EQUAL(4321u, g_lastRestoreNowMs);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_process_is_noop_when_preview_has_not_ended);
    RUN_TEST(test_process_restores_via_pipeline_when_preview_ends);
    return UNITY_END();
}

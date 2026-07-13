#include <unity.h>

#include "../mocks/Arduino.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../src/modules/wifi/wifi_auto_start_module.cpp"
#include "../../src/status_observability_payload.cpp"

void setUp() {}
void tearDown() {}

void test_append_status_observability_adds_wifi_runtime_fields() {
    JsonDocument doc;
    JsonObject root = doc.to<JsonObject>();
    JsonObject wifi = root["wifi"].to<JsonObject>();
    wifi["setup_mode"] = true;

    StatusObservabilityPayload::WifiStatusSnapshot wifiStatus;
    wifiStatus.apLastTransitionReasonCode = 4;
    wifiStatus.apLastTransitionReason = "low_dma";
    wifiStatus.lowDmaCooldownRemainingMs = 12000;
    wifiStatus.autoStart.gate = WifiAutoStartGate::WaitingDma;
    wifiStatus.autoStart.enableWifi = true;
    wifiStatus.autoStart.bleConnected = true;
    wifiStatus.autoStart.v1ConnectedAtMs = 1000;
    wifiStatus.autoStart.msSinceV1Connect = 2500;
    wifiStatus.autoStart.settleMs = 3000;
    wifiStatus.autoStart.bootTimeoutMs = 30000;
    wifiStatus.autoStart.canStartDma = false;
    wifiStatus.autoStart.wifiAutoStartDone = false;
    wifiStatus.autoStart.bleSettled = false;
    wifiStatus.autoStart.bootTimeoutReached = false;
    wifiStatus.autoStart.shouldAutoStart = false;
    wifiStatus.autoStart.startTriggered = false;
    wifiStatus.autoStart.startSucceeded = false;

    StatusObservabilityPayload::appendStatusObservability(root, wifiStatus);

    TEST_ASSERT_TRUE(root["wifi"]["setup_mode"].as<bool>());
    TEST_ASSERT_EQUAL_STRING("low_dma", root["wifi"]["ap_last_transition_reason"].as<const char*>());
    TEST_ASSERT_EQUAL_UINT32(12000, root["wifi"]["low_dma_cooldown_ms"].as<uint32_t>());
    TEST_ASSERT_EQUAL_STRING(
        "waiting_dma",
        root["wifi"]["auto_start"]["gate"].as<const char*>());
    TEST_ASSERT_FALSE(root["wifi"]["auto_start"]["startSucceeded"].as<bool>());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_append_status_observability_adds_wifi_runtime_fields);
    return UNITY_END();
}

/**
 * test_connection_state.cpp - ConnectionStateModule Logic Tests
 * 
 * Tests the BLE connection state tracking logic:
 * - Connect/disconnect transitions
 * - Parser state reset on disconnect
 * - Stale data detection and recovery
 * - Request rate limiting
 * 
 * Note: We test the logic patterns directly since the module implementation
 * includes hardware headers that can't be mocked in native tests.
 */
#include <unity.h>

// Test-controllable time
static unsigned long mockMillis = 0;

// Mocks
#ifndef ARDUINO
#include "../mocks/Arduino.h"
#endif
#include "../mocks/ble_client.h"
#include "../mocks/display.h"
#include "../mocks/packet_parser.h"
#include "../mocks/modules/power/power_module.h"
#include "../mocks/modules/ble/ble_queue_module.h"

// Globals for mocks
#ifndef ARDUINO
SerialClass Serial;
#endif

// Module instances
static V1BLEClient bleClient;
static V1Display display;
static PacketParser parser;
static PowerModule powerModule;
static BleQueueModule bleQueueModule;

// Compile the REAL module too: the replica below pins the logic pattern, but
// only the real translation unit can kill catalog mutations against
// src/modules/ble/connection_state_module.cpp (critical-020 survived while
// this suite exercised the copy only — 2026-07-09 review). The event bus has
// no mock; the real header is native-safe.
#include "../../src/modules/system/system_event_bus.h"
#include "../../src/modules/ble/connection_state_module.cpp"

// Replicate the ConnectionStateModule logic for testing
// These constants match the real module
static constexpr unsigned long DATA_STALE_MS = 2000;
static constexpr unsigned long DATA_REQUEST_INTERVAL_MS = 1000;

struct ConnectionStateLogic {
    bool wasConnected = false;
    unsigned long lastDataRequestMs = 0;
    
    void reset() {
        wasConnected = false;
        lastDataRequestMs = 0;
    }
    
    // Returns true if connected
    bool process(unsigned long nowMs, V1BLEClient* ble, PacketParser* parserPtr,
                 V1Display* displayPtr, PowerModule* power, BleQueueModule* bleQueue) {
        bool isConnected = ble->isConnected();
        
        // Handle state transitions
        if (isConnected != wasConnected) {
            if (power) {
                power->onV1ConnectionChange(isConnected);
            }
            
            if (isConnected) {
                displayPtr->showResting();
            } else {
                parserPtr->resetAlertAssembly();
                displayPtr->resetChangeTracking();
                displayPtr->showScanning();
            }
            wasConnected = isConnected;
        }
        
        // If connected but not seeing traffic, periodically re-request alert data
        if (isConnected && bleQueue) {
            unsigned long lastRx = bleQueue->getLastRxMillis();
            bool dataStale = (nowMs - lastRx) > DATA_STALE_MS;
            bool canRequest = (nowMs - lastDataRequestMs) > DATA_REQUEST_INTERVAL_MS;
            
            if (dataStale && canRequest) {
                ble->requestAlertData();
                lastDataRequestMs = nowMs;
            }
        }
        
        // When disconnected, refresh indicators periodically
        if (!isConnected) {
            displayPtr->drawWiFiIndicator();
            displayPtr->drawBatteryIndicator();
            displayPtr->flush();
        }
        
        return isConnected;
    }
};

static ConnectionStateLogic connectionState;

// ============================================================================
// Test: Connection Transitions
// ============================================================================

void test_connect_transition_shows_resting() {
    // Setup: disconnected state
    bleClient.setConnected(false);
    connectionState.reset();
    connectionState.process(0, &bleClient, &parser, &display, &powerModule, &bleQueueModule);
    
    display.reset();
    powerModule.reset();
    
    // Simulate connection
    bleClient.setConnected(true);
    mockMillis = 1000;
    bool result = connectionState.process(mockMillis, &bleClient, &parser, &display, &powerModule, &bleQueueModule);
    
    TEST_ASSERT_TRUE(result);  // Now connected
    TEST_ASSERT_EQUAL(1, display.showRestingCalls);
    TEST_ASSERT_EQUAL(1, powerModule.onV1ConnectionChangeCalls);
    TEST_ASSERT_TRUE(powerModule.lastConnectionState);
}

void test_disconnect_transition_shows_scanning() {
    // Setup: connected state
    bleClient.setConnected(true);
    connectionState.reset();
    connectionState.process(0, &bleClient, &parser, &display, &powerModule, &bleQueueModule);
    
    display.reset();
    powerModule.reset();
    parser.reset();
    display.resetChangeTrackingCalls = 0;
    bleClient.setConnected(false);
    mockMillis = 1000;
    bool result = connectionState.process(mockMillis, &bleClient, &parser, &display, &powerModule, &bleQueueModule);
    
    TEST_ASSERT_FALSE(result);  // Now disconnected
    TEST_ASSERT_EQUAL(1, display.showScanningCalls);
    TEST_ASSERT_EQUAL(1, powerModule.onV1ConnectionChangeCalls);
    TEST_ASSERT_FALSE(powerModule.lastConnectionState);
    
    // Parser state should be reset
    TEST_ASSERT_EQUAL(1, parser.resetAlertAssemblyCalls);
    TEST_ASSERT_EQUAL(1, display.resetChangeTrackingCalls);
}

void test_no_transition_when_state_unchanged() {
    // Setup: connected state
    bleClient.setConnected(true);
    connectionState.reset();
    connectionState.process(0, &bleClient, &parser, &display, &powerModule, &bleQueueModule);
    
    display.reset();
    powerModule.reset();
    
    // Process again with same state (use fresh data to avoid stale request)
    mockMillis = 500;
    bleQueueModule.setLastRxMillis(mockMillis);
    connectionState.process(mockMillis, &bleClient, &parser, &display, &powerModule, &bleQueueModule);
    
    // No transition callbacks
    TEST_ASSERT_EQUAL(0, display.showRestingCalls);
    TEST_ASSERT_EQUAL(0, display.showScanningCalls);
    TEST_ASSERT_EQUAL(0, powerModule.onV1ConnectionChangeCalls);
}

void test_boot_hold_no_spurious_transition() {
    // Boot-hold scenario: BLE stays disconnected before boot gate opens.
    bleClient.setConnected(false);
    connectionState.reset();
    display.reset();
    powerModule.reset();

    mockMillis = 1000;
    bool result = connectionState.process(mockMillis, &bleClient, &parser, &display, &powerModule, &bleQueueModule);

    TEST_ASSERT_FALSE(result);
    TEST_ASSERT_EQUAL(0, display.showRestingCalls);
    TEST_ASSERT_EQUAL(0, display.showScanningCalls);
}

// ============================================================================
// Test: Stale Data Detection
// ============================================================================

void test_stale_data_triggers_alert_request() {
    // Setup: connected, with recent data
    bleClient.setConnected(true);
    bleQueueModule.setLastRxMillis(0);
    connectionState.reset();
    connectionState.process(0, &bleClient, &parser, &display, &powerModule, &bleQueueModule);
    
    bleClient.reset();
    bleClient.setConnected(true);
    
    // Time passes - data becomes stale (>2000ms)
    mockMillis = 3000;
    bleQueueModule.setLastRxMillis(0);  // Last data at time 0
    
    connectionState.process(mockMillis, &bleClient, &parser, &display, &powerModule, &bleQueueModule);
    
    TEST_ASSERT_EQUAL(1, bleClient.requestAlertDataCalls);
}

void test_fresh_data_skips_alert_request() {
    // Setup: connected
    bleClient.setConnected(true);
    connectionState.reset();
    connectionState.process(0, &bleClient, &parser, &display, &powerModule, &bleQueueModule);
    
    bleClient.reset();
    bleClient.setConnected(true);
    
    // Recent data (within 2000ms)
    mockMillis = 1500;
    bleQueueModule.setLastRxMillis(1000);  // 500ms ago
    
    connectionState.process(mockMillis, &bleClient, &parser, &display, &powerModule, &bleQueueModule);
    
    TEST_ASSERT_EQUAL(0, bleClient.requestAlertDataCalls);
}

void test_request_rate_limiting() {
    // Setup: connected, stale data
    bleClient.setConnected(true);
    bleQueueModule.setLastRxMillis(0);
    connectionState.reset();
    connectionState.process(0, &bleClient, &parser, &display, &powerModule, &bleQueueModule);
    
    bleClient.reset();
    bleClient.setConnected(true);
    
    // First request at 3000ms (stale)
    mockMillis = 3000;
    connectionState.process(mockMillis, &bleClient, &parser, &display, &powerModule, &bleQueueModule);
    TEST_ASSERT_EQUAL(1, bleClient.requestAlertDataCalls);
    
    // Too soon for another request (within 1000ms)
    mockMillis = 3500;
    connectionState.process(mockMillis, &bleClient, &parser, &display, &powerModule, &bleQueueModule);
    TEST_ASSERT_EQUAL(1, bleClient.requestAlertDataCalls);  // Still 1
    
    // OK to request again (>1000ms since last request)
    mockMillis = 4500;
    connectionState.process(mockMillis, &bleClient, &parser, &display, &powerModule, &bleQueueModule);
    TEST_ASSERT_EQUAL(2, bleClient.requestAlertDataCalls);
}

// ============================================================================
// Test: Disconnected Indicator Refresh
// ============================================================================

void test_disconnected_refreshes_indicators() {
    // Setup: disconnected
    bleClient.setConnected(false);
    connectionState.reset();
    connectionState.process(0, &bleClient, &parser, &display, &powerModule, &bleQueueModule);
    
    display.reset();
    
    // Process while disconnected
    mockMillis = 1000;
    connectionState.process(mockMillis, &bleClient, &parser, &display, &powerModule, &bleQueueModule);
    
    TEST_ASSERT_EQUAL(1, display.drawWiFiIndicatorCalls);
    TEST_ASSERT_EQUAL(1, display.drawBatteryIndicatorCalls);
    TEST_ASSERT_EQUAL(1, display.flushCalls);
}

void test_connected_skips_indicator_refresh() {
    // Setup: connected
    bleClient.setConnected(true);
    bleQueueModule.setLastRxMillis(0);
    connectionState.reset();
    connectionState.process(0, &bleClient, &parser, &display, &powerModule, &bleQueueModule);
    
    display.reset();
    
    // Fresh data to avoid stale logic
    mockMillis = 500;
    bleQueueModule.setLastRxMillis(mockMillis);
    
    connectionState.process(mockMillis, &bleClient, &parser, &display, &powerModule, &bleQueueModule);
    
    TEST_ASSERT_EQUAL(0, display.drawWiFiIndicatorCalls);
    TEST_ASSERT_EQUAL(0, display.drawBatteryIndicatorCalls);
    TEST_ASSERT_EQUAL(0, display.flushCalls);
}

// ============================================================================
// Main
// ============================================================================

// REAL-module boundary pin (mutation catalog critical-020): exactly
// DATA_STALE_MS elapsed is NOT stale ('>' semantics); one ms past is. This
// must exercise the real ConnectionStateModule — the replica above cannot
// kill mutations applied to the module source.
void test_real_module_data_stale_boundary_is_exclusive() {
    ConnectionStateModule real;
    real.begin(&bleClient, &parser, &display, &powerModule, &bleQueueModule);

    bleClient.setConnected(true);
    bleQueueModule.setLastRxMillis(10000);
    real.process(10000);  // connect transition poll
    bleClient.requestAlertDataCalls = 0;

    // Exactly DATA_STALE_MS (2000 ms) since last RX: not stale yet.
    real.process(12000);
    TEST_ASSERT_EQUAL(0, bleClient.requestAlertDataCalls);

    // One ms past the threshold: stale, re-request fires.
    real.process(12001);
    TEST_ASSERT_EQUAL(1, bleClient.requestAlertDataCalls);
}

void setUp() {
    mockMillis = 0;
    bleClient.reset();
    display.reset();
    parser.reset();
    powerModule.reset();
    bleQueueModule.reset();
    display.resetChangeTrackingCalls = 0;
}

void runAllTests() {
    // Connection transitions
    RUN_TEST(test_connect_transition_shows_resting);
    RUN_TEST(test_disconnect_transition_shows_scanning);
    RUN_TEST(test_no_transition_when_state_unchanged);
    RUN_TEST(test_boot_hold_no_spurious_transition);
    
    // Stale data detection
    RUN_TEST(test_stale_data_triggers_alert_request);
    RUN_TEST(test_fresh_data_skips_alert_request);
    RUN_TEST(test_request_rate_limiting);
    
    // Disconnected indicator refresh
    RUN_TEST(test_disconnected_refreshes_indicators);
    RUN_TEST(test_connected_skips_indicator_refresh);

    // Real-module mutation pins
    RUN_TEST(test_real_module_data_stale_boundary_is_exclusive);
}

#ifdef ARDUINO
void setup() {
    delay(2000);
    UNITY_BEGIN();
    runAllTests();
    UNITY_END();
}
void loop() {}
#else
int main(int argc, char **argv) {
    UNITY_BEGIN();
    runAllTests();
    return UNITY_END();
}
#endif

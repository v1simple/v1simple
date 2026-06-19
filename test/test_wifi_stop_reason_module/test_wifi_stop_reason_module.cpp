#include <unity.h>

#include "../mocks/Arduino.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../src/modules/wifi/wifi_stop_reason_module.cpp"

static PerfCounters counters;

void setUp() {
    counters.reset();
}

void tearDown() {}

void test_record_stop_request_tracks_reason_and_mode() {
    WifiStopReasonModule module(&counters);

    module.recordStopRequest("timeout", false, false);
    module.recordStopRequest("no_clients", true, false);
    module.recordStopRequest("no_clients_auto", false, false);
    module.recordStopRequest("low_dma", false, true);
    module.recordStopRequest("poweroff", false, true);

    TEST_ASSERT_EQUAL_UINT32(3u, counters.wifiStopGraceful.load());
    TEST_ASSERT_EQUAL_UINT32(2u, counters.wifiStopImmediate.load());
    TEST_ASSERT_EQUAL_UINT32(1u, counters.wifiStopManual.load());

    TEST_ASSERT_EQUAL_UINT32(1u, counters.wifiStopTimeout.load());
    TEST_ASSERT_EQUAL_UINT32(1u, counters.wifiStopNoClients.load());
    TEST_ASSERT_EQUAL_UINT32(1u, counters.wifiStopNoClientsAuto.load());
    TEST_ASSERT_EQUAL_UINT32(1u, counters.wifiStopLowDma.load());
    TEST_ASSERT_EQUAL_UINT32(1u, counters.wifiStopPoweroff.load());
    TEST_ASSERT_EQUAL_UINT32(0u, counters.wifiStopOther.load());
}

void test_record_stop_request_maps_unknown_to_other() {
    WifiStopReasonModule module(&counters);

    module.recordStopRequest("", false, false);
    module.recordStopRequest(nullptr, false, false);
    module.recordStopRequest("toggle", false, false);

    TEST_ASSERT_EQUAL_UINT32(3u, counters.wifiStopGraceful.load());
    TEST_ASSERT_EQUAL_UINT32(0u, counters.wifiStopImmediate.load());
    TEST_ASSERT_EQUAL_UINT32(3u, counters.wifiStopOther.load());
}

void test_record_ap_drop_counters() {
    WifiStopReasonModule module(&counters);

    module.recordApDropLowDma();
    module.recordApDropIdleSta();

    TEST_ASSERT_EQUAL_UINT32(1u, counters.wifiApDropLowDma.load());
    TEST_ASSERT_EQUAL_UINT32(1u, counters.wifiApDropIdleSta.load());
}

void test_null_counters_is_safe_noop() {
    WifiStopReasonModule module;

    module.recordStopRequest("timeout", true, true);
    module.recordApDropLowDma();
    module.recordApDropIdleSta();

    TEST_ASSERT_EQUAL_UINT32(0u, counters.wifiStopImmediate.load());
    TEST_ASSERT_EQUAL_UINT32(0u, counters.wifiApDropLowDma.load());
    TEST_ASSERT_EQUAL_UINT32(0u, counters.wifiApDropIdleSta.load());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_record_stop_request_tracks_reason_and_mode);
    RUN_TEST(test_record_stop_request_maps_unknown_to_other);
    RUN_TEST(test_record_ap_drop_counters);
    RUN_TEST(test_null_counters_is_safe_noop);
    return UNITY_END();
}

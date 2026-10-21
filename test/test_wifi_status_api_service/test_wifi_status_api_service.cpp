#include <unity.h>
#include "../mocks/Arduino.h"
#include "../mocks/WebServer.h"
#include "../../src/modules/wifi/wifi_status_api_service.cpp"

SerialClass Serial;
unsigned long mockMillis = 1000;
unsigned long mockMicros = 0;

void setUp() {}
void tearDown() {}

static JsonDocument statusDoc(WifiStatusApiService::StatusRuntime runtime) {
    WebServer server(80);
    WifiStatusApiService::StatusJsonCache cache;
    unsigned long last = 0;
    WifiStatusApiService::handleApiStatus(server, runtime, cache, last, 100,
                                         [](void*) { return mockMillis; }, nullptr, nullptr, nullptr);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    JsonDocument doc;
    TEST_ASSERT_TRUE(deserializeJson(doc, server.lastBody) == DeserializationError::Ok);
    WifiStatusApiService::releaseStatusJsonCache(cache, last);
    return doc;
}

void test_unavailable_source_is_explicit_even_with_conservative_battery_policy() {
    WifiStatusApiService::StatusRuntime runtime;
    runtime.batteryOnBattery = [](void*) { return true; };
    runtime.batteryHasBattery = [](void*) { return true; };
    runtime.batteryVoltageMv = [](void*) -> uint16_t { return 3800; };
    const auto doc = statusDoc(runtime);
    TEST_ASSERT_EQUAL_STRING("unknown", doc["battery"]["source"]);
    TEST_ASSERT_TRUE(doc["battery"]["on_battery"].as<bool>());
    TEST_ASSERT_TRUE(doc["battery"]["has_battery"].as<bool>());
    TEST_ASSERT_EQUAL_UINT16(3800, doc["battery"]["voltage_mv"]);
}

void test_missing_battery_callbacks_do_not_invent_external_power_source() {
    const auto doc = statusDoc({});
    TEST_ASSERT_EQUAL_STRING("unknown", doc["battery"]["source"]);
    TEST_ASSERT_FALSE(doc["battery"]["has_battery"].as<bool>());
}

void test_explicit_source_is_serialized_independently_of_legacy_policy_bit() {
    for (const char* source : {"unknown", "battery", "usb"}) {
        WifiStatusApiService::StatusRuntime runtime;
        runtime.batterySource = [](void* context) { return static_cast<const char*>(context); };
        runtime.batterySourceCtx = const_cast<char*>(source);
        runtime.batteryOnBattery = [](void*) { return true; };
        const auto doc = statusDoc(runtime);
        TEST_ASSERT_EQUAL_STRING(source, doc["battery"]["source"]);
        TEST_ASSERT_TRUE(doc["battery"]["on_battery"].as<bool>());
    }
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_unavailable_source_is_explicit_even_with_conservative_battery_policy);
    RUN_TEST(test_missing_battery_callbacks_do_not_invent_external_power_source);
    RUN_TEST(test_explicit_source_is_serialized_independently_of_legacy_policy_bit);
    return UNITY_END();
}

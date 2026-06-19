#include <unity.h>
#include <cstring>
#include <vector>

#include "../mocks/mock_heap_caps_state.h"
#include "../mocks/esp_heap_caps.h"
#include "../../src/modules/wifi/wifi_json_document.h"
#include "../../src/modules/wifi/wifi_v1_devices_api_service.h"
#include "../../src/modules/wifi/wifi_v1_devices_api_service.cpp"  // Pull implementation for UNIT_TEST.

#ifndef ARDUINO
SerialClass Serial;
#endif

unsigned long mockMillis = 0;
unsigned long mockMicros = 0;

static bool responseContains(const WebServer& server, const char* needle) {
    return std::strstr(server.lastBody.c_str(), needle) != nullptr;
}

struct FakeRuntime {
    std::vector<WifiV1DevicesApiService::DeviceInfo> devices;

    bool setNameResult = true;
    bool setProfileResult = true;
    bool deleteResult = true;

    int listCalls = 0;
    int setNameCalls = 0;
    int setProfileCalls = 0;
    int deleteCalls = 0;

    String lastAddress;
    String lastName;
    uint8_t lastProfile = 0;
};

static WifiV1DevicesApiService::Runtime makeRuntime(FakeRuntime& rt) {
    return WifiV1DevicesApiService::Runtime{
        [](void* ctx) {
            auto* rtp = static_cast<FakeRuntime*>(ctx);
            rtp->listCalls++;
            return rtp->devices;
        }, &rt,
        [](const String& address, const String& name, void* ctx) {
            auto* rtp = static_cast<FakeRuntime*>(ctx);
            rtp->setNameCalls++;
            rtp->lastAddress = address;
            rtp->lastName = name;
            return rtp->setNameResult;
        }, &rt,
        [](const String& address, uint8_t profile, void* ctx) {
            auto* rtp = static_cast<FakeRuntime*>(ctx);
            rtp->setProfileCalls++;
            rtp->lastAddress = address;
            rtp->lastProfile = profile;
            return rtp->setProfileResult;
        }, &rt,
        [](const String& address, void* ctx) {
            auto* rtp = static_cast<FakeRuntime*>(ctx);
            rtp->deleteCalls++;
            rtp->lastAddress = address;
            return rtp->deleteResult;
        }, &rt,
    };
}

void setUp() {
    mockMillis = 1000;
    mockMicros = 1000000;
    mock_reset_heap_caps();
}

void tearDown() {}

void test_devices_list_returns_records() {
    WebServer server(80);
    FakeRuntime rt;
    rt.devices = {
        {"AA:BB:CC:DD:EE:FF", "Sedan", 2, true},
        {"11:22:33:44:55:66", "", 0, false},
    };

    WifiV1DevicesApiService::handleApiDevicesList(server, makeRuntime(rt));

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"count\":2"));
    TEST_ASSERT_TRUE(responseContains(server, "\"address\":\"AA:BB:CC:DD:EE:FF\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"name\":\"Sedan\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"defaultProfile\":2"));
    TEST_ASSERT_TRUE(responseContains(server, "\"connected\":true"));
    TEST_ASSERT_EQUAL_INT(1, rt.listCalls);
}

void test_devices_list_uses_wifi_json_allocator() {
    WebServer server(80);
    FakeRuntime rt;
    rt.devices = {
        {"AA:BB:CC:DD:EE:01", "Sedan", 1, true},
        {"AA:BB:CC:DD:EE:02", "Truck", 2, false},
        {"AA:BB:CC:DD:EE:03", "Motorcycle", 3, false},
    };

    mock_reset_heap_caps_tracking();
    WifiV1DevicesApiService::handleApiDevicesList(server, makeRuntime(rt));

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_GREATER_THAN_UINT32(0u, g_mock_heap_caps_malloc_calls);
    TEST_ASSERT_EQUAL_UINT32(WifiJson::kPsramCaps, g_mock_heap_caps_last_malloc_caps);
    TEST_ASSERT_EQUAL_UINT32(0u, g_mock_heap_caps_outstanding_allocations);
}

void test_device_name_save_missing_address_returns_400() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("name", "Truck");

    WifiV1DevicesApiService::handleApiDeviceNameSave(
        server,
        makeRuntime(rt),
        [](void* /*ctx*/) { return true; }, nullptr);

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "Missing address"));
    TEST_ASSERT_EQUAL_INT(0, rt.setNameCalls);
}

void test_device_name_save_rate_limited_short_circuits() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("address", "AA:BB:CC:DD:EE:FF");
    server.setArg("name", "Truck");

    WifiV1DevicesApiService::handleApiDeviceNameSave(
        server,
        makeRuntime(rt),
        [](void* /*ctx*/) { return false; }, nullptr);

    TEST_ASSERT_EQUAL_INT(0, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(0, rt.setNameCalls);
}

void test_device_name_save_success_returns_200() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("address", "AA:BB:CC:DD:EE:FF");
    server.setArg("name", "Truck");

    WifiV1DevicesApiService::handleApiDeviceNameSave(
        server,
        makeRuntime(rt),
        [](void* /*ctx*/) { return true; }, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"success\":true"));
    TEST_ASSERT_EQUAL_INT(1, rt.setNameCalls);
    TEST_ASSERT_EQUAL_STRING("AA:BB:CC:DD:EE:FF", rt.lastAddress.c_str());
    TEST_ASSERT_EQUAL_STRING("Truck", rt.lastName.c_str());
}

void test_device_profile_save_invalid_profile_returns_400() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("address", "AA:BB:CC:DD:EE:FF");
    server.setArg("profile", "9");

    WifiV1DevicesApiService::handleApiDeviceProfileSave(
        server,
        makeRuntime(rt),
        [](void* /*ctx*/) { return true; }, nullptr);

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "Invalid profile"));
    TEST_ASSERT_EQUAL_INT(0, rt.setProfileCalls);
}

void test_device_profile_save_success_returns_200() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("address", "AA:BB:CC:DD:EE:FF");
    server.setArg("profile", "3");

    WifiV1DevicesApiService::handleApiDeviceProfileSave(
        server,
        makeRuntime(rt),
        [](void* /*ctx*/) { return true; }, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"success\":true"));
    TEST_ASSERT_EQUAL_INT(1, rt.setProfileCalls);
    TEST_ASSERT_EQUAL_UINT8(3, rt.lastProfile);
}

void test_device_delete_success_returns_200() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("address", "AA:BB:CC:DD:EE:FF");

    WifiV1DevicesApiService::handleApiDeviceDelete(
        server,
        makeRuntime(rt),
        [](void* /*ctx*/) { return true; }, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"success\":true"));
    TEST_ASSERT_EQUAL_INT(1, rt.deleteCalls);
}

void test_device_delete_failure_returns_400() {
    WebServer server(80);
    FakeRuntime rt;
    rt.deleteResult = false;
    server.setArg("address", "AA:BB:CC:DD:EE:FF");

    WifiV1DevicesApiService::handleApiDeviceDelete(
        server,
        makeRuntime(rt),
        [](void* /*ctx*/) { return true; }, nullptr);

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "write failed"));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_devices_list_returns_records);
    RUN_TEST(test_devices_list_uses_wifi_json_allocator);
    RUN_TEST(test_device_name_save_missing_address_returns_400);
    RUN_TEST(test_device_name_save_rate_limited_short_circuits);
    RUN_TEST(test_device_name_save_success_returns_200);
    RUN_TEST(test_device_profile_save_invalid_profile_returns_400);
    RUN_TEST(test_device_profile_save_success_returns_200);
    RUN_TEST(test_device_delete_success_returns_200);
    RUN_TEST(test_device_delete_failure_returns_400);
    return UNITY_END();
}

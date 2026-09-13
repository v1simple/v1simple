#include <unity.h>

#include <cstring>

#include "../../src/modules/wifi/wifi_v1_devices_api_service.h"
#include "../../src/modules/wifi/wifi_v1_devices_api_service.cpp"

#ifndef ARDUINO
SerialClass Serial;
#endif

unsigned long mockMillis = 0;
unsigned long mockMicros = 0;

namespace {

struct FakeRuntime {
    int nameCalls = 0;
    int profileCalls = 0;
    int deleteCalls = 0;
    V1DeviceMutationStatus mutationStatus = V1DeviceMutationStatus::FullyMirrored;
};

WifiV1DevicesApiService::Runtime makeRuntime(FakeRuntime& fake) {
    WifiV1DevicesApiService::Runtime runtime{};
    runtime.setDeviceName = [](const String&, const String&, void* ctx) {
        static_cast<FakeRuntime*>(ctx)->nameCalls++;
        return V1DeviceMutationResult{static_cast<FakeRuntime*>(ctx)->mutationStatus};
    };
    runtime.setDeviceNameCtx = &fake;
    runtime.setDeviceDefaultProfile = [](const String&, uint8_t, void* ctx) {
        static_cast<FakeRuntime*>(ctx)->profileCalls++;
        return V1DeviceMutationResult{static_cast<FakeRuntime*>(ctx)->mutationStatus};
    };
    runtime.setDeviceDefaultProfileCtx = &fake;
    runtime.deleteDevice = [](const String&, void* ctx) {
        static_cast<FakeRuntime*>(ctx)->deleteCalls++;
        return V1DeviceMutationResult{static_cast<FakeRuntime*>(ctx)->mutationStatus};
    };
    runtime.deleteDeviceCtx = &fake;
    return runtime;
}

bool allow(void*) { return true; }

} // namespace

void setUp() {}
void tearDown() {}

void test_exact_device_forms_reject_unknown_fields_before_mutation() {
    struct Case {
        const char* body;
        int operation;
    };
    const Case cases[] = {
        {"address=AA%3ABB%3ACC%3ADD%3AEE%3AFF&name=Road&naem=Typo", 0},
        {"address=AA%3ABB%3ACC%3ADD%3AEE%3AFF&profile=2&profil=1", 1},
        {"address=AA%3ABB%3ACC%3ADD%3AEE%3AFF&force=true", 2},
    };
    for (const auto& test : cases) {
        WebServer server(80);
        FakeRuntime fake;
        const auto runtime = makeRuntime(fake);
        if (test.operation == 0) {
            WifiV1DevicesApiService::handleApiDeviceNameSaveBody(
                server, runtime, reinterpret_cast<const uint8_t*>(test.body), std::strlen(test.body), allow, nullptr);
        } else if (test.operation == 1) {
            WifiV1DevicesApiService::handleApiDeviceProfileSaveBody(
                server, runtime, reinterpret_cast<const uint8_t*>(test.body), std::strlen(test.body), allow, nullptr);
        } else {
            WifiV1DevicesApiService::handleApiDeviceDeleteBody(
                server, runtime, reinterpret_cast<const uint8_t*>(test.body), std::strlen(test.body), allow, nullptr);
        }
        TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
        TEST_ASSERT_EQUAL_INT(0, fake.nameCalls);
        TEST_ASSERT_EQUAL_INT(0, fake.profileCalls);
        TEST_ASSERT_EQUAL_INT(0, fake.deleteCalls);
    }
}

void test_exact_device_name_form_accepts_declared_fields() {
    const char body[] = "address=AA%3ABB%3ACC%3ADD%3AEE%3AFF&name=Road+V1";
    WebServer server(80);
    FakeRuntime fake;
    WifiV1DevicesApiService::handleApiDeviceNameSaveBody(
        server, makeRuntime(fake), reinterpret_cast<const uint8_t*>(body), sizeof(body) - 1u, allow, nullptr);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(1, fake.nameCalls);
}

void test_shipped_ui_multipart_device_mutations_remain_compatible() {
    constexpr char boundary[] = "legacy-v203";
    const char nameBody[] =
        "--legacy-v203\r\nContent-Disposition: form-data; name=\"address\"\r\n\r\nAA:BB:CC:DD:EE:FF\r\n"
        "--legacy-v203\r\nContent-Disposition: form-data; name=\"name\"\r\n\r\nRoad V1\r\n"
        "--legacy-v203--\r\n";
    const char profileBody[] =
        "--legacy-v203\r\nContent-Disposition: form-data; name=\"address\"\r\n\r\nAA:BB:CC:DD:EE:FF\r\n"
        "--legacy-v203\r\nContent-Disposition: form-data; name=\"profile\"\r\n\r\n2\r\n"
        "--legacy-v203--\r\n";
    const char deleteBody[] =
        "--legacy-v203\r\nContent-Disposition: form-data; name=\"address\"\r\n\r\nAA:BB:CC:DD:EE:FF\r\n"
        "--legacy-v203--\r\n";

    WebServer server(80);
    FakeRuntime fake;
    const auto runtime = makeRuntime(fake);
    WifiV1DevicesApiService::handleApiDeviceNameSaveBody(
        server, runtime, reinterpret_cast<const uint8_t*>(nameBody), sizeof(nameBody) - 1u,
        allow, nullptr, boundary, sizeof(boundary) - 1u);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    WifiV1DevicesApiService::handleApiDeviceProfileSaveBody(
        server, runtime, reinterpret_cast<const uint8_t*>(profileBody), sizeof(profileBody) - 1u,
        allow, nullptr, boundary, sizeof(boundary) - 1u);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    WifiV1DevicesApiService::handleApiDeviceDeleteBody(
        server, runtime, reinterpret_cast<const uint8_t*>(deleteBody), sizeof(deleteBody) - 1u,
        allow, nullptr, boundary, sizeof(boundary) - 1u);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(1, fake.nameCalls);
    TEST_ASSERT_EQUAL_INT(1, fake.profileCalls);
    TEST_ASSERT_EQUAL_INT(1, fake.deleteCalls);
}

void test_device_name_requires_exact_trimmed_utf8_byte_boundary() {
    const char* invalidBodies[] = {
        "address=AA%3ABB%3ACC%3ADD%3AEE%3AFF&name=%20Road",
        "address=AA%3ABB%3ACC%3ADD%3AEE%3AFF&name=Road%20",
        "address=AA%3ABB%3ACC%3ADD%3AEE%3AFF&name=123456789012345678901234567890123",
        "address=AA%3ABB%3ACC%3ADD%3AEE%3AFF&name=%C3%A9%C3%A9%C3%A9%C3%A9%C3%A9%C3%A9%C3%A9%C3%A9%C3%A9%C3%A9%C3%A9%C3%A9%C3%A9%C3%A9%C3%A9%C3%A9%C3%A9",
    };
    for (const char* body : invalidBodies) {
        WebServer server(80);
        FakeRuntime fake;
        WifiV1DevicesApiService::handleApiDeviceNameSaveBody(
            server, makeRuntime(fake), reinterpret_cast<const uint8_t*>(body), std::strlen(body), allow, nullptr);
        TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
        TEST_ASSERT_EQUAL_INT(0, fake.nameCalls);
    }

    const char valid[] =
        "address=AA%3ABB%3ACC%3ADD%3AEE%3AFF&name=%C3%A9%C3%A9%C3%A9%C3%A9%C3%A9%C3%A9%C3%A9%C3%A9%C3%A9%C3%A9%C3%A9%C3%A9%C3%A9%C3%A9%C3%A9%C3%A9";
    WebServer server(80);
    FakeRuntime fake;
    WifiV1DevicesApiService::handleApiDeviceNameSaveBody(
        server, makeRuntime(fake), reinterpret_cast<const uint8_t*>(valid), sizeof(valid) - 1u, allow, nullptr);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(1, fake.nameCalls);
}

void test_device_mutation_results_distinguish_pending_busy_and_uncommitted() {
    const char body[] = "address=AA%3ABB%3ACC%3ADD%3AEE%3AFF";
    struct Case {
        V1DeviceMutationStatus status;
        int http;
        const char* bodyFragment;
    };
    const Case cases[] = {
        {V1DeviceMutationStatus::DurablePending, 202, "operationPending"},
        {V1DeviceMutationStatus::PrimaryCommittedMirrorPending, 202, "mirrorSyncPending"},
        {V1DeviceMutationStatus::Busy, 409, "Another device deletion is pending"},
        {V1DeviceMutationStatus::NotCommitted, 503, "not committed"},
    };
    for (const auto& test : cases) {
        WebServer server(80);
        FakeRuntime fake;
        fake.mutationStatus = test.status;
        WifiV1DevicesApiService::handleApiDeviceDeleteBody(
            server, makeRuntime(fake), reinterpret_cast<const uint8_t*>(body), sizeof(body) - 1u, allow, nullptr);
        TEST_ASSERT_EQUAL_INT(test.http, server.lastStatusCode);
        TEST_ASSERT_NOT_EQUAL(nullptr, std::strstr(server.lastBody.c_str(), test.bodyFragment));
        TEST_ASSERT_EQUAL_INT(1, fake.deleteCalls);
    }
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_exact_device_forms_reject_unknown_fields_before_mutation);
    RUN_TEST(test_exact_device_name_form_accepts_declared_fields);
    RUN_TEST(test_shipped_ui_multipart_device_mutations_remain_compatible);
    RUN_TEST(test_device_name_requires_exact_trimmed_utf8_byte_boundary);
    RUN_TEST(test_device_mutation_results_distinguish_pending_busy_and_uncommitted);
    return UNITY_END();
}

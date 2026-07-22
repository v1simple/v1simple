#include <unity.h>

#include <array>
#include <cstdio>
#include <cstring>

#define V1SIMPLE_HIL_FAULT_CONTROL 1
#include "../../src/modules/hil/hil_fault_controller.cpp"
#include "../../src/modules/hil/hil_fault_serial_module.cpp"
#include "../../src/modules/ble/ble_notification_delay_gate.cpp"
#include "../../src/modules/ble/ble_bsc05_hil_fault_module.cpp"

namespace {
constexpr char kSessionHashText[] = "6162636465666768696a6b6c6d6e6f707172737475767778797a7b7c7d7e7f80";

struct Fixture {
    char output[8192]{};
    size_t outputLength = 0;
    uint32_t openGeneration = 0;
    uint32_t forwardCalls = 0;
    uint32_t forwardedGeneration = 0;
    uint16_t forwardedCharacteristic = 0;
    std::array<uint8_t, 256> forwardedData{};
    size_t forwardedLength = 0;
    bool forceAccept = false;
};

void writeEvidence(const char* text, void* context) noexcept {
    auto& fixture = *static_cast<Fixture*>(context);
    const size_t length = text == nullptr ? 0 : std::strlen(text);
    const size_t available = sizeof(fixture.output) - fixture.outputLength - 1;
    const size_t copied = length < available ? length : available;
    std::memcpy(fixture.output + fixture.outputLength, text, copied);
    fixture.outputLength += copied;
}

bool forwardNotification(const uint8_t* data, const size_t length, const uint16_t characteristicUuid,
                         const uint32_t generation, void* context) noexcept {
    auto& fixture = *static_cast<Fixture*>(context);
    ++fixture.forwardCalls;
    fixture.forwardedGeneration = generation;
    fixture.forwardedCharacteristic = characteristicUuid;
    fixture.forwardedLength = length;
    if (data != nullptr && length <= fixture.forwardedData.size()) {
        std::memcpy(fixture.forwardedData.data(), data, length);
    }
    return fixture.forceAccept || generation == fixture.openGeneration;
}

void feed(HilFaultRuntimeOwner& owner, const char* command, const uint32_t nowMs) {
    for (const char* cursor = command; *cursor != '\0'; ++cursor) {
        owner.acceptSerialByte(*cursor, nowMs);
    }
    owner.acceptSerialByte('\n', nowMs);
}

void beginAndArm(HilFaultRuntimeOwner& owner, const uint32_t nowMs) {
    char command[224]{};
    std::snprintf(command, sizeof(command), "V1HIL BEGIN BSC-05 %s 60000", kSessionHashText);
    feed(owner, command, nowMs);
    std::snprintf(command, sizeof(command), "V1HIL ARM BSC-05 v1-notification-delay-once %s 15", kSessionHashText);
    feed(owner, command, nowMs + 1);
}

BleBsc05HilRuntime runtime(Fixture& fixture) {
    return {forwardNotification, &fixture, writeEvidence, &fixture};
}
} // namespace

void test_unarmed_notification_is_forwarded_unchanged() {
    Fixture fixture{};
    HilFaultRuntimeOwner owner;
    BleBsc05HilFaultModule module(owner, runtime(fixture));
    const uint8_t data[] = {1, 2, 3};
    TEST_ASSERT_TRUE(module.routeNotification(data, sizeof(data), 0xB2CE, 3, 100));
    TEST_ASSERT_EQUAL_UINT32(0, fixture.forwardCalls);
    TEST_ASSERT_EQUAL_STRING("", fixture.output);
}

void test_armed_notification_releases_only_after_close_and_new_open() {
    Fixture fixture{};
    HilFaultRuntimeOwner owner;
    BleBsc05HilFaultModule module(owner, runtime(fixture));
    beginAndArm(owner, 100);
    module.service(102);
    TEST_ASSERT_TRUE(module.snapshot().armed);

    const uint8_t data[] = {7, 8, 9, 10};
    TEST_ASSERT_FALSE(module.routeNotification(data, sizeof(data), 0xB2CE, 40, 103));
    TEST_ASSERT_EQUAL_STRING("", fixture.output);
    module.service(104);
    TEST_ASSERT_NOT_NULL(std::strstr(fixture.output, "\"hil_event\":\"ready\""));
    TEST_ASSERT_NOT_NULL(std::strstr(fixture.output, "\"hil_event\":\"fired\""));
    TEST_ASSERT_EQUAL_UINT32(0, fixture.forwardCalls);

    module.recordSessionOpened(41, 105);
    module.service(105);
    TEST_ASSERT_EQUAL_UINT32(0, fixture.forwardCalls);
    module.recordSessionClosed(39, 106);
    module.service(106);
    TEST_ASSERT_EQUAL_UINT32(0, fixture.forwardCalls);
    module.recordSessionClosed(40, 107);
    module.recordSessionOpened(40, 108);
    module.service(108);
    TEST_ASSERT_EQUAL_UINT32(0, fixture.forwardCalls);

    fixture.openGeneration = 41;
    module.recordSessionOpened(41, 109);
    module.service(109);
    TEST_ASSERT_EQUAL_UINT32(1, fixture.forwardCalls);
    TEST_ASSERT_EQUAL_UINT32(40, fixture.forwardedGeneration);
    TEST_ASSERT_EQUAL_UINT16(0xB2CE, fixture.forwardedCharacteristic);
    TEST_ASSERT_EQUAL_UINT32(sizeof(data), fixture.forwardedLength);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(data, fixture.forwardedData.data(), sizeof(data));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilFaultState::Released),
                            static_cast<uint8_t>(module.controllerSnapshot().state));
    const BleBsc05HilSnapshot snapshot = module.snapshot();
    TEST_ASSERT_TRUE(snapshot.releaseAttempted);
    TEST_ASSERT_TRUE(snapshot.wrongGenerationRejected);
    TEST_ASSERT_EQUAL_UINT32(40, snapshot.oldGeneration);
    TEST_ASSERT_EQUAL_UINT32(41, snapshot.newGeneration);
    TEST_ASSERT_NOT_NULL(std::strstr(fixture.output, "\"hil_event\":\"released\""));
    TEST_ASSERT_NOT_NULL(std::strstr(fixture.output, "\"characteristic_class\":\"display\""));
    TEST_ASSERT_NOT_NULL(std::strstr(fixture.output, "\"old_session_closed_at_ms\":107"));
    TEST_ASSERT_NOT_NULL(std::strstr(fixture.output, "\"new_session_opened_at_ms\":109"));
    TEST_ASSERT_NOT_NULL(std::strstr(fixture.output, "\"wrong_generation_rejected\":true"));
}

void test_timeout_discards_copy_without_forwarding() {
    Fixture fixture{};
    HilFaultRuntimeOwner owner;
    BleBsc05HilFaultModule module(owner, runtime(fixture));
    beginAndArm(owner, 200);
    module.service(202);
    const uint8_t data[] = {3, 4};
    TEST_ASSERT_FALSE(module.routeNotification(data, sizeof(data), 0xB4E0, 50, 203));
    module.service(203 + BleBsc05HilFaultModule::kAutomaticReleaseMs);
    TEST_ASSERT_EQUAL_UINT32(0, fixture.forwardCalls);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilFaultState::Expired),
                            static_cast<uint8_t>(module.controllerSnapshot().state));
    TEST_ASSERT_NOT_NULL(std::strstr(fixture.output, "automatic_timeout_discarded_copy"));
    TEST_ASSERT_NOT_NULL(std::strstr(fixture.output, "\"characteristic_class\":\"long\""));
}

void test_session_end_discards_copy_without_forwarding() {
    Fixture fixture{};
    HilFaultRuntimeOwner owner;
    BleBsc05HilFaultModule module(owner, runtime(fixture));
    beginAndArm(owner, 300);
    module.service(302);
    const uint8_t data = 5;
    TEST_ASSERT_FALSE(module.routeNotification(&data, 1, 0xB2CE, 60, 303));

    char command[192]{};
    std::snprintf(command, sizeof(command), "V1HIL END BSC-05 %s", kSessionHashText);
    feed(owner, command, 304);
    module.service(304);
    TEST_ASSERT_EQUAL_UINT32(0, fixture.forwardCalls);
    TEST_ASSERT_FALSE(module.snapshot().notificationCaptured);
    TEST_ASSERT_NOT_NULL(std::strstr(fixture.output, "session_end_discarded_copy"));
}

void test_unexpected_old_generation_acceptance_blocks_release() {
    Fixture fixture{};
    fixture.forceAccept = true;
    HilFaultRuntimeOwner owner;
    BleBsc05HilFaultModule module(owner, runtime(fixture));
    beginAndArm(owner, 400);
    module.service(402);
    const uint8_t data = 6;
    TEST_ASSERT_FALSE(module.routeNotification(&data, 1, 0xB2CE, 70, 403));
    module.recordSessionClosed(70, 404);
    module.recordSessionOpened(71, 405);
    module.service(405);
    TEST_ASSERT_EQUAL_UINT32(1, fixture.forwardCalls);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilFaultState::Fired),
                            static_cast<uint8_t>(module.controllerSnapshot().state));
    TEST_ASSERT_NOT_NULL(std::strstr(fixture.output, "old_generation_was_unexpectedly_accepted"));
    TEST_ASSERT_NULL(std::strstr(fixture.output, "\"hil_event\":\"released\""));
}

void test_missing_queue_adapter_blocks_release() {
    Fixture fixture{};
    HilFaultRuntimeOwner owner;
    BleBsc05HilRuntime missingAdapter{};
    missingAdapter.writeEvidence = writeEvidence;
    missingAdapter.evidenceContext = &fixture;
    BleBsc05HilFaultModule module(owner, missingAdapter);
    beginAndArm(owner, 500);
    module.service(502);
    const uint8_t data = 7;
    TEST_ASSERT_FALSE(module.routeNotification(&data, 1, 0xB2CE, 80, 503));
    module.recordSessionClosed(80, 504);
    module.recordSessionOpened(81, 505);
    module.service(505);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilFaultState::Fired),
                            static_cast<uint8_t>(module.controllerSnapshot().state));
    TEST_ASSERT_NOT_NULL(std::strstr(fixture.output, "notification_queue_adapter_unavailable"));
    TEST_ASSERT_NULL(std::strstr(fixture.output, "\"hil_event\":\"released\""));
}

int main(int argc, char** argv) {
    UNITY_BEGIN();
    RUN_TEST(test_unarmed_notification_is_forwarded_unchanged);
    RUN_TEST(test_armed_notification_releases_only_after_close_and_new_open);
    RUN_TEST(test_timeout_discards_copy_without_forwarding);
    RUN_TEST(test_session_end_discards_copy_without_forwarding);
    RUN_TEST(test_unexpected_old_generation_acceptance_blocks_release);
    RUN_TEST(test_missing_queue_adapter_blocks_release);
    return UNITY_END();
}

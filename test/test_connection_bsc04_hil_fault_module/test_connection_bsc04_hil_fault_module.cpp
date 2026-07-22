#include <unity.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#define V1SIMPLE_HIL_FAULT_CONTROL 1
#include "../../src/modules/hil/hil_fault_controller.cpp"
#include "../../src/modules/hil/hil_fault_serial_module.cpp"
#include "../../src/modules/system/connection_bsc04_hil_fault_module.cpp"

namespace {
constexpr char kSessionHashText[] = "3132333435363738393a3b3c3d3e3f404142434445464748494a4b4c4d4e4f50";

struct Fixture {
    char output[4096]{};
    size_t outputLength = 0;
};

void writeEvidence(const char* text, void* context) noexcept {
    Fixture& fixture = *static_cast<Fixture*>(context);
    if (text == nullptr) {
        return;
    }
    const size_t length = std::strlen(text);
    const size_t available = sizeof(fixture.output) - fixture.outputLength - 1;
    const size_t copyLength = length < available ? length : available;
    std::memcpy(fixture.output + fixture.outputLength, text, copyLength);
    fixture.outputLength += copyLength;
    fixture.output[fixture.outputLength] = '\0';
}

ConnectionBsc04HilRuntime runtimeFor(Fixture& fixture) {
    ConnectionBsc04HilRuntime runtime{};
    runtime.writeEvidence = writeEvidence;
    runtime.context = &fixture;
    return runtime;
}

void feed(HilFaultRuntimeOwner& owner, const char* command, const uint32_t nowMs) {
    for (const char* cursor = command; *cursor != '\0'; ++cursor) {
        owner.acceptSerialByte(*cursor, nowMs);
    }
    owner.acceptSerialByte('\n', nowMs);
}

void beginAndArm(HilFaultRuntimeOwner& owner, const uint32_t armSequence, const uint32_t nowMs) {
    char command[224]{};
    std::snprintf(command, sizeof(command), "V1HIL BEGIN BSC-04 %s 60000", kSessionHashText);
    feed(owner, command, nowMs);
    std::snprintf(command, sizeof(command), "V1HIL ARM BSC-04 v1-verify-push-suppress-once %s %lu", kSessionHashText,
                  static_cast<unsigned long>(armSequence));
    feed(owner, command, nowMs + 1);
}

ConnectionBsc04VerifyPushAdmission settlingEdge() {
    ConnectionBsc04VerifyPushAdmission admission{};
    admission.verifyPushMatchEdge = true;
    admission.v1GattConnected = true;
    admission.coordinatorStateCode = ConnectionBsc04HilFaultModule::kV1SettlingStateCode;
    return admission;
}
} // namespace

void test_unarmed_verify_push_edge_is_forwarded_without_mutation() {
    Fixture fixture{};
    HilFaultRuntimeOwner owner;
    ConnectionBsc04HilFaultModule module(owner, runtimeFor(fixture));

    TEST_ASSERT_TRUE(module.routeVerifyPushMatchEdge(settlingEdge(), 100));
    TEST_ASSERT_FALSE(module.snapshot().admissionAttempted);
    TEST_ASSERT_FALSE(module.snapshot().suppressionApplied);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilFaultState::Disarmed),
                            static_cast<uint8_t>(module.controllerSnapshot().state));
    TEST_ASSERT_EQUAL_STRING("", fixture.output);
}

void test_armed_edge_is_suppressed_once_with_typed_lifecycle_evidence() {
    Fixture fixture{};
    HilFaultRuntimeOwner owner;
    ConnectionBsc04HilFaultModule module(owner, runtimeFor(fixture));
    beginAndArm(owner, 7, 100);

    TEST_ASSERT_FALSE(module.routeVerifyPushMatchEdge(settlingEdge(), 102));
    TEST_ASSERT_TRUE(module.snapshot().admissionAttempted);
    TEST_ASSERT_TRUE(module.snapshot().suppressionApplied);
    TEST_ASSERT_EQUAL_UINT32(7, module.snapshot().armSequence);
    TEST_ASSERT_NOT_EQUAL(0, module.snapshot().readySequence);
    TEST_ASSERT_NOT_EQUAL(0, module.snapshot().generation);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilFaultState::Released),
                            static_cast<uint8_t>(module.controllerSnapshot().state));
    TEST_ASSERT_NOT_NULL(std::strstr(fixture.output, "\"hil_event\":\"ready\""));
    TEST_ASSERT_NOT_NULL(std::strstr(fixture.output, "\"hil_event\":\"fired\""));
    TEST_ASSERT_NOT_NULL(std::strstr(fixture.output, "\"hil_event\":\"released\""));
    TEST_ASSERT_NOT_NULL(std::strstr(fixture.output, "\"coordinator_state\":\"V1_SETTLING\""));
    TEST_ASSERT_NOT_NULL(std::strstr(fixture.output, "\"raw_verify_push_edge\":true"));
    TEST_ASSERT_NOT_NULL(std::strstr(fixture.output, "\"forwarded_verify_push_edge\":false"));

    TEST_ASSERT_TRUE(module.routeVerifyPushMatchEdge(settlingEdge(), 103));
}

void test_admission_waits_for_connected_settling_edge() {
    Fixture fixture{};
    HilFaultRuntimeOwner owner;
    ConnectionBsc04HilFaultModule module(owner, runtimeFor(fixture));
    beginAndArm(owner, 9, 200);

    ConnectionBsc04VerifyPushAdmission admission = settlingEdge();
    admission.verifyPushMatchEdge = false;
    TEST_ASSERT_FALSE(module.routeVerifyPushMatchEdge(admission, 202));
    admission = settlingEdge();
    admission.v1GattConnected = false;
    TEST_ASSERT_TRUE(module.routeVerifyPushMatchEdge(admission, 203));
    admission = settlingEdge();
    admission.coordinatorStateCode = 7;
    TEST_ASSERT_TRUE(module.routeVerifyPushMatchEdge(admission, 204));
    TEST_ASSERT_FALSE(module.snapshot().admissionAttempted);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilFaultState::Armed),
                            static_cast<uint8_t>(module.controllerSnapshot().state));

    TEST_ASSERT_FALSE(module.routeVerifyPushMatchEdge(settlingEdge(), 205));
    TEST_ASSERT_TRUE(module.snapshot().suppressionApplied);
}

void test_expired_fault_never_suppresses_product_edge() {
    Fixture fixture{};
    HilFaultRuntimeOwner owner;
    ConnectionBsc04HilFaultModule module(owner, runtimeFor(fixture));
    beginAndArm(owner, 4, 100);
    module.service(60101);

    TEST_ASSERT_TRUE(module.routeVerifyPushMatchEdge(settlingEdge(), 60102));
    TEST_ASSERT_FALSE(module.snapshot().admissionAttempted);
    TEST_ASSERT_FALSE(module.snapshot().suppressionApplied);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilFaultState::Disarmed),
                            static_cast<uint8_t>(module.controllerSnapshot().state));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_unarmed_verify_push_edge_is_forwarded_without_mutation);
    RUN_TEST(test_armed_edge_is_suppressed_once_with_typed_lifecycle_evidence);
    RUN_TEST(test_admission_waits_for_connected_settling_edge);
    RUN_TEST(test_expired_fault_never_suppresses_product_edge);
    return UNITY_END();
}

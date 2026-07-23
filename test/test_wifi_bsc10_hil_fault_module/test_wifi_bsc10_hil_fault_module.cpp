#include <unity.h>

#include <cstdio>
#include <cstring>

#define V1SIMPLE_HIL_FAULT_CONTROL 1
#include "../../src/modules/hil/hil_fault_controller.cpp"
#include "../../src/modules/hil/hil_fault_serial_module.cpp"
#include "../../src/modules/wifi/wifi_bsc10_hil_fault_module.cpp"

namespace {
constexpr char kSessionHashText[] = "5152535455565758595a5b5c5d5e5f606162636465666768696a6b6c6d6e6f70";

struct Fixture {
    char output[4096]{};
    size_t outputLength = 0;
};

void writeEvidence(const char* text, void* context) noexcept {
    auto& fixture = *static_cast<Fixture*>(context);
    const size_t length = text == nullptr ? 0 : std::strlen(text);
    const size_t available = sizeof(fixture.output) - fixture.outputLength - 1;
    const size_t copied = length < available ? length : available;
    std::memcpy(fixture.output + fixture.outputLength, text, copied);
    fixture.outputLength += copied;
}

void feed(HilFaultRuntimeOwner& owner, const char* command, const uint32_t nowMs) {
    for (const char* cursor = command; *cursor != '\0'; ++cursor) {
        owner.acceptSerialByte(*cursor, nowMs);
    }
    owner.acceptSerialByte('\n', nowMs);
}

void beginAndArm(HilFaultRuntimeOwner& owner, const uint32_t nowMs) {
    char command[224]{};
    std::snprintf(command, sizeof(command), "V1HIL BEGIN BSC-10 %s 60000", kSessionHashText);
    feed(owner, command, nowMs);
    std::snprintf(command, sizeof(command), "V1HIL ARM BSC-10 wifi-enable-admission-fail-once %s 9", kSessionHashText);
    feed(owner, command, nowMs + 1);
}
} // namespace

void test_unarmed_admission_is_unchanged() {
    Fixture fixture{};
    HilFaultRuntimeOwner owner;
    WifiBsc10HilRuntime runtime{writeEvidence, &fixture};
    WifiBsc10HilFaultModule module(owner, runtime);

    TEST_ASSERT_TRUE(module.admitLifecycleStart({false, 2, 1}, 100));
    TEST_ASSERT_FALSE(module.snapshot().admissionAttempted);
    TEST_ASSERT_EQUAL_STRING("", fixture.output);
}

void test_armed_admission_fails_once_before_mutation() {
    Fixture fixture{};
    HilFaultRuntimeOwner owner;
    WifiBsc10HilRuntime runtime{writeEvidence, &fixture};
    WifiBsc10HilFaultModule module(owner, runtime);
    beginAndArm(owner, 100);

    TEST_ASSERT_FALSE(module.admitLifecycleStart({false, 2, 1}, 102));
    TEST_ASSERT_TRUE(module.snapshot().admissionAttempted);
    TEST_ASSERT_TRUE(module.snapshot().rejectionApplied);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilFaultState::Released),
                            static_cast<uint8_t>(module.controllerSnapshot().state));
    TEST_ASSERT_NOT_NULL(std::strstr(fixture.output, "\"hil_event\":\"ready\""));
    TEST_ASSERT_NOT_NULL(std::strstr(fixture.output, "\"hil_event\":\"fired\""));
    TEST_ASSERT_NOT_NULL(std::strstr(fixture.output, "\"hil_event\":\"released\""));
    TEST_ASSERT_NOT_NULL(std::strstr(fixture.output, "\"post_span_mutation_count\":0"));
    TEST_ASSERT_TRUE(module.admitLifecycleStart({false, 2, 1}, 103));
}

void test_expired_arm_never_changes_admission() {
    Fixture fixture{};
    HilFaultRuntimeOwner owner;
    WifiBsc10HilRuntime runtime{writeEvidence, &fixture};
    WifiBsc10HilFaultModule module(owner, runtime);
    beginAndArm(owner, 100);
    module.service(60101);

    TEST_ASSERT_TRUE(module.admitLifecycleStart({false, 2, 1}, 60102));
    TEST_ASSERT_FALSE(module.snapshot().admissionAttempted);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_unarmed_admission_is_unchanged);
    RUN_TEST(test_armed_admission_fails_once_before_mutation);
    RUN_TEST(test_expired_arm_never_changes_admission);
    return UNITY_END();
}

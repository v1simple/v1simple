#include <unity.h>

#include <cstdio>
#include <cstring>

#define V1SIMPLE_HIL_FAULT_CONTROL 1
#include "../../src/modules/hil/hil_fault_controller.cpp"
#include "../../src/modules/hil/hil_fault_serial_module.cpp"
#include "../../src/modules/obd/obd_physical_link_preownership_barrier.cpp"
#include "../../src/modules/obd/obd_bsc13_hil_fault_module.cpp"

namespace {
constexpr char kSessionHashText[] = "9192939495969798999a9b9c9d9e9fa0a1a2a3a4a5a6a7a8a9aaabacadaeafb0";

struct Fixture {
    uint32_t nowMs = 100;
    uint32_t cancellationEpoch = 20;
    uint32_t linkDownGeneration = 0;
    char output[8192]{};
    size_t outputLength = 0;
};

void writeEvidence(const char* text, void* context) noexcept {
    auto& fixture = *static_cast<Fixture*>(context);
    const size_t length = text == nullptr ? 0 : std::strlen(text);
    const size_t available = sizeof(fixture.output) - fixture.outputLength - 1;
    const size_t copied = length < available ? length : available;
    std::memcpy(fixture.output + fixture.outputLength, text, copied);
    fixture.outputLength += copied;
    fixture.output[fixture.outputLength] = '\0';
}

uint32_t clockMs(void* context) noexcept {
    return static_cast<Fixture*>(context)->nowMs;
}

uint32_t cancellationEpoch(void* context) noexcept {
    return static_cast<Fixture*>(context)->cancellationEpoch;
}

bool linkDownConfirmed(const uint32_t generation, void* context) noexcept {
    return static_cast<Fixture*>(context)->linkDownGeneration == generation;
}

ObdBsc13HilRuntime runtimeFor(Fixture& fixture) {
    ObdBsc13HilRuntime runtime{};
    runtime.barrier = {clockMs, cancellationEpoch, linkDownConfirmed, &fixture};
    runtime.writeEvidence = writeEvidence;
    runtime.evidenceContext = &fixture;
    return runtime;
}

void feed(HilFaultRuntimeOwner& owner, const char* command, uint32_t nowMs) {
    for (const char* cursor = command; *cursor != '\0'; ++cursor) {
        owner.acceptSerialByte(*cursor, nowMs);
    }
    owner.acceptSerialByte('\n', nowMs);
}

void beginAndArm(HilFaultRuntimeOwner& owner, uint32_t nowMs) {
    char command[224]{};
    std::snprintf(command, sizeof(command), "V1HIL BEGIN BSC-13 %s 60000", kSessionHashText);
    feed(owner, command, nowMs);
    std::snprintf(command, sizeof(command), "V1HIL ARM BSC-13 obd-physical-link-preownership-barrier-once %s 13",
                  kSessionHashText);
    feed(owner, command, nowMs + 1);
}

void endSession(HilFaultRuntimeOwner& owner, uint32_t nowMs) {
    char command[192]{};
    std::snprintf(command, sizeof(command), "V1HIL END BSC-13 %s", kSessionHashText);
    feed(owner, command, nowMs);
}

ObdBsc13HilAdmission physicalConnect(uint32_t generation = 31) {
    return {true, generation, ObdBsc13HilFaultModule::kConnectingRuntimeStateCode};
}

void initializeArmed(Fixture& fixture, HilFaultRuntimeOwner& owner, ObdBsc13HilFaultModule& module) {
    beginAndArm(owner, fixture.nowMs);
    fixture.nowMs += 2;
    module.service(fixture.nowMs);
    TEST_ASSERT_TRUE(module.snapshot().armed);
}
} // namespace

void test_unarmed_invalid_or_nonconnecting_admission_is_neutral() {
    Fixture fixture;
    HilFaultRuntimeOwner owner;
    ObdBsc13HilFaultModule module(owner, runtimeFor(fixture));
    TEST_ASSERT_TRUE(module.admitSessionOwnership(physicalConnect(), fixture.nowMs));

    initializeArmed(fixture, owner, module);
    auto wrongState = physicalConnect();
    wrongState.runtimeStateCode = 8;
    TEST_ASSERT_TRUE(module.admitSessionOwnership(wrongState, fixture.nowMs));
    TEST_ASSERT_TRUE(module.snapshot().armed);
    TEST_ASSERT_EQUAL_STRING("", fixture.output);
}

void test_ready_barrier_holds_only_session_adoption_at_physical_connect_edge() {
    Fixture fixture;
    HilFaultRuntimeOwner owner;
    ObdBsc13HilFaultModule module(owner, runtimeFor(fixture));
    initializeArmed(fixture, owner, module);

    TEST_ASSERT_FALSE(module.admitSessionOwnership(physicalConnect(), fixture.nowMs));
    const auto snapshot = module.snapshot();
    TEST_ASSERT_TRUE(snapshot.barrierActive);
    TEST_ASSERT_EQUAL_UINT32(31, snapshot.activeGeneration);
    TEST_ASSERT_EQUAL_UINT32(20, snapshot.dispatchEpoch);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilFaultState::Fired),
                            static_cast<uint8_t>(module.controllerSnapshot().state));
    TEST_ASSERT_NOT_NULL(std::strstr(fixture.output, "\"hil_event\":\"ready\""));
    TEST_ASSERT_NOT_NULL(std::strstr(fixture.output, "physical_link_before_session_ownership"));
    TEST_ASSERT_NOT_NULL(std::strstr(fixture.output, "session_ownership_held"));
}

void test_cancellation_alone_does_not_release_until_matching_callback_down() {
    Fixture fixture;
    HilFaultRuntimeOwner owner;
    ObdBsc13HilFaultModule module(owner, runtimeFor(fixture));
    initializeArmed(fixture, owner, module);
    TEST_ASSERT_FALSE(module.admitSessionOwnership(physicalConnect(), fixture.nowMs));

    fixture.cancellationEpoch += 2;
    fixture.nowMs += 4;
    module.service(fixture.nowMs);
    TEST_ASSERT_TRUE(module.snapshot().barrierActive);
    TEST_ASSERT_TRUE(module.snapshot().cancellationObserved);
    TEST_ASSERT_FALSE(module.snapshot().linkDownConfirmed);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilFaultState::Fired),
                            static_cast<uint8_t>(module.controllerSnapshot().state));

    fixture.linkDownGeneration = 31;
    ++fixture.nowMs;
    module.service(fixture.nowMs);
    const auto snapshot = module.snapshot();
    TEST_ASSERT_TRUE(snapshot.completionRecorded);
    TEST_ASSERT_TRUE(snapshot.linkDownConfirmed);
    TEST_ASSERT_TRUE(snapshot.controllerReleaseRecorded);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilFaultState::Released),
                            static_cast<uint8_t>(module.controllerSnapshot().state));
    TEST_ASSERT_NOT_NULL(std::strstr(fixture.output, "matching_generation_callback_down_after_preemption"));
    TEST_ASSERT_NOT_NULL(std::strstr(fixture.output, "\"matching_link_down_confirmed\":true"));
}

void test_link_down_without_cancellation_never_qualifies_release() {
    Fixture fixture;
    HilFaultRuntimeOwner owner;
    ObdBsc13HilFaultModule module(owner, runtimeFor(fixture));
    initializeArmed(fixture, owner, module);
    TEST_ASSERT_FALSE(module.admitSessionOwnership(physicalConnect(), fixture.nowMs));

    fixture.linkDownGeneration = 31;
    ++fixture.nowMs;
    module.service(fixture.nowMs);
    TEST_ASSERT_TRUE(module.snapshot().completionRecorded);
    TEST_ASSERT_FALSE(module.snapshot().controllerReleaseRecorded);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilFaultState::Fired),
                            static_cast<uint8_t>(module.controllerSnapshot().state));
    TEST_ASSERT_NOT_NULL(std::strstr(fixture.output, "matching_link_down_without_preemption"));
    TEST_ASSERT_NULL(std::strstr(fixture.output, "\"hil_event\":\"released\""));
}

void test_confirmed_preemption_rejects_captured_link_but_admits_one_later_generation() {
    Fixture fixture;
    HilFaultRuntimeOwner owner;
    ObdBsc13HilFaultModule module(owner, runtimeFor(fixture));
    initializeArmed(fixture, owner, module);
    TEST_ASSERT_FALSE(module.admitSessionOwnership(physicalConnect(31), fixture.nowMs));

    fixture.cancellationEpoch += 2;
    fixture.linkDownGeneration = 31;
    ++fixture.nowMs;
    module.service(fixture.nowMs);
    TEST_ASSERT_TRUE(module.snapshot().controllerReleaseRecorded);
    TEST_ASSERT_FALSE(module.admitSessionOwnership(physicalConnect(31), fixture.nowMs));

    fixture.linkDownGeneration = 0;
    ++fixture.nowMs;
    TEST_ASSERT_TRUE(module.admitSessionOwnership(physicalConnect(32), fixture.nowMs));
    TEST_ASSERT_EQUAL_UINT32(31, module.snapshot().activeGeneration);
}

void test_unexpected_down_rejects_captured_link_but_does_not_strand_new_generation() {
    Fixture fixture;
    HilFaultRuntimeOwner owner;
    ObdBsc13HilFaultModule module(owner, runtimeFor(fixture));
    initializeArmed(fixture, owner, module);
    TEST_ASSERT_FALSE(module.admitSessionOwnership(physicalConnect(41), fixture.nowMs));

    fixture.linkDownGeneration = 41;
    ++fixture.nowMs;
    module.service(fixture.nowMs);
    TEST_ASSERT_FALSE(module.snapshot().controllerReleaseRecorded);
    TEST_ASSERT_FALSE(module.admitSessionOwnership(physicalConnect(41), fixture.nowMs));

    fixture.linkDownGeneration = 0;
    ++fixture.nowMs;
    TEST_ASSERT_TRUE(module.admitSessionOwnership(physicalConnect(42), fixture.nowMs));
    TEST_ASSERT_EQUAL_UINT32(41, module.snapshot().activeGeneration);
}

void test_adjacent_generation_down_remains_held_until_captured_generation_is_confirmed() {
    Fixture fixture;
    HilFaultRuntimeOwner owner;
    ObdBsc13HilFaultModule module(owner, runtimeFor(fixture));
    initializeArmed(fixture, owner, module);
    TEST_ASSERT_FALSE(module.admitSessionOwnership(physicalConnect(51), fixture.nowMs));

    fixture.cancellationEpoch += 2;
    fixture.linkDownGeneration = 50;
    ++fixture.nowMs;
    module.service(fixture.nowMs);
    TEST_ASSERT_TRUE(module.snapshot().barrierActive);
    TEST_ASSERT_TRUE(module.snapshot().cancellationObserved);
    TEST_ASSERT_FALSE(module.snapshot().linkDownConfirmed);
    TEST_ASSERT_FALSE(module.snapshot().controllerReleaseRecorded);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilFaultState::Fired),
                            static_cast<uint8_t>(module.controllerSnapshot().state));

    fixture.linkDownGeneration = 51;
    ++fixture.nowMs;
    module.service(fixture.nowMs);
    TEST_ASSERT_TRUE(module.snapshot().completionRecorded);
    TEST_ASSERT_TRUE(module.snapshot().linkDownConfirmed);
    TEST_ASSERT_TRUE(module.snapshot().controllerReleaseRecorded);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilFaultState::Released),
                            static_cast<uint8_t>(module.controllerSnapshot().state));
}

void test_automatic_timeout_resumes_ordinary_adoption_without_qualifying() {
    Fixture fixture;
    HilFaultRuntimeOwner owner;
    ObdBsc13HilFaultModule module(owner, runtimeFor(fixture));
    initializeArmed(fixture, owner, module);
    TEST_ASSERT_FALSE(module.admitSessionOwnership(physicalConnect(), fixture.nowMs));

    fixture.nowMs += ObdBsc13HilFaultModule::kAutomaticReleaseMs + 3;
    TEST_ASSERT_TRUE(module.admitSessionOwnership(physicalConnect(), fixture.nowMs));
    TEST_ASSERT_FALSE(module.snapshot().controllerReleaseRecorded);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilFaultState::Expired),
                            static_cast<uint8_t>(module.controllerSnapshot().state));
    TEST_ASSERT_NOT_NULL(std::strstr(fixture.output, "automatic_timeout_resumed_session_ownership"));
    TEST_ASSERT_NULL(std::strstr(fixture.output, "\"hil_event\":\"released\""));
}

void test_session_end_resumes_ordinary_adoption_without_qualifying() {
    Fixture fixture;
    HilFaultRuntimeOwner owner;
    ObdBsc13HilFaultModule module(owner, runtimeFor(fixture));
    initializeArmed(fixture, owner, module);
    TEST_ASSERT_FALSE(module.admitSessionOwnership(physicalConnect(), fixture.nowMs));

    fixture.nowMs += 4;
    endSession(owner, fixture.nowMs);
    ++fixture.nowMs;
    TEST_ASSERT_TRUE(module.admitSessionOwnership(physicalConnect(), fixture.nowMs));
    TEST_ASSERT_NOT_NULL(std::strstr(fixture.output, "session_end_resumed_session_ownership"));
    TEST_ASSERT_NULL(std::strstr(fixture.output, "\"hil_event\":\"released\""));
}

void test_missing_runtime_does_not_capture_or_hold_connection() {
    Fixture fixture;
    HilFaultRuntimeOwner owner;
    ObdBsc13HilFaultModule module(owner);
    initializeArmed(fixture, owner, module);
    TEST_ASSERT_TRUE(module.admitSessionOwnership(physicalConnect(), fixture.nowMs));
    TEST_ASSERT_TRUE(module.snapshot().armed);
    TEST_ASSERT_FALSE(module.snapshot().barrierActive);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_unarmed_invalid_or_nonconnecting_admission_is_neutral);
    RUN_TEST(test_ready_barrier_holds_only_session_adoption_at_physical_connect_edge);
    RUN_TEST(test_cancellation_alone_does_not_release_until_matching_callback_down);
    RUN_TEST(test_link_down_without_cancellation_never_qualifies_release);
    RUN_TEST(test_confirmed_preemption_rejects_captured_link_but_admits_one_later_generation);
    RUN_TEST(test_unexpected_down_rejects_captured_link_but_does_not_strand_new_generation);
    RUN_TEST(test_adjacent_generation_down_remains_held_until_captured_generation_is_confirmed);
    RUN_TEST(test_automatic_timeout_resumes_ordinary_adoption_without_qualifying);
    RUN_TEST(test_session_end_resumes_ordinary_adoption_without_qualifying);
    RUN_TEST(test_missing_runtime_does_not_capture_or_hold_connection);
    return UNITY_END();
}

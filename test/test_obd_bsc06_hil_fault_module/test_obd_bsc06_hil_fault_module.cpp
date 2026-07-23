#include <unity.h>

#include <cstdio>
#include <cstring>

#define V1SIMPLE_HIL_FAULT_CONTROL 1
#include "../../src/modules/hil/hil_fault_controller.cpp"
#include "../../src/modules/hil/hil_fault_serial_module.cpp"
#include "../../src/modules/obd/obd_transport_operation_barrier.cpp"
#include "../../src/modules/obd/obd_bsc06_hil_fault_module.cpp"

namespace {
constexpr char kSessionHashText[] = "7172737475767778797a7b7c7d7e7f808182838485868788898a8b8c8d8e8f90";

enum class YieldAction : uint8_t {
    None,
    Cancel,
    LinkDown,
    EndSession,
};

struct Fixture {
    HilFaultRuntimeOwner* owner = nullptr;
    ObdBsc06HilFaultModule* module = nullptr;
    uint32_t nowMs = 100;
    uint32_t cancellationEpoch = 20;
    uint32_t yields = 0;
    bool linkDown = false;
    bool actionTaken = false;
    YieldAction action = YieldAction::None;
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

bool linkDownConfirmed(const uint32_t, void* context) noexcept {
    return static_cast<Fixture*>(context)->linkDown;
}

void feed(HilFaultRuntimeOwner& owner, const char* command, uint32_t nowMs) {
    for (const char* cursor = command; *cursor != '\0'; ++cursor) {
        owner.acceptSerialByte(*cursor, nowMs);
    }
    owner.acceptSerialByte('\n', nowMs);
}

void endSession(Fixture& fixture) {
    char command[192]{};
    std::snprintf(command, sizeof(command), "V1HIL END BSC-06 %s", kSessionHashText);
    feed(*fixture.owner, command, fixture.nowMs);
}

void yieldTransportOwner(void* context) noexcept {
    auto& fixture = *static_cast<Fixture*>(context);
    ++fixture.yields;
    ++fixture.nowMs;
    fixture.module->service(fixture.nowMs);
    if (fixture.actionTaken) {
        return;
    }
    fixture.actionTaken = true;
    if (fixture.action == YieldAction::Cancel) {
        fixture.cancellationEpoch += 2;
    } else if (fixture.action == YieldAction::LinkDown) {
        fixture.linkDown = true;
    } else if (fixture.action == YieldAction::EndSession) {
        endSession(fixture);
    }
}

ObdBsc06HilRuntime runtimeFor(Fixture& fixture) {
    ObdBsc06HilRuntime runtime{};
    runtime.barrier = {clockMs, cancellationEpoch, linkDownConfirmed, yieldTransportOwner, &fixture};
    runtime.writeEvidence = writeEvidence;
    runtime.evidenceContext = &fixture;
    return runtime;
}

void beginAndArm(HilFaultRuntimeOwner& owner, uint32_t nowMs) {
    char command[224]{};
    std::snprintf(command, sizeof(command), "V1HIL BEGIN BSC-06 %s 60000", kSessionHashText);
    feed(owner, command, nowMs);
    std::snprintf(command, sizeof(command), "V1HIL ARM BSC-06 obd-transport-operation-barrier-once %s 6",
                  kSessionHashText);
    feed(owner, command, nowMs + 1);
}

ObdBsc06HilAdmission pollingWrite(const Fixture& fixture) {
    return {ObdBsc06Operation::Write, 31, 42, fixture.cancellationEpoch,
            ObdBsc06HilFaultModule::kPollingRuntimeStateCode};
}

void initializeArmed(Fixture& fixture, HilFaultRuntimeOwner& owner, ObdBsc06HilFaultModule& module) {
    fixture.owner = &owner;
    fixture.module = &module;
    beginAndArm(owner, fixture.nowMs);
    module.service(fixture.nowMs + 2);
    TEST_ASSERT_TRUE(module.snapshot().armed);
}
} // namespace

void test_unarmed_or_non_polling_operation_is_neutral() {
    Fixture fixture;
    HilFaultRuntimeOwner owner;
    ObdBsc06HilFaultModule module(owner, runtimeFor(fixture));
    fixture.owner = &owner;
    fixture.module = &module;
    TEST_ASSERT_TRUE(module.routeOperation(pollingWrite(fixture), fixture.nowMs));
    TEST_ASSERT_EQUAL_UINT32(0, fixture.yields);

    initializeArmed(fixture, owner, module);
    ObdBsc06HilAdmission wrongState = pollingWrite(fixture);
    wrongState.runtimeStateCode = 7;
    TEST_ASSERT_TRUE(module.routeOperation(wrongState, fixture.nowMs + 3));
    TEST_ASSERT_TRUE(module.snapshot().armed);
    TEST_ASSERT_EQUAL_STRING("", fixture.output);
}

void test_newer_cancellation_epoch_suppresses_polling_write_and_releases() {
    Fixture fixture;
    fixture.action = YieldAction::Cancel;
    HilFaultRuntimeOwner owner;
    ObdBsc06HilFaultModule module(owner, runtimeFor(fixture));
    initializeArmed(fixture, owner, module);

    TEST_ASSERT_FALSE(module.routeOperation(pollingWrite(fixture), fixture.nowMs));
    module.service(fixture.nowMs);

    const ObdBsc06HilSnapshot snapshot = module.snapshot();
    TEST_ASSERT_TRUE(snapshot.completionRecorded);
    TEST_ASSERT_TRUE(snapshot.operationSuppressed);
    TEST_ASSERT_TRUE(snapshot.controllerReleaseRecorded);
    TEST_ASSERT_EQUAL_UINT32(42, snapshot.requestId);
    TEST_ASSERT_EQUAL_UINT32(20, snapshot.dispatchEpoch);
    TEST_ASSERT_EQUAL_UINT32(22, snapshot.cancellationEpoch);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilFaultState::Released),
                            static_cast<uint8_t>(module.controllerSnapshot().state));
    TEST_ASSERT_EQUAL_STRING(
        "{\"hil_event\":\"ready\",\"reason\":\"polling_write_after_epoch_claim\",\"case_id\":\"BSC-06\","
        "\"fault_id\":\"obd-transport-operation-barrier-once\",\"arm_sequence\":6,\"ready_sequence\":1,"
        "\"generation\":31,\"phase\":1,\"request_id\":42,\"dispatch_epoch\":20,\"operation\":\"write\","
        "\"runtime_state\":\"polling\",\"ready_timestamp_ms\":100}\n"
        "{\"hil_event\":\"fired\",\"reason\":\"transport_owner_barrier_active\",\"case_id\":\"BSC-06\","
        "\"fault_id\":\"obd-transport-operation-barrier-once\",\"arm_sequence\":6,\"ready_sequence\":1,"
        "\"generation\":31,\"phase\":1,\"request_id\":42,\"dispatch_epoch\":20,\"operation\":\"write\","
        "\"runtime_state\":\"polling\",\"ready_timestamp_ms\":100}\n"
        "{\"hil_event\":\"released\",\"reason\":\"newer_cancellation_epoch_suppressed_write\","
        "\"case_id\":\"BSC-06\",\"fault_id\":\"obd-transport-operation-barrier-once\",\"arm_sequence\":6,"
        "\"ready_sequence\":1,\"generation\":31,\"phase\":1,\"request_id\":42,\"dispatch_epoch\":20,"
        "\"operation\":\"write\",\"runtime_state\":\"polling\",\"cancellation_epoch\":22,"
        "\"link_down_generation\":0,\"ready_timestamp_ms\":100,\"completion_timestamp_ms\":101,"
        "\"operation_suppressed\":true,\"controller_release_recorded\":true}\n",
        fixture.output);
}

void test_matching_link_down_suppresses_polling_write_and_releases() {
    Fixture fixture;
    fixture.action = YieldAction::LinkDown;
    HilFaultRuntimeOwner owner;
    ObdBsc06HilFaultModule module(owner, runtimeFor(fixture));
    initializeArmed(fixture, owner, module);

    TEST_ASSERT_FALSE(module.routeOperation(pollingWrite(fixture), fixture.nowMs));
    module.service(fixture.nowMs);

    TEST_ASSERT_TRUE(module.snapshot().operationSuppressed);
    TEST_ASSERT_TRUE(module.snapshot().controllerReleaseRecorded);
    TEST_ASSERT_EQUAL_UINT32(20, module.snapshot().cancellationEpoch);
    TEST_ASSERT_EQUAL_STRING(
        "{\"hil_event\":\"ready\",\"reason\":\"polling_write_after_epoch_claim\",\"case_id\":\"BSC-06\","
        "\"fault_id\":\"obd-transport-operation-barrier-once\",\"arm_sequence\":6,\"ready_sequence\":1,"
        "\"generation\":31,\"phase\":1,\"request_id\":42,\"dispatch_epoch\":20,\"operation\":\"write\","
        "\"runtime_state\":\"polling\",\"ready_timestamp_ms\":100}\n"
        "{\"hil_event\":\"fired\",\"reason\":\"transport_owner_barrier_active\",\"case_id\":\"BSC-06\","
        "\"fault_id\":\"obd-transport-operation-barrier-once\",\"arm_sequence\":6,\"ready_sequence\":1,"
        "\"generation\":31,\"phase\":1,\"request_id\":42,\"dispatch_epoch\":20,\"operation\":\"write\","
        "\"runtime_state\":\"polling\",\"ready_timestamp_ms\":100}\n"
        "{\"hil_event\":\"released\",\"reason\":\"matching_link_down_suppressed_write\","
        "\"case_id\":\"BSC-06\",\"fault_id\":\"obd-transport-operation-barrier-once\",\"arm_sequence\":6,"
        "\"ready_sequence\":1,\"generation\":31,\"phase\":1,\"request_id\":42,\"dispatch_epoch\":20,"
        "\"operation\":\"write\",\"runtime_state\":\"polling\",\"cancellation_epoch\":20,"
        "\"link_down_generation\":31,\"ready_timestamp_ms\":100,\"completion_timestamp_ms\":101,"
        "\"operation_suppressed\":true,\"controller_release_recorded\":true}\n",
        fixture.output);
}

void test_automatic_timeout_resumes_ordinary_write_without_qualifying() {
    Fixture fixture;
    HilFaultRuntimeOwner owner;
    ObdBsc06HilFaultModule module(owner, runtimeFor(fixture));
    initializeArmed(fixture, owner, module);

    TEST_ASSERT_TRUE(module.routeOperation(pollingWrite(fixture), fixture.nowMs));
    module.service(fixture.nowMs);

    TEST_ASSERT_FALSE(module.snapshot().operationSuppressed);
    TEST_ASSERT_FALSE(module.snapshot().controllerReleaseRecorded);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilFaultState::Expired),
                            static_cast<uint8_t>(module.controllerSnapshot().state));
    TEST_ASSERT_NOT_NULL(std::strstr(fixture.output, "automatic_timeout_resumed_write"));
    TEST_ASSERT_NULL(std::strstr(fixture.output, "\"hil_event\":\"released\""));
}

void test_session_end_resumes_ordinary_write_without_qualifying() {
    Fixture fixture;
    fixture.action = YieldAction::EndSession;
    HilFaultRuntimeOwner owner;
    ObdBsc06HilFaultModule module(owner, runtimeFor(fixture));
    initializeArmed(fixture, owner, module);

    TEST_ASSERT_TRUE(module.routeOperation(pollingWrite(fixture), fixture.nowMs));
    module.service(fixture.nowMs);

    TEST_ASSERT_FALSE(module.snapshot().operationSuppressed);
    TEST_ASSERT_NOT_NULL(std::strstr(fixture.output, "session_end_resumed_write"));
    TEST_ASSERT_NULL(std::strstr(fixture.output, "\"hil_event\":\"released\""));
}

void test_missing_runtime_does_not_capture_or_block_the_operation() {
    Fixture fixture;
    HilFaultRuntimeOwner owner;
    ObdBsc06HilFaultModule module(owner);
    initializeArmed(fixture, owner, module);

    TEST_ASSERT_TRUE(module.routeOperation(pollingWrite(fixture), fixture.nowMs));
    TEST_ASSERT_TRUE(module.snapshot().armed);
    TEST_ASSERT_FALSE(module.snapshot().barrierActive);
    TEST_ASSERT_EQUAL_UINT32(0, fixture.yields);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_unarmed_or_non_polling_operation_is_neutral);
    RUN_TEST(test_newer_cancellation_epoch_suppresses_polling_write_and_releases);
    RUN_TEST(test_matching_link_down_suppresses_polling_write_and_releases);
    RUN_TEST(test_automatic_timeout_resumes_ordinary_write_without_qualifying);
    RUN_TEST(test_session_end_resumes_ordinary_write_without_qualifying);
    RUN_TEST(test_missing_runtime_does_not_capture_or_block_the_operation);
    return UNITY_END();
}

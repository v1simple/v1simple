#include <unity.h>

#define V1SIMPLE_HIL_FAULT_CONTROL 1
#include "../../src/modules/hil/hil_fault_controller.cpp"
#include "../../src/modules/hil/hil_fault_serial_module.cpp"
#include "../../src/modules/hil/hil_ready_barrier.h"

#include <climits>
#include <new>

#define HIL_SESSION_HASH_TEXT \
    "101112131415161718191a1b1c1d1e1f202122232425262728292a2b2c2d2e2f"

namespace {
HilFaultController controller;
constexpr char kSessionHashText[] = HIL_SESSION_HASH_TEXT;

HilSessionTokenHash makeHash(const uint8_t seed) {
    HilSessionTokenHash hash{};
    for (size_t index = 0; index < hash.bytes.size(); ++index) {
        hash.bytes[index] = static_cast<uint8_t>(seed + index);
    }
    return hash;
}

constexpr HilCaseId kCase = HilCaseId::Bsc06;
constexpr HilFaultId kFault = HilFaultId::ObdTransportOperationBarrierOnce;
const HilSessionTokenHash kSession = makeHash(0x10);
const HilSessionTokenHash kOtherSession = makeHash(0x80);

struct InterleavingContext {
    uint32_t clockNowMs = 0;
    HilFaultResult beginResult = HilFaultResult::WrongState;
};

struct SnapshotInterleavingContext {
    HilFaultController* target = nullptr;
    HilFaultResult beginResult = HilFaultResult::WrongState;
};

uint32_t testClock(void* context) noexcept {
    return static_cast<InterleavingContext*>(context)->clockNowMs;
}

void replaceDuringReadyReservation(HilFaultController& target,
                                   const uint32_t nowMs,
                                   void* context) {
    auto& state = *static_cast<InterleavingContext*>(context);
    state.beginResult = target.beginSession(
        HilCaseId::Bsc04, kOtherSession, nowMs + 500, nowMs);
    state.clockNowMs = nowMs + 99;
}

void replaceDuringSnapshot(void* context) {
    auto& state = *static_cast<SnapshotInterleavingContext*>(context);
    state.beginResult = state.target->beginSession(
        kCase, kOtherSession, 2000, 1000);
}

HilReadyResult armAndReadyOn(HilFaultController& target,
                             const HilSessionTokenHash& session,
                             const uint32_t nowMs,
                             const uint32_t generation = 7,
                             const uint16_t phase = 3) {
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilFaultResult::Ok),
                            static_cast<uint8_t>(target.arm(
                                kCase, kFault, session, 11, nowMs)));
    return target.publishReady(kCase, kFault, session, 11, generation, phase,
                               nowMs + 1, 250);
}

HilReadyResult armAndReady(const uint32_t nowMs = 100,
                           const uint32_t generation = 7,
                           const uint16_t phase = 3) {
    return armAndReadyOn(controller, kSession, nowMs, generation, phase);
}

void serviceDuringReadyReservation(HilFaultController& target,
                                   const uint32_t nowMs,
                                   void*) {
    target.service(nowMs);
}
}  // namespace

void setUp() {
    controller.~HilFaultController();
    new (&controller) HilFaultController();
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(HilFaultResult::Ok),
        static_cast<uint8_t>(controller.beginSession(kCase, kSession, 10000, 0)));
}

void tearDown() {}

void test_typed_allowlist_and_full_one_shot_lifecycle() {
    TEST_ASSERT_TRUE(HilFaultController::isAllowed(kCase, kFault));
    TEST_ASSERT_FALSE(HilFaultController::isAllowed(
        HilCaseId::Bsc04, kFault));

    const HilReadyResult ready = armAndReady();
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilFaultResult::Ok),
                            static_cast<uint8_t>(ready.result));
    TEST_ASSERT_NOT_EQUAL(0, ready.readySequence);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilFaultState::Ready),
                            static_cast<uint8_t>(controller.snapshot(kFault).state));

    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(HilFaultResult::Ok),
        static_cast<uint8_t>(controller.fire(kCase, kFault, kSession, 11,
                                             ready.readySequence, 7, 3, 102)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilFaultState::Fired),
                            static_cast<uint8_t>(controller.snapshot(kFault).state));

    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(HilFaultResult::CompetingOperationMissing),
        static_cast<uint8_t>(controller.release(kCase, kFault, kSession, 11,
                                                ready.readySequence, 7, 3, 103)));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(HilFaultResult::Ok),
        static_cast<uint8_t>(controller.observeCompetingOperation(
            kCase, kFault, kSession, 11, ready.readySequence, 7, 3, 103)));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(HilFaultResult::Ok),
        static_cast<uint8_t>(controller.release(kCase, kFault, kSession, 11,
                                                ready.readySequence, 7, 3, 104)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilFaultState::Released),
                            static_cast<uint8_t>(controller.snapshot(kFault).state));
}

void test_wrong_case_fault_session_and_arm_sequence_are_rejected() {
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(HilFaultResult::WrongCase),
        static_cast<uint8_t>(controller.arm(HilCaseId::Bsc04, kFault, kSession, 1, 1)));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(HilFaultResult::WrongCase),
        static_cast<uint8_t>(controller.arm(kCase, HilFaultId::WifiApStartFailOnce,
                                            kSession, 1, 1)));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(HilFaultResult::WrongSession),
        static_cast<uint8_t>(controller.arm(kCase, kFault, kOtherSession, 1, 1)));

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilFaultResult::Ok),
                            static_cast<uint8_t>(controller.arm(
                                kCase, kFault, kSession, 12, 1)));
    const HilReadyResult wrongArm =
        controller.publishReady(kCase, kFault, kSession, 13, 1, 1, 2, 100);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilFaultResult::WrongArmSequence),
                            static_cast<uint8_t>(wrongArm.result));
}

void test_wrong_ready_generation_phase_and_competing_operation_are_rejected() {
    const HilReadyResult ready = armAndReady(100, 9, 5);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilFaultResult::Ok),
                            static_cast<uint8_t>(ready.result));

    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(HilFaultResult::WrongReadySequence),
        static_cast<uint8_t>(controller.fire(kCase, kFault, kSession, 11,
                                             ready.readySequence + 1, 9, 5, 102)));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(HilFaultResult::WrongGeneration),
        static_cast<uint8_t>(controller.fire(kCase, kFault, kSession, 11,
                                             ready.readySequence, 10, 5, 102)));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(HilFaultResult::WrongPhase),
        static_cast<uint8_t>(controller.fire(kCase, kFault, kSession, 11,
                                             ready.readySequence, 9, 6, 102)));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(HilFaultResult::Ok),
        static_cast<uint8_t>(controller.fire(kCase, kFault, kSession, 11,
                                             ready.readySequence, 9, 5, 102)));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(HilFaultResult::CompetingOperationMissing),
        static_cast<uint8_t>(controller.release(kCase, kFault, kSession, 11,
                                                ready.readySequence, 9, 5, 103)));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(HilFaultResult::WrongReadySequence),
        static_cast<uint8_t>(controller.observeCompetingOperation(
            kCase, kFault, kSession, 11, ready.readySequence + 1, 9, 5, 103)));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(HilFaultResult::WrongGeneration),
        static_cast<uint8_t>(controller.observeCompetingOperation(
            kCase, kFault, kSession, 11, ready.readySequence, 10, 5, 103)));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(HilFaultResult::WrongPhase),
        static_cast<uint8_t>(controller.observeCompetingOperation(
            kCase, kFault, kSession, 11, ready.readySequence, 9, 6, 103)));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(HilFaultResult::WrongSession),
        static_cast<uint8_t>(controller.observeCompetingOperation(
            kCase, kFault, kOtherSession, 11, ready.readySequence, 9, 5, 103)));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(HilFaultResult::Ok),
        static_cast<uint8_t>(controller.observeCompetingOperation(
            kCase, kFault, kSession, 11, ready.readySequence, 9, 5, 103)));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(HilFaultResult::WrongState),
        static_cast<uint8_t>(controller.observeCompetingOperation(
            kCase, kFault, kSession, 11, ready.readySequence, 9, 5, 104)));
}

void test_duplicate_fire_is_rejected() {
    const HilReadyResult ready = armAndReady();
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(HilFaultResult::Ok),
        static_cast<uint8_t>(controller.fire(kCase, kFault, kSession, 11,
                                             ready.readySequence, 7, 3, 102)));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(HilFaultResult::DuplicateFire),
        static_cast<uint8_t>(controller.fire(kCase, kFault, kSession, 11,
                                             ready.readySequence, 7, 3, 103)));
}

void test_ready_metadata_is_published_before_ready_becomes_visible() {
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilFaultResult::Ok),
                            static_cast<uint8_t>(controller.arm(
                                kCase, kFault, kSession, 11, 100)));
    controller.setReadyReservationHook(serviceDuringReadyReservation, nullptr);
    const HilReadyResult ready =
        controller.publishReady(kCase, kFault, kSession, 11, 7, 3, 101, 250);
    controller.setReadyReservationHook(nullptr, nullptr);

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilFaultResult::Ok),
                            static_cast<uint8_t>(ready.result));
    const HilFaultSnapshot snapshot = controller.snapshot(kFault);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilFaultState::Ready),
                            static_cast<uint8_t>(snapshot.state));
    TEST_ASSERT_EQUAL_UINT32(351, snapshot.automaticReleaseDeadlineMs);
}

void test_forged_fault_enum_cannot_index_outside_fixed_registry() {
    const HilFaultSnapshot snapshot = controller.snapshot(static_cast<HilFaultId>(255));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilFaultState::Disarmed),
                            static_cast<uint8_t>(snapshot.state));
    TEST_ASSERT_FALSE(HilFaultController::isAllowed(kCase, static_cast<HilFaultId>(255)));
}

void test_ready_and_unobserved_fired_timeout_expire_without_blocking() {
    const HilReadyResult ready = armAndReady();
    controller.service(351);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilFaultState::Expired),
                            static_cast<uint8_t>(controller.snapshot(kFault).state));

    HilFaultController firedController;
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(HilFaultResult::Ok),
        static_cast<uint8_t>(firedController.beginSession(
            kCase, kOtherSession, 20000, 10000)));
    const HilReadyResult firedReady = armAndReadyOn(firedController, kOtherSession, 10100);
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(HilFaultResult::Ok),
        static_cast<uint8_t>(firedController.fire(kCase, kFault, kOtherSession, 11,
                                                  firedReady.readySequence, 7, 3, 10102)));
    firedController.service(10351);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilFaultState::Expired),
                            static_cast<uint8_t>(firedController.snapshot(kFault).state));

    HilFaultController observedController;
    const HilSessionTokenHash observedSession = makeHash(0x40);
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(HilFaultResult::Ok),
        static_cast<uint8_t>(observedController.beginSession(
            kCase, observedSession, 20000, 10000)));
    const HilReadyResult observedReady =
        armAndReadyOn(observedController, observedSession, 10100);
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(HilFaultResult::Ok),
        static_cast<uint8_t>(observedController.fire(
            kCase, kFault, observedSession, 11, observedReady.readySequence, 7, 3, 10102)));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(HilFaultResult::Ok),
        static_cast<uint8_t>(observedController.observeCompetingOperation(
            kCase, kFault, observedSession, 11, observedReady.readySequence, 7, 3, 10103)));
    observedController.service(10351);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilFaultState::Released),
                            static_cast<uint8_t>(observedController.snapshot(kFault).state));
}

void test_deadlines_are_rollover_safe() {
    HilFaultController rolloverController;
    const uint32_t now = UINT32_MAX - 100u;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilFaultResult::Ok),
                            static_cast<uint8_t>(rolloverController.beginSession(
                                kCase, kSession, now + 1000u, now)));
    const HilReadyResult ready = armAndReadyOn(rolloverController, kSession, now + 1u);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilFaultResult::Ok),
                            static_cast<uint8_t>(ready.result));
    rolloverController.service(now + 252u);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilFaultState::Expired),
                            static_cast<uint8_t>(rolloverController.snapshot(kFault).state));
}

void test_session_hash_reuse_is_rejected_and_cannot_create_aba_identity() {
    const HilReadyResult oldReady = armAndReady(100, 7, 3);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilFaultResult::Ok),
                            static_cast<uint8_t>(oldReady.result));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(HilFaultResult::Ok),
        static_cast<uint8_t>(controller.beginSession(kCase, kOtherSession, 2000, 1000)));
    const HilReadyResult currentReady = armAndReadyOn(controller, kOtherSession, 1100, 7, 3);
    TEST_ASSERT_EQUAL_UINT32(oldReady.readySequence, currentReady.readySequence);

    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(HilFaultResult::WrongSession),
        static_cast<uint8_t>(controller.fire(
            kCase, kFault, kSession, 11, oldReady.readySequence, 7, 3, 1102)));
    TEST_ASSERT_FALSE(controller.shouldPause(
        kCase, kFault, kSession, 11, oldReady.readySequence, 7, 3, 1102));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(HilFaultResult::InvalidSessionHash),
        static_cast<uint8_t>(controller.beginSession(kCase, kSession, 3000, 1200)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilCaseId::Bsc06),
                            static_cast<uint8_t>(controller.activeCase()));
}

void test_ready_reservation_blocks_replacement_and_rechecks_completion_deadline() {
    InterleavingContext context{};
    context.clockNowMs = 101;
    HilFaultController timedController(testClock, &context);
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(HilFaultResult::Ok),
        static_cast<uint8_t>(timedController.beginSession(kCase, kSession, 200, 100)));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(HilFaultResult::Ok),
        static_cast<uint8_t>(timedController.arm(kCase, kFault, kSession, 11, 100)));
    timedController.setReadyReservationHook(replaceDuringReadyReservation, &context);
    const HilReadyResult ready =
        timedController.publishReady(kCase, kFault, kSession, 11, 7, 3, 101, 20);
    timedController.setReadyReservationHook(nullptr, nullptr);

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilFaultResult::Busy),
                            static_cast<uint8_t>(context.beginResult));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilFaultResult::Expired),
                            static_cast<uint8_t>(ready.result));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilFaultState::Expired),
                            static_cast<uint8_t>(timedController.snapshot(kFault).state));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(HilFaultResult::Ok),
        static_cast<uint8_t>(timedController.beginSession(
            HilCaseId::Bsc04, kOtherSession, 700, 200)));
}

void test_regressed_completion_clock_fails_closed() {
    InterleavingContext context{};
    context.clockNowMs = 90;
    HilFaultController timedController(testClock, &context);
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(HilFaultResult::Ok),
        static_cast<uint8_t>(timedController.beginSession(kCase, kSession, 1000, 0)));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(HilFaultResult::Ok),
        static_cast<uint8_t>(timedController.arm(kCase, kFault, kSession, 11, 100)));
    const HilReadyResult ready =
        timedController.publishReady(kCase, kFault, kSession, 11, 7, 3, 101, 20);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilFaultResult::Expired),
                            static_cast<uint8_t>(ready.result));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilFaultState::Expired),
                            static_cast<uint8_t>(timedController.snapshot(kFault).state));
}

void test_new_session_and_end_clear_every_prior_arm() {
    const HilReadyResult ready = armAndReady();
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilFaultResult::Ok),
                            static_cast<uint8_t>(ready.result));

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilFaultResult::Ok),
                            static_cast<uint8_t>(controller.beginSession(
                                HilCaseId::Bsc04, kOtherSession, 2000, 1000)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilFaultState::Disarmed),
                            static_cast<uint8_t>(controller.snapshot(kFault).state));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilCaseId::Bsc04),
                            static_cast<uint8_t>(controller.activeCase()));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(HilFaultResult::WrongSession),
        static_cast<uint8_t>(controller.endSession(HilCaseId::Bsc04, kSession)));
    TEST_ASSERT_TRUE(controller.sessionActive());
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(HilFaultResult::Ok),
        static_cast<uint8_t>(controller.endSession(HilCaseId::Bsc04, kOtherSession)));
    TEST_ASSERT_FALSE(controller.sessionActive());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilFaultState::Disarmed),
                            static_cast<uint8_t>(controller.snapshot(kFault).state));
}

void test_snapshot_rejects_metadata_changed_by_session_replacement() {
    const HilReadyResult ready = armAndReady();
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilFaultResult::Ok),
                            static_cast<uint8_t>(ready.result));
    SnapshotInterleavingContext context{};
    context.target = &controller;
    controller.setSnapshotReadHook(replaceDuringSnapshot, &context);
    const HilFaultSnapshot snapshot = controller.snapshot(kFault);
    controller.setSnapshotReadHook(nullptr, nullptr);

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilFaultResult::Ok),
                            static_cast<uint8_t>(context.beginResult));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilFaultState::Disarmed),
                            static_cast<uint8_t>(snapshot.state));
    TEST_ASSERT_EQUAL_UINT32(0, snapshot.readySequence);
}

void test_zero_session_hash_is_rejected_without_replacing_active_session() {
    HilSessionTokenHash zeroHash{};
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(HilFaultResult::InvalidSessionHash),
        static_cast<uint8_t>(controller.beginSession(HilCaseId::Bsc04, zeroHash, 1000, 0)));
    TEST_ASSERT_TRUE(controller.sessionActive());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(kCase),
                            static_cast<uint8_t>(controller.activeCase()));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(HilFaultResult::Ok),
        static_cast<uint8_t>(controller.arm(kCase, kFault, kSession, 15, 1)));
}

void test_forged_case_enum_is_rejected_without_replacing_active_session() {
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(HilFaultResult::WrongCase),
        static_cast<uint8_t>(controller.beginSession(
            static_cast<HilCaseId>(255), kOtherSession, 1000, 0)));
    TEST_ASSERT_TRUE(controller.sessionActive());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(kCase),
                            static_cast<uint8_t>(controller.activeCase()));
}

void test_serial_policy_accepts_only_hashed_local_commands() {
    HilSerialCommand command{};
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(HilSerialParseResult::Ok),
        static_cast<uint8_t>(HilFaultSerialModule::parse(
            "V1HIL BEGIN BSC-06 " HIL_SESSION_HASH_TEXT " 500", command)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilSerialCommandKind::Begin),
                            static_cast<uint8_t>(command.kind));
    TEST_ASSERT_EQUAL_UINT32(500, command.durationMs);

    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(HilSerialParseResult::InvalidArguments),
        static_cast<uint8_t>(HilFaultSerialModule::parse(
            "V1HIL BEGIN BSC-06 raw-session-token 500", command)));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(HilSerialParseResult::InvalidArguments),
        static_cast<uint8_t>(HilFaultSerialModule::parse(
            "V1HIL BEGIN BSC-06 "
            "0000000000000000000000000000000000000000000000000000000000000000 500",
            command)));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(HilSerialParseResult::InvalidArguments),
        static_cast<uint8_t>(HilFaultSerialModule::parse(
            "V1HIL BEGIN BSC-06 " HIL_SESSION_HASH_TEXT " 0", command)));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(HilSerialParseResult::InvalidArguments),
        static_cast<uint8_t>(HilFaultSerialModule::parse(
            "V1HIL BEGIN BSC-06 " HIL_SESSION_HASH_TEXT " 60001", command)));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(HilSerialParseResult::InvalidArguments),
        static_cast<uint8_t>(HilFaultSerialModule::parse(
            "V1HIL RELEASE BSC-06 obd-transport-operation-barrier-once "
            HIL_SESSION_HASH_TEXT
            " 1 2 3", command)));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(HilSerialParseResult::Ok),
        static_cast<uint8_t>(HilFaultSerialModule::parse(
            "V1HIL RELEASE BSC-06 obd-transport-operation-barrier-once "
            HIL_SESSION_HASH_TEXT
            " 1 2 3 4", command)));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(HilSerialParseResult::UnknownCommand),
        static_cast<uint8_t>(HilFaultSerialModule::parse("V1HIL FIRE", command)));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(HilSerialParseResult::InvalidPrefix),
        static_cast<uint8_t>(HilFaultSerialModule::parse("HTTP BEGIN", command)));
}

void test_serial_policy_executes_begin_arm_status_end_without_exposing_hash() {
    HilFaultController serialController;
    HilSerialCommand command{};
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(HilSerialParseResult::Ok),
        static_cast<uint8_t>(HilFaultSerialModule::parse(
            "V1HIL BEGIN BSC-06 " HIL_SESSION_HASH_TEXT " 500", command)));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(HilFaultResult::Ok),
        static_cast<uint8_t>(HilFaultSerialModule::execute(command, serialController, 1000)));

    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(HilSerialParseResult::Ok),
        static_cast<uint8_t>(HilFaultSerialModule::parse(
            "V1HIL ARM BSC-06 obd-transport-operation-barrier-once "
            HIL_SESSION_HASH_TEXT " 9",
            command)));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(HilFaultResult::Ok),
        static_cast<uint8_t>(HilFaultSerialModule::execute(command, serialController, 1001)));

    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(HilFaultResult::Expired),
        static_cast<uint8_t>(serialController.arm(
            kCase, kFault, kSession, 10, 1500)));

    char response[192]{};
    TEST_ASSERT_TRUE(HilFaultSerialModule::formatResponse(
        HilSerialParseResult::Ok, HilFaultResult::Ok, serialController, response,
        sizeof(response)));
    TEST_ASSERT_NOT_NULL(std::strstr(response, "\"case_id\":\"BSC-06\""));
    TEST_ASSERT_NULL(std::strstr(response, kSessionHashText));
    TEST_ASSERT_EQUAL_CHAR('{', response[0]);
    TEST_ASSERT_NULL(std::strstr(response, "V1HIL"));
    TEST_ASSERT_EQUAL_CHAR('\n', response[std::strlen(response) - 1]);

    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(HilSerialParseResult::Ok),
        static_cast<uint8_t>(HilFaultSerialModule::parse(
            "V1HIL END BSC-06 " HIL_SESSION_HASH_TEXT, command)));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(HilFaultResult::Ok),
        static_cast<uint8_t>(HilFaultSerialModule::execute(command, serialController, 1501)));
    TEST_ASSERT_FALSE(serialController.sessionActive());
}

void test_ready_barrier_publication_never_waits_and_auto_unpauses() {
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilFaultResult::Ok),
                            static_cast<uint8_t>(controller.arm(
                                kCase, kFault, kSession, 42, 10)));
    HilReadyPublication publication{};
    publication.caseId = kCase;
    publication.faultId = kFault;
    publication.sessionHash = kSession;
    publication.armSequence = 42;
    publication.activeGeneration = 8;
    publication.exactPhase = 2;
    publication.nowMs = 11;
    publication.automaticReleaseAfterMs = 20;
    const HilReadyResult ready = HilReadyBarrier::publish(controller, publication);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilFaultResult::Ok),
                            static_cast<uint8_t>(ready.result));
    HilReadyIdentity identity{};
    identity.caseId = kCase;
    identity.faultId = kFault;
    identity.sessionHash = kSession;
    identity.armSequence = 42;
    identity.readySequence = ready.readySequence;
    identity.activeGeneration = 8;
    identity.exactPhase = 2;
    TEST_ASSERT_TRUE(HilReadyBarrier::shouldPause(controller, identity, 12));

    HilReadyIdentity stale = identity;
    stale.sessionHash = kOtherSession;
    TEST_ASSERT_FALSE(HilReadyBarrier::shouldPause(controller, stale, 12));
    stale = identity;
    ++stale.armSequence;
    TEST_ASSERT_FALSE(HilReadyBarrier::shouldPause(controller, stale, 12));
    stale = identity;
    ++stale.readySequence;
    TEST_ASSERT_FALSE(HilReadyBarrier::shouldPause(controller, stale, 12));
    stale = identity;
    ++stale.activeGeneration;
    TEST_ASSERT_FALSE(HilReadyBarrier::shouldPause(controller, stale, 12));
    stale = identity;
    ++stale.exactPhase;
    TEST_ASSERT_FALSE(HilReadyBarrier::shouldPause(controller, stale, 12));

    TEST_ASSERT_FALSE(HilReadyBarrier::shouldPause(controller, identity, 31));

    controller.service(31);
    TEST_ASSERT_FALSE(HilReadyBarrier::shouldPause(controller, identity, 31));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilFaultState::Expired),
                            static_cast<uint8_t>(controller.snapshot(kFault).state));
}

int main(int argc, char** argv) {
    UNITY_BEGIN();
    RUN_TEST(test_typed_allowlist_and_full_one_shot_lifecycle);
    RUN_TEST(test_wrong_case_fault_session_and_arm_sequence_are_rejected);
    RUN_TEST(test_wrong_ready_generation_phase_and_competing_operation_are_rejected);
    RUN_TEST(test_duplicate_fire_is_rejected);
    RUN_TEST(test_ready_metadata_is_published_before_ready_becomes_visible);
    RUN_TEST(test_forged_fault_enum_cannot_index_outside_fixed_registry);
    RUN_TEST(test_ready_and_unobserved_fired_timeout_expire_without_blocking);
    RUN_TEST(test_deadlines_are_rollover_safe);
    RUN_TEST(test_session_hash_reuse_is_rejected_and_cannot_create_aba_identity);
    RUN_TEST(test_ready_reservation_blocks_replacement_and_rechecks_completion_deadline);
    RUN_TEST(test_regressed_completion_clock_fails_closed);
    RUN_TEST(test_new_session_and_end_clear_every_prior_arm);
    RUN_TEST(test_snapshot_rejects_metadata_changed_by_session_replacement);
    RUN_TEST(test_zero_session_hash_is_rejected_without_replacing_active_session);
    RUN_TEST(test_forged_case_enum_is_rejected_without_replacing_active_session);
    RUN_TEST(test_serial_policy_accepts_only_hashed_local_commands);
    RUN_TEST(test_serial_policy_executes_begin_arm_status_end_without_exposing_hash);
    RUN_TEST(test_ready_barrier_publication_never_waits_and_auto_unpauses);
    return UNITY_END();
}

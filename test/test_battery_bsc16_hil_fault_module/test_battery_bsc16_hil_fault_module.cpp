#include <unity.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#define V1SIMPLE_HIL_FAULT_CONTROL 1
#include "../../src/modules/hil/hil_fault_controller.cpp"
#include "../../src/modules/hil/hil_fault_serial_module.cpp"
#include "../../src/modules/power/battery_bsc16_hil_fault_module.cpp"

namespace {
constexpr char kSessionHashText[] = "3132333435363738393a3b3c3d3e3f404142434445464748494a4b4c4d4e4f50";

struct Fixture {
    uint64_t persistentNowMs = 1000;
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

uint64_t persistentClock(void* context) noexcept {
    return static_cast<Fixture*>(context)->persistentNowMs;
}

BatteryBsc16HilRuntime runtimeFor(Fixture& fixture) {
    BatteryBsc16HilRuntime runtime{};
    runtime.writeEvidence = writeEvidence;
    runtime.persistentClockMs = persistentClock;
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
    std::snprintf(command, sizeof(command), "V1HIL BEGIN BSC-16 %s 60000", kSessionHashText);
    feed(owner, command, nowMs);
    std::snprintf(command, sizeof(command), "V1HIL ARM BSC-16 battery-adc-init-fail-once %s %lu", kSessionHashText,
                  static_cast<unsigned long>(armSequence));
    feed(owner, command, nowMs + 1);
}

bool stageBattery(const HilArmedFaultIdentity& identity, const uint32_t deadlineMs, const uint32_t stagedAtMs,
                  void* context) noexcept {
    return static_cast<BatteryBsc16HilFaultModule*>(context)->stageNextBoot(identity, deadlineMs, stagedAtMs);
}

void clearBattery(void* context) noexcept {
    static_cast<BatteryBsc16HilFaultModule*>(context)->clearNextBoot();
}

void stageNextBoot(HilFaultRuntimeOwner& owner, const uint32_t armSequence, const uint32_t nowMs) {
    char command[224]{};
    std::snprintf(command, sizeof(command), "V1HIL NEXT_BOOT BSC-16 battery-adc-init-fail-once %s %lu",
                  kSessionHashText, static_cast<unsigned long>(armSequence));
    feed(owner, command, nowMs);
}

BatteryBsc16HilAdcAdmission safeAdmission() {
    BatteryBsc16HilAdcAdmission admission{};
    admission.latchInitialized = true;
    admission.sourceClassification = battery_source_policy::Source::Battery;
    admission.powerButtonWillBeEnabled = true;
    return admission;
}
} // namespace

void test_next_boot_record_is_routed_restored_cleared_and_consumed_once() {
    Fixture fixture{};
    HilNextBootFaultRecord record{};
    HilFaultRuntimeOwner stagingOwner;
    BatteryBsc16HilFaultModule staging(stagingOwner, record, 40, runtimeFor(fixture));
    HilNextBootFaultRouter router;
    TEST_ASSERT_TRUE(router.configure(
        1, {HilCaseId::Bsc16, HilFaultId::BatteryAdcInitFailOnce, stageBattery, clearBattery, &staging}));
    stagingOwner.configureSerial(writeEvidence, &fixture);
    stagingOwner.configureNextBoot(HilNextBootFaultRouter::stageCallback, HilNextBootFaultRouter::clearCallback,
                                   &router);
    beginAndArm(stagingOwner, 7, 100);
    stageNextBoot(stagingOwner, 7, 102);

    TEST_ASSERT_EQUAL_UINT32(BatteryBsc16HilFaultModule::kNextBootMagic, record.magic);
    TEST_ASSERT_EQUAL_UINT32(41, record.targetBootSequence);
    TEST_ASSERT_EQUAL_UINT32(59998, record.remainingSessionMs);
    const HilNextBootFaultRecord staged = record;

    HilFaultRuntimeOwner restoredOwner;
    BatteryBsc16HilFaultModule restored(restoredOwner, record, 41, runtimeFor(fixture));
    fixture.persistentNowMs = 1100;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilFaultResult::Ok),
                            static_cast<uint8_t>(restored.restoreNextBoot(1000)));
    TEST_ASSERT_EQUAL_UINT32(0, record.magic);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilFaultState::Armed),
                            static_cast<uint8_t>(restored.controllerSnapshot().state));

    TEST_ASSERT_TRUE(restored.beginAdcAdmission(safeAdmission(), 1001));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilFaultState::Fired),
                            static_cast<uint8_t>(restored.controllerSnapshot().state));
    restored.completeAdcAdmissionSuppression(1002);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilFaultState::Released),
                            static_cast<uint8_t>(restored.controllerSnapshot().state));
    TEST_ASSERT_TRUE(restored.snapshot().admissionSuppressed);
    TEST_ASSERT_FALSE(restored.beginAdcAdmission(safeAdmission(), 1003));
    TEST_ASSERT_NOT_NULL(std::strstr(fixture.output, "\"hil_event\":\"ready\""));
    TEST_ASSERT_NOT_NULL(std::strstr(fixture.output, "\"hil_event\":\"fired\""));
    TEST_ASSERT_NOT_NULL(std::strstr(fixture.output, "\"hil_event\":\"released\""));
    TEST_ASSERT_NOT_NULL(std::strstr(fixture.output, "\"latch_initialized\":true"));
    TEST_ASSERT_NOT_NULL(std::strstr(fixture.output, "\"adc_handle_allocated\":false"));
    TEST_ASSERT_NOT_NULL(std::strstr(fixture.output, "\"power_button_enabled\":true"));

    record = staged;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilFaultResult::InvalidSessionHash),
                            static_cast<uint8_t>(restored.restoreNextBoot(1004)));
    TEST_ASSERT_EQUAL_UINT32(0, record.magic);
}

void test_next_boot_record_rejects_tamper_wrong_boot_and_expiry_before_restoring() {
    Fixture fixture{};
    HilNextBootFaultRecord record{};
    HilFaultRuntimeOwner stagingOwner;
    BatteryBsc16HilFaultModule staging(stagingOwner, record, 10, runtimeFor(fixture));
    HilNextBootFaultRouter router;
    TEST_ASSERT_TRUE(router.configure(
        0, {HilCaseId::Bsc16, HilFaultId::BatteryAdcInitFailOnce, stageBattery, clearBattery, &staging}));
    stagingOwner.configureNextBoot(HilNextBootFaultRouter::stageCallback, HilNextBootFaultRouter::clearCallback,
                                   &router);
    beginAndArm(stagingOwner, 9, 200);
    stageNextBoot(stagingOwner, 9, 202);
    const HilNextBootFaultRecord valid = record;

    record = valid;
    record.sessionHash[3] ^= 0x80;
    HilFaultRuntimeOwner tamperedOwner;
    BatteryBsc16HilFaultModule tampered(tamperedOwner, record, 11, runtimeFor(fixture));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilFaultResult::WrongState),
                            static_cast<uint8_t>(tampered.restoreNextBoot(300)));
    TEST_ASSERT_EQUAL_UINT32(0, record.magic);

    record = valid;
    HilFaultRuntimeOwner wrongBootOwner;
    BatteryBsc16HilFaultModule wrongBoot(wrongBootOwner, record, 12, runtimeFor(fixture));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilFaultResult::WrongState),
                            static_cast<uint8_t>(wrongBoot.restoreNextBoot(300)));

    record = valid;
    fixture.persistentNowMs = valid.persistentDeadlineMs;
    HilFaultRuntimeOwner expiredOwner;
    BatteryBsc16HilFaultModule expired(expiredOwner, record, 11, runtimeFor(fixture));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilFaultResult::WrongState),
                            static_cast<uint8_t>(expired.restoreNextBoot(300)));
}

void test_adc_fault_refuses_unsafe_or_unarmed_admission_without_mutation() {
    Fixture fixture{};
    HilNextBootFaultRecord record{};
    HilFaultRuntimeOwner owner;
    BatteryBsc16HilFaultModule module(owner, record, 1, runtimeFor(fixture));
    TEST_ASSERT_FALSE(module.beginAdcAdmission(safeAdmission(), 100));
    TEST_ASSERT_FALSE(module.snapshot().admissionAttempted);

    HilSessionTokenHash session{};
    session.bytes[0] = 1;
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(HilFaultResult::Ok),
        static_cast<uint8_t>(owner.controller().beginSession(HilCaseId::Bsc16, session, 1000, 100)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilFaultResult::Ok),
                            static_cast<uint8_t>(owner.controller().arm(
                                HilCaseId::Bsc16, HilFaultId::BatteryAdcInitFailOnce, session, 1, 101)));

    BatteryBsc16HilAdcAdmission unsafe = safeAdmission();
    unsafe.latchInitialized = false;
    TEST_ASSERT_FALSE(module.beginAdcAdmission(unsafe, 102));
    unsafe = safeAdmission();
    unsafe.sourceClassification = battery_source_policy::Source::Usb;
    TEST_ASSERT_FALSE(module.beginAdcAdmission(unsafe, 103));
    unsafe = safeAdmission();
    unsafe.powerButtonWillBeEnabled = false;
    TEST_ASSERT_FALSE(module.beginAdcAdmission(unsafe, 104));
    TEST_ASSERT_FALSE(module.snapshot().admissionAttempted);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilFaultState::Armed),
                            static_cast<uint8_t>(module.controllerSnapshot().state));
}

void test_router_rejects_unknown_duplicate_and_mismatched_routes_and_clears_all() {
    Fixture fixture{};
    HilNextBootFaultRecord first{};
    HilNextBootFaultRecord second{};
    HilFaultRuntimeOwner owner;
    BatteryBsc16HilFaultModule firstModule(owner, first, 1, runtimeFor(fixture));
    BatteryBsc16HilFaultModule secondModule(owner, second, 1, runtimeFor(fixture));
    HilNextBootFaultRouter router;
    const HilNextBootFaultRoute route{HilCaseId::Bsc16, HilFaultId::BatteryAdcInitFailOnce, stageBattery, clearBattery,
                                      &firstModule};
    TEST_ASSERT_TRUE(router.configure(0, route));
    TEST_ASSERT_FALSE(router.configure(1, route));
    TEST_ASSERT_FALSE(router.configure(2, route));

    HilArmedFaultIdentity wrong{};
    wrong.caseId = HilCaseId::Bsc02;
    wrong.faultId = HilFaultId::WifiApStartFailOnce;
    TEST_ASSERT_FALSE(router.stage(wrong, 1000, 10));

    first.magic = 1;
    second.magic = 2;
    TEST_ASSERT_TRUE(router.configure(
        1, {HilCaseId::Bsc02, HilFaultId::WifiApStartFailOnce, stageBattery, clearBattery, &secondModule}));
    router.clear();
    TEST_ASSERT_EQUAL_UINT32(0, first.magic);
    TEST_ASSERT_EQUAL_UINT32(0, second.magic);
}

void test_suppression_must_be_completed_to_release_the_fault() {
    Fixture fixture{};
    HilNextBootFaultRecord record{};
    HilFaultRuntimeOwner owner;
    BatteryBsc16HilFaultModule module(owner, record, 1, runtimeFor(fixture));
    beginAndArm(owner, 4, 100);
    TEST_ASSERT_TRUE(module.beginAdcAdmission(safeAdmission(), 102));
    TEST_ASSERT_TRUE(module.snapshot().suppressionPending);
    module.service(1102);
    module.completeAdcAdmissionSuppression(1103);
    TEST_ASSERT_FALSE(module.snapshot().admissionSuppressed);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilFaultState::Expired),
                            static_cast<uint8_t>(module.controllerSnapshot().state));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_next_boot_record_is_routed_restored_cleared_and_consumed_once);
    RUN_TEST(test_next_boot_record_rejects_tamper_wrong_boot_and_expiry_before_restoring);
    RUN_TEST(test_adc_fault_refuses_unsafe_or_unarmed_admission_without_mutation);
    RUN_TEST(test_router_rejects_unknown_duplicate_and_mismatched_routes_and_clears_all);
    RUN_TEST(test_suppression_must_be_completed_to_release_the_fault);
    return UNITY_END();
}

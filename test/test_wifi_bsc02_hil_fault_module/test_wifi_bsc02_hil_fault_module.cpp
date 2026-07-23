#include <unity.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

#define V1SIMPLE_HIL_FAULT_CONTROL 1
#include "../../src/modules/hil/hil_fault_controller.cpp"
#include "../../src/modules/hil/hil_fault_serial_module.cpp"
#include "../../src/modules/wifi/wifi_bsc02_hil_fault_module.cpp"

namespace {
constexpr char kSessionHashText[] = "0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20";
constexpr uint32_t kTriggerFree = 16 * 1024;
constexpr uint32_t kTriggerLargest = 8 * 1024;
constexpr uint32_t kSafetyFree = WifiBsc02HilFaultModule::kPressureSafetyFreeBytes;
constexpr uint32_t kSafetyLargest = WifiBsc02HilFaultModule::kPressureSafetyLargestBlockBytes;
constexpr uint32_t kTaskOverheadFree = 3 * 1024;
constexpr uint32_t kTaskOverheadLargest = 1024;

struct Fixture {
    uint32_t freeInternal = 96 * 1024;
    uint32_t largestInternal = 80 * 1024;
    uint32_t allocationCount = 0;
    uint32_t releaseCount = 0;
    bool taskStarted = false;
    bool taskStartAllowed = true;
    uint32_t taskOverheadFree = kTaskOverheadFree;
    uint32_t taskOverheadLargest = kTaskOverheadLargest;
    uint64_t persistentNowMs = 1000;
    std::array<uint8_t, WifiBsc02HilFaultModule::kMaximumPressureChunks> tokens{};
    char serialOutput[4096]{};
    size_t serialOutputLength = 0;
};

uint32_t fakeFreeInternal(void* context) noexcept {
    return static_cast<Fixture*>(context)->freeInternal;
}

uint32_t fakeLargestInternal(void* context) noexcept {
    return static_cast<Fixture*>(context)->largestInternal;
}

void* fakeAllocateInternal(const size_t bytes, void* context) noexcept {
    Fixture& fixture = *static_cast<Fixture*>(context);
    if (bytes != WifiBsc02HilFaultModule::kPressureChunkBytes || fixture.allocationCount >= fixture.tokens.size() ||
        fixture.freeInternal < bytes) {
        return nullptr;
    }
    void* result = &fixture.tokens[fixture.allocationCount++];
    fixture.freeInternal -= static_cast<uint32_t>(bytes);
    fixture.largestInternal =
        fixture.largestInternal > bytes ? fixture.largestInternal - static_cast<uint32_t>(bytes) : 0;
    return result;
}

void fakeReleaseInternal(void*, void* context) noexcept {
    Fixture& fixture = *static_cast<Fixture*>(context);
    ++fixture.releaseCount;
    fixture.freeInternal += WifiBsc02HilFaultModule::kPressureChunkBytes;
    fixture.largestInternal += WifiBsc02HilFaultModule::kPressureChunkBytes;
}

bool fakeStartPressureTask(void* context) noexcept {
    Fixture& fixture = *static_cast<Fixture*>(context);
    fixture.taskStarted = true;
    if (!fixture.taskStartAllowed) {
        return false;
    }
    // Model the task stack/TCB allocation before the task-owned baseline. The
    // pressure allocation cap therefore cannot omit this runtime overhead.
    fixture.freeInternal -= fixture.taskOverheadFree;
    fixture.largestInternal -= fixture.taskOverheadLargest;
    return true;
}

void fakeWrite(const char* text, void* context) noexcept {
    Fixture& fixture = *static_cast<Fixture*>(context);
    if (text == nullptr) {
        return;
    }
    const size_t length = std::strlen(text);
    const size_t available = sizeof(fixture.serialOutput) - fixture.serialOutputLength - 1;
    const size_t copyLength = length < available ? length : available;
    std::memcpy(fixture.serialOutput + fixture.serialOutputLength, text, copyLength);
    fixture.serialOutputLength += copyLength;
    fixture.serialOutput[fixture.serialOutputLength] = '\0';
}

uint64_t fakePersistentClockMs(void* context) noexcept {
    return static_cast<Fixture*>(context)->persistentNowMs;
}

WifiBsc02HilRuntime makeRuntime(Fixture& fixture) {
    WifiBsc02HilRuntime runtime{};
    runtime.freeInternal = fakeFreeInternal;
    runtime.largestInternal = fakeLargestInternal;
    runtime.allocateInternal = fakeAllocateInternal;
    runtime.releaseInternal = fakeReleaseInternal;
    runtime.startPressureTask = fakeStartPressureTask;
    runtime.writeEvidence = fakeWrite;
    runtime.persistentClockMs = fakePersistentClockMs;
    runtime.context = &fixture;
    return runtime;
}

WifiBsc02HilPressurePlannerParameters planner() {
    WifiBsc02HilPressurePlannerParameters result{};
    result.triggerFreeBytes = kTriggerFree;
    result.triggerLargestBlockBytes = kTriggerLargest;
    result.safetyFreeBytes = kSafetyFree;
    result.safetyLargestBlockBytes = kSafetyLargest;
    result.absoluteMinimumFreeBytes = WifiBsc02HilFaultModule::kAbsoluteMinimumFreeBytes;
    result.absoluteMinimumLargestBlockBytes = WifiBsc02HilFaultModule::kAbsoluteMinimumLargestBlockBytes;
    result.allocationCapBytes = WifiBsc02HilFaultModule::kMaximumPressureBytes;
    result.chunkBytes = WifiBsc02HilFaultModule::kPressureChunkBytes;
    result.allocationReserveBytes = WifiBsc02HilFaultModule::kPressureAllocationReserveBytes;
    return result;
}

void configureModule(WifiBsc02HilFaultModule& module) {
    module.configurePressurePlanner(planner());
}

void feed(HilFaultRuntimeOwner& owner, const char* command, const uint32_t nowMs) {
    for (const char* cursor = command; *cursor != '\0'; ++cursor) {
        owner.acceptSerialByte(*cursor, nowMs);
    }
    owner.acceptSerialByte('\n', nowMs);
}

bool stageNextBoot(const HilArmedFaultIdentity& identity, const uint32_t deadlineMs, const uint32_t stagedAtMs,
                   void* context) noexcept {
    return static_cast<WifiBsc02HilFaultModule*>(context)->stageNextBoot(identity, deadlineMs, stagedAtMs);
}

void clearNextBoot(void* context) noexcept {
    static_cast<WifiBsc02HilFaultModule*>(context)->clearNextBoot();
}

void beginAndArm(HilFaultRuntimeOwner& owner, const char* fault, const uint32_t armSequence, const uint32_t nowMs,
                 const uint32_t durationMs = 60000) {
    char command[224]{};
    std::snprintf(command, sizeof(command), "V1HIL BEGIN BSC-02 %s %lu", kSessionHashText,
                  static_cast<unsigned long>(durationMs));
    feed(owner, command, nowMs);
    std::snprintf(command, sizeof(command), "V1HIL ARM BSC-02 %s %s %lu", fault, kSessionHashText,
                  static_cast<unsigned long>(armSequence));
    feed(owner, command, nowMs + 1);
}

void stageApNextBoot(HilFaultRuntimeOwner& owner, const uint32_t armSequence, const uint32_t nowMs) {
    char command[224]{};
    std::snprintf(command, sizeof(command), "V1HIL NEXT_BOOT BSC-02 wifi-ap-start-fail-once %s %lu", kSessionHashText,
                  static_cast<unsigned long>(armSequence));
    feed(owner, command, nowMs);
}
} // namespace

void test_next_boot_record_is_deadline_bound_cleared_and_consumed_once() {
    Fixture fixture{};
    HilFaultRuntimeOwner stagingOwner;
    WifiBsc02HilNextBootRecord rtc{};
    WifiBsc02HilFaultModule staging(stagingOwner, rtc, 10, makeRuntime(fixture));
    stagingOwner.configureSerial(fakeWrite, &fixture);
    stagingOwner.configureNextBoot(stageNextBoot, clearNextBoot, &staging);
    beginAndArm(stagingOwner, "wifi-ap-start-fail-once", 7, 100);
    stageApNextBoot(stagingOwner, 7, 102);
    TEST_ASSERT_EQUAL_UINT32(WifiBsc02HilFaultModule::kNextBootMagic, rtc.magic);
    TEST_ASSERT_EQUAL_UINT32(102, rtc.stagedAtMs);
    TEST_ASSERT_EQUAL_UINT32(60100, rtc.sessionDeadlineMs);
    TEST_ASSERT_EQUAL_UINT32(59998, rtc.remainingSessionMs);
    TEST_ASSERT_EQUAL_UINT64(1000, rtc.persistentStagedAtMs);
    TEST_ASSERT_EQUAL_UINT64(60998, rtc.persistentDeadlineMs);
    const WifiBsc02HilNextBootRecord stagedCopy = rtc;

    fixture.persistentNowMs = 1100;
    HilFaultRuntimeOwner restoredOwner;
    WifiBsc02HilFaultModule restored(restoredOwner, rtc, 11, makeRuntime(fixture));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilFaultResult::Ok),
                            static_cast<uint8_t>(restored.restoreNextBoot(true, 1000)));
    TEST_ASSERT_EQUAL_UINT32(0, rtc.magic);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilFaultState::Armed),
                            static_cast<uint8_t>(restored.controllerSnapshot(HilFaultId::WifiApStartFailOnce).state));
    // One hundred milliseconds elapsed on the persistent clock before boot.
    // Restoration must subtract it instead of granting the staging-time
    // remainder again.
    restoredOwner.service(60897);
    TEST_ASSERT_TRUE(restoredOwner.controller().sessionActive());
    restoredOwner.service(60898);
    TEST_ASSERT_FALSE(restoredOwner.controller().sessionActive());

    rtc = stagedCopy;
    fixture.persistentNowMs = 1200;
    HilFaultRuntimeOwner staleOwner;
    WifiBsc02HilFaultModule stale(staleOwner, rtc, 12, makeRuntime(fixture));
    TEST_ASSERT_NOT_EQUAL(static_cast<uint8_t>(HilFaultResult::Ok),
                          static_cast<uint8_t>(stale.restoreNextBoot(true, 1100)));
    TEST_ASSERT_EQUAL_UINT32(0, rtc.magic);

    rtc = stagedCopy;
    fixture.persistentNowMs = stagedCopy.persistentDeadlineMs;
    HilFaultRuntimeOwner expiredOwner;
    WifiBsc02HilFaultModule expired(expiredOwner, rtc, 11, makeRuntime(fixture));
    TEST_ASSERT_NOT_EQUAL(static_cast<uint8_t>(HilFaultResult::Ok),
                          static_cast<uint8_t>(expired.restoreNextBoot(true, 1200)));
    TEST_ASSERT_EQUAL_UINT32(0, rtc.magic);
}

void test_next_boot_record_is_invalidated_on_end_and_session_expiry() {
    {
        Fixture fixture{};
        HilFaultRuntimeOwner owner;
        WifiBsc02HilNextBootRecord rtc{};
        WifiBsc02HilFaultModule module(owner, rtc, 20, makeRuntime(fixture));
        owner.configureNextBoot(stageNextBoot, clearNextBoot, &module);
        beginAndArm(owner, "wifi-ap-start-fail-once", 9, 200);
        stageApNextBoot(owner, 9, 202);
        TEST_ASSERT_NOT_EQUAL_UINT32(0, rtc.magic);
        char command[128]{};
        std::snprintf(command, sizeof(command), "V1HIL END BSC-02 %s", kSessionHashText);
        feed(owner, command, 203);
        TEST_ASSERT_EQUAL_UINT32(0, rtc.magic);
    }
    {
        Fixture fixture{};
        HilFaultRuntimeOwner owner;
        WifiBsc02HilNextBootRecord rtc{};
        WifiBsc02HilFaultModule module(owner, rtc, 30, makeRuntime(fixture));
        owner.configureNextBoot(stageNextBoot, clearNextBoot, &module);
        beginAndArm(owner, "wifi-ap-start-fail-once", 11, 300, 100);
        stageApNextBoot(owner, 11, 302);
        TEST_ASSERT_NOT_EQUAL_UINT32(0, rtc.magic);
        owner.service(400);
        TEST_ASSERT_EQUAL_UINT32(0, rtc.magic);
        TEST_ASSERT_FALSE(owner.controller().sessionActive());
    }
    {
        Fixture fixture{};
        HilFaultRuntimeOwner owner;
        WifiBsc02HilNextBootRecord rtc{};
        WifiBsc02HilFaultModule module(owner, rtc, 31, makeRuntime(fixture));
        owner.configureNextBoot(stageNextBoot, clearNextBoot, &module);
        constexpr uint32_t nearWrap = UINT32_MAX - 50u;
        beginAndArm(owner, "wifi-ap-start-fail-once", 12, nearWrap, 51);
        stageApNextBoot(owner, 12, nearWrap + 2u);
        TEST_ASSERT_NOT_EQUAL_UINT32(0, rtc.magic);
        owner.service(UINT32_MAX);
        TEST_ASSERT_TRUE(owner.controller().sessionActive());
        owner.service(0);
        TEST_ASSERT_EQUAL_UINT32(0, rtc.magic);
        TEST_ASSERT_FALSE(owner.controller().sessionActive());
    }
    {
        Fixture fixture{};
        HilFaultRuntimeOwner owner;
        WifiBsc02HilNextBootRecord rtc{};
        WifiBsc02HilFaultModule module(owner, rtc, 32, makeRuntime(fixture));
        owner.configureNextBoot(stageNextBoot, clearNextBoot, &module);
        beginAndArm(owner, "wifi-ap-start-fail-once", 14, 500);
        stageApNextBoot(owner, 14, 502);
        HilArmedFaultIdentity identity{};
        TEST_ASSERT_TRUE(owner.armedIdentity(HilFaultId::WifiApStartFailOnce, identity));
        TEST_ASSERT_NOT_EQUAL_UINT32(0, rtc.magic);
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilFaultResult::Ok),
                                static_cast<uint8_t>(owner.abortSession(identity)));
        TEST_ASSERT_EQUAL_UINT32(0, rtc.magic);
        TEST_ASSERT_FALSE(owner.controller().sessionActive());
    }
}

void test_corrupt_nonmaintenance_or_deadline_forgery_is_cleared() {
    Fixture fixture{};
    HilFaultRuntimeOwner owner;
    WifiBsc02HilNextBootRecord rtc{};
    WifiBsc02HilFaultModule module(owner, rtc, 40, makeRuntime(fixture));
    owner.configureNextBoot(stageNextBoot, clearNextBoot, &module);
    beginAndArm(owner, "wifi-ap-start-fail-once", 13, 400);
    HilArmedFaultIdentity identity{};
    TEST_ASSERT_TRUE(owner.armedIdentity(HilFaultId::WifiApStartFailOnce, identity));
    TEST_ASSERT_TRUE(module.stageNextBoot(identity, 60400, 402));
    rtc.remainingSessionMs -= 1;
    HilFaultRuntimeOwner forgedOwner;
    WifiBsc02HilFaultModule forged(forgedOwner, rtc, 41, makeRuntime(fixture));
    TEST_ASSERT_NOT_EQUAL(static_cast<uint8_t>(HilFaultResult::Ok),
                          static_cast<uint8_t>(forged.restoreNextBoot(true, 500)));
    TEST_ASSERT_EQUAL_UINT32(0, rtc.magic);

    TEST_ASSERT_TRUE(module.stageNextBoot(identity, 60400, 403));
    HilFaultRuntimeOwner nonmaintenanceOwner;
    WifiBsc02HilFaultModule nonmaintenance(nonmaintenanceOwner, rtc, 41, makeRuntime(fixture));
    TEST_ASSERT_NOT_EQUAL(static_cast<uint8_t>(HilFaultResult::Ok),
                          static_cast<uint8_t>(nonmaintenance.restoreNextBoot(false, 501)));
    TEST_ASSERT_EQUAL_UINT32(0, rtc.magic);
}

void test_ap_terminal_waits_for_verified_cleanup_and_retry_can_succeed() {
    Fixture fixture{};
    HilFaultRuntimeOwner stagingOwner;
    WifiBsc02HilNextBootRecord rtc{};
    WifiBsc02HilFaultModule staging(stagingOwner, rtc, 50, makeRuntime(fixture));
    stagingOwner.configureNextBoot(stageNextBoot, clearNextBoot, &staging);
    beginAndArm(stagingOwner, "wifi-ap-start-fail-once", 15, 500);
    stageApNextBoot(stagingOwner, 15, 502);

    HilFaultRuntimeOwner owner;
    WifiBsc02HilFaultModule module(owner, rtc, 51, makeRuntime(fixture));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilFaultResult::Ok),
                            static_cast<uint8_t>(module.restoreNextBoot(true, 1000)));
    HilArmedFaultIdentity restoredIdentity{};
    TEST_ASSERT_TRUE(owner.armedIdentity(HilFaultId::WifiApStartFailOnce, restoredIdentity));
    TEST_ASSERT_FALSE(module.shouldSuppressFreshApStart(false, true, false, 1001));
    TEST_ASSERT_FALSE(module.shouldSuppressFreshApStart(true, false, false, 1001));
    TEST_ASSERT_FALSE(module.shouldSuppressFreshApStart(true, true, true, 1001));
    TEST_ASSERT_TRUE(module.shouldSuppressFreshApStart(true, true, false, 1001));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilFaultState::Fired),
                            static_cast<uint8_t>(module.controllerSnapshot(HilFaultId::WifiApStartFailOnce).state));
    TEST_ASSERT_NULL(std::strstr(fixture.serialOutput, "\"hil_event\":\"terminal\""));
    TEST_ASSERT_FALSE(module.finalizeSuppressedApStart(true, false, false, 1002));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilFaultState::Fired),
                            static_cast<uint8_t>(module.controllerSnapshot(HilFaultId::WifiApStartFailOnce).state));
    TEST_ASSERT_TRUE(module.finalizeSuppressedApStart(false, false, false, 1003));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilFaultState::Released),
                            static_cast<uint8_t>(module.controllerSnapshot(HilFaultId::WifiApStartFailOnce).state));
    TEST_ASSERT_NOT_NULL(std::strstr(fixture.serialOutput, "\"reason\":\"released_after_suppression\""));
    // The later maintenance retry has no armed one-shot and therefore proceeds
    // to the real setupAP path instead of being falsely suppressed again.
    TEST_ASSERT_FALSE(module.shouldSuppressFreshApStart(true, true, false, 4003));
    // The immutable evidence identity remains bound after release, but the
    // released controller state must never be eligible for another reboot.
    TEST_ASSERT_FALSE(module.stageNextBoot(restoredIdentity, 60000, 4004));
    TEST_ASSERT_EQUAL_UINT32(0, rtc.magic);
}

void test_pressure_planner_separates_trigger_safety_and_absolute_floors() {
    const WifiBsc02HilPressurePlannerParameters parameters = planner();
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(WifiBsc02HilPressureDecision::Allocate),
        static_cast<uint8_t>(WifiBsc02HilFaultModule::planPressureStep(parameters, {20 * 1024, 12 * 1024}, 0)));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(WifiBsc02HilPressureDecision::TriggerReached),
        static_cast<uint8_t>(WifiBsc02HilFaultModule::planPressureStep(parameters, {15 * 1024, 7 * 1024}, 1024)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(WifiBsc02HilPressureDecision::SafetyBreach),
                            static_cast<uint8_t>(WifiBsc02HilFaultModule::planPressureStep(
                                parameters, {kSafetyFree - 1, kSafetyLargest}, 1024)));
    WifiBsc02HilPressurePlannerParameters invalid = parameters;
    invalid.safetyFreeBytes = invalid.absoluteMinimumFreeBytes;
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(WifiBsc02HilPressureDecision::InvalidParameters),
        static_cast<uint8_t>(WifiBsc02HilFaultModule::planPressureStep(invalid, {20 * 1024, 12 * 1024}, 0)));
}

void test_pressure_crosses_trigger_then_guard_observes_after_1500ms() {
    Fixture fixture{};
    fixture.freeInternal = 24 * 1024;
    fixture.largestInternal = 16 * 1024;
    HilFaultRuntimeOwner owner;
    WifiBsc02HilNextBootRecord rtc{};
    WifiBsc02HilFaultModule module(owner, rtc, 60, makeRuntime(fixture));
    configureModule(module);
    beginAndArm(owner, "wifi-internal-sram-hold", 17, 1000);
    module.serviceSramPressure(true, true, 1002);
    TEST_ASSERT_TRUE(fixture.taskStarted);
    TEST_ASSERT_EQUAL_UINT32(0, module.pressureSnapshot().freeInternalBefore);

    uint32_t triggerAt = 0;
    for (uint32_t now = 1003; now < 1100; ++now) {
        TEST_ASSERT_TRUE(module.pressureTaskTick(now));
        if (module.pressureSnapshot().triggerReached) {
            triggerAt = now;
            break;
        }
    }
    TEST_ASSERT_NOT_EQUAL_UINT32(0, triggerAt);
    const WifiBsc02HilPressureSnapshot pressured = module.pressureSnapshot();
    TEST_ASSERT_EQUAL_UINT32(24 * 1024 - kTaskOverheadFree, pressured.freeInternalBefore);
    TEST_ASSERT_EQUAL_UINT32(kTaskOverheadFree, pressured.taskOverheadBytes);
    TEST_ASSERT_TRUE(pressured.minimumFreeBytes >= kSafetyFree);
    TEST_ASSERT_TRUE(pressured.minimumLargestBlockBytes >= kSafetyLargest);
    TEST_ASSERT_TRUE(fixture.freeInternal < kTriggerFree || fixture.largestInternal < kTriggerLargest);
    TEST_ASSERT_FALSE(pressured.competingOperationObserved);
    module.service(triggerAt + 1499);
    TEST_ASSERT_FALSE(module.observeHeapGuardStop(triggerAt + 1499, triggerAt, 1500));
    TEST_ASSERT_FALSE(module.pressureSnapshot().competingOperationObserved);
    TEST_ASSERT_TRUE(module.observeHeapGuardStop(triggerAt + 1500, triggerAt, 1500));
    TEST_ASSERT_TRUE(module.pressureSnapshot().competingOperationObserved);

    const HilFaultSnapshot controllerState = module.controllerSnapshot(HilFaultId::WifiInternalSramHold);
    char releaseCommand[256]{};
    std::snprintf(releaseCommand, sizeof(releaseCommand),
                  "V1HIL RELEASE BSC-02 wifi-internal-sram-hold %s 17 %lu %lu %u", kSessionHashText,
                  static_cast<unsigned long>(controllerState.readySequence),
                  static_cast<unsigned long>(controllerState.activeGeneration),
                  static_cast<unsigned>(controllerState.exactPhase));
    feed(owner, releaseCommand, triggerAt + 1501);
    TEST_ASSERT_FALSE(module.pressureTaskTick(triggerAt + 1502));
    TEST_ASSERT_EQUAL_UINT32(fixture.allocationCount, fixture.releaseCount);
    const char* ready = std::strstr(fixture.serialOutput, "\"hil_event\":\"ready\"");
    const char* fired = std::strstr(fixture.serialOutput, "\"hil_event\":\"fired\"");
    const char* competing = std::strstr(fixture.serialOutput, "\"hil_event\":\"competing_observed\"");
    const char* terminal = std::strstr(fixture.serialOutput, "\"hil_event\":\"terminal\"");
    TEST_ASSERT_NOT_NULL(ready);
    TEST_ASSERT_NOT_NULL(fired);
    TEST_ASSERT_NOT_NULL(competing);
    TEST_ASSERT_NOT_NULL(terminal);
    TEST_ASSERT_TRUE(ready < fired && fired < competing && competing < terminal);
}

void test_cross_task_heap_stop_handoff_retries_until_pressure_is_fired() {
    Fixture fixture{};
    fixture.freeInternal = 24 * 1024;
    fixture.largestInternal = 16 * 1024;
    HilFaultRuntimeOwner owner;
    WifiBsc02HilNextBootRecord rtc{};
    WifiBsc02HilFaultModule module(owner, rtc, 70, makeRuntime(fixture));
    configureModule(module);
    beginAndArm(owner, "wifi-internal-sram-hold", 19, 2000);
    module.serviceSramPressure(true, true, 2002);
    TEST_ASSERT_TRUE(module.observeHeapGuardStop(2003, 503, 1500));
    TEST_ASSERT_TRUE(module.pressureSnapshot().heapStopPending);
    TEST_ASSERT_FALSE(module.pressureSnapshot().competingOperationObserved);
    TEST_ASSERT_TRUE(module.pressureTaskTick(2004));
    TEST_ASSERT_FALSE(module.pressureSnapshot().heapStopPending);
    TEST_ASSERT_TRUE(module.pressureSnapshot().competingOperationObserved);
}

void test_pressure_continuously_samples_and_aborts_on_safety_breach() {
    Fixture fixture{};
    fixture.freeInternal = 24 * 1024;
    fixture.largestInternal = 16 * 1024;
    HilFaultRuntimeOwner owner;
    WifiBsc02HilNextBootRecord rtc{};
    WifiBsc02HilFaultModule module(owner, rtc, 80, makeRuntime(fixture));
    configureModule(module);
    beginAndArm(owner, "wifi-internal-sram-hold", 21, 3000);
    module.serviceSramPressure(true, true, 3002);
    for (uint32_t now = 3003; now < 3100 && !module.pressureSnapshot().triggerReached; ++now) {
        TEST_ASSERT_TRUE(module.pressureTaskTick(now));
    }
    TEST_ASSERT_TRUE(module.pressureSnapshot().triggerReached);
    fixture.freeInternal = kSafetyFree - 1;
    TEST_ASSERT_FALSE(module.pressureTaskTick(3101));
    const WifiBsc02HilPressureSnapshot snapshot = module.pressureSnapshot();
    TEST_ASSERT_TRUE(snapshot.safetyBreach);
    TEST_ASSERT_FALSE(snapshot.taskActive);
    TEST_ASSERT_EQUAL_UINT32(fixture.allocationCount, fixture.releaseCount);
    TEST_ASSERT_TRUE(owner.controller().sessionActive());
    TEST_ASSERT_NULL(std::strstr(fixture.serialOutput, "\"reason\":\"pressure_safety_floor_breach\""));
    module.service(3102);
    TEST_ASSERT_FALSE(owner.controller().sessionActive());
    TEST_ASSERT_NOT_NULL(std::strstr(fixture.serialOutput, "\"reason\":\"pressure_safety_floor_breach\""));
}

void test_pressure_rejects_task_overhead_outside_fixed_bound() {
    Fixture fixture{};
    fixture.taskOverheadFree = WifiBsc02HilFaultModule::kMaximumPressureTaskOverheadBytes + 1;
    HilFaultRuntimeOwner owner;
    WifiBsc02HilNextBootRecord rtc{};
    WifiBsc02HilFaultModule module(owner, rtc, 85, makeRuntime(fixture));
    configureModule(module);
    beginAndArm(owner, "wifi-internal-sram-hold", 22, 3500);
    module.serviceSramPressure(true, true, 3502);
    TEST_ASSERT_FALSE(module.pressureTaskTick(3503));
    TEST_ASSERT_EQUAL_UINT32(WifiBsc02HilFaultModule::kMaximumPressureTaskOverheadBytes + 1,
                             module.pressureSnapshot().taskOverheadBytes);
    TEST_ASSERT_EQUAL_UINT32(0, fixture.allocationCount);
    TEST_ASSERT_FALSE(module.pressureSnapshot().taskActive);
    TEST_ASSERT_TRUE(owner.controller().sessionActive());
    module.service(3504);
    TEST_ASSERT_FALSE(owner.controller().sessionActive());
}

void test_pressure_task_start_failure_aborts_and_emits_terminal_event() {
    Fixture fixture{};
    fixture.taskStartAllowed = false;
    HilFaultRuntimeOwner owner;
    WifiBsc02HilNextBootRecord rtc{};
    WifiBsc02HilFaultModule module(owner, rtc, 90, makeRuntime(fixture));
    configureModule(module);
    beginAndArm(owner, "wifi-internal-sram-hold", 23, 4000);
    module.serviceSramPressure(true, true, 4002);
    TEST_ASSERT_TRUE(fixture.taskStarted);
    TEST_ASSERT_FALSE(module.pressureSnapshot().taskActive);
    TEST_ASSERT_FALSE(owner.controller().sessionActive());
    TEST_ASSERT_NOT_NULL(std::strstr(fixture.serialOutput, "\"reason\":\"pressure_task_start_failed\""));
    TEST_ASSERT_EQUAL_UINT32(0, fixture.allocationCount);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_next_boot_record_is_deadline_bound_cleared_and_consumed_once);
    RUN_TEST(test_next_boot_record_is_invalidated_on_end_and_session_expiry);
    RUN_TEST(test_corrupt_nonmaintenance_or_deadline_forgery_is_cleared);
    RUN_TEST(test_ap_terminal_waits_for_verified_cleanup_and_retry_can_succeed);
    RUN_TEST(test_pressure_planner_separates_trigger_safety_and_absolute_floors);
    RUN_TEST(test_pressure_crosses_trigger_then_guard_observes_after_1500ms);
    RUN_TEST(test_cross_task_heap_stop_handoff_retries_until_pressure_is_fired);
    RUN_TEST(test_pressure_continuously_samples_and_aborts_on_safety_breach);
    RUN_TEST(test_pressure_rejects_task_overhead_outside_fixed_bound);
    RUN_TEST(test_pressure_task_start_failure_aborts_and_emits_terminal_event);
    return UNITY_END();
}

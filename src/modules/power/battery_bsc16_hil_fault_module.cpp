#if defined(V1SIMPLE_HIL_FAULT_CONTROL)

#include "battery_bsc16_hil_fault_module.h"

#include <cstdio>

#if defined(ARDUINO_ARCH_ESP32)
#include <Arduino.h>
#include <esp_attr.h>
#include <esp_rtc_time.h>
RTC_DATA_ATTR static HilNextBootFaultRecord gBatteryBsc16NextBootRecord;
RTC_DATA_ATTR static uint32_t gBatteryBsc16BootSequence;
#else
static HilNextBootFaultRecord gBatteryBsc16NextBootRecord;
static uint32_t gBatteryBsc16BootSequence;
#endif

BatteryBsc16HilFaultModule::BatteryBsc16HilFaultModule(HilFaultRuntimeOwner& owner,
                                                       HilNextBootFaultRecord& nextBootRecord,
                                                       const uint32_t currentBootSequence,
                                                       const BatteryBsc16HilRuntime& runtime) noexcept
    : owner_(owner),
      nextBootStore_(owner, nextBootRecord, currentBootSequence, HilCaseId::Bsc16, HilFaultId::BatteryAdcInitFailOnce,
                     kNextBootMagic, runtime.persistentClockMs, runtime.context),
      runtime_(runtime) {}

void BatteryBsc16HilFaultModule::configure(const BatteryBsc16HilRuntime& runtime) noexcept {
    runtime_ = runtime;
    nextBootStore_.configureClock(runtime.persistentClockMs, runtime.context);
}

bool BatteryBsc16HilFaultModule::stageNextBoot(const HilArmedFaultIdentity& identity, const uint32_t sessionDeadlineMs,
                                               const uint32_t stagedAtMs) noexcept {
    return nextBootStore_.stage(identity, sessionDeadlineMs, stagedAtMs);
}

void BatteryBsc16HilFaultModule::clearNextBoot() noexcept {
    nextBootStore_.clear();
}

HilFaultResult BatteryBsc16HilFaultModule::restoreNextBoot(const uint32_t nowMs) noexcept {
    return nextBootStore_.restore(true, nowMs);
}

uint32_t BatteryBsc16HilFaultModule::nextGeneration() noexcept {
    ++generationCounter_;
    if (generationCounter_ == 0) {
        ++generationCounter_;
    }
    return generationCounter_;
}

void BatteryBsc16HilFaultModule::emitEvent(const char* event, const char* reason) noexcept {
    if (runtime_.writeEvidence == nullptr || event == nullptr || reason == nullptr) {
        return;
    }
    char response[512]{};
    const int written = std::snprintf(
        response, sizeof(response),
        "{\"hil_event\":\"%s\",\"reason\":\"%s\",\"case_id\":\"BSC-16\","
        "\"fault_id\":\"battery-adc-init-fail-once\",\"arm_sequence\":%lu,"
        "\"ready_sequence\":%lu,\"generation\":%lu,\"phase\":%u,"
        "\"latch_initialized\":%s,\"adc_handle_allocated\":false,\"voltage_valid\":false,"
        "\"source_classification\":\"%s\",\"power_button_enabled\":%s}\n",
        event, reason, static_cast<unsigned long>(identity_.armSequence),
        static_cast<unsigned long>(identity_.readySequence), static_cast<unsigned long>(identity_.generation),
        static_cast<unsigned>(kAdcAdmissionPhase), admission_.latchInitialized ? "true" : "false",
        battery_source_policy::sourceName(admission_.sourceClassification),
        admission_.powerButtonWillBeEnabled ? "true" : "false");
    if (written > 0 && static_cast<size_t>(written) < sizeof(response)) {
        runtime_.writeEvidence(response, runtime_.context);
    }
}

bool BatteryBsc16HilFaultModule::beginAdcAdmission(const BatteryBsc16HilAdcAdmission& admission,
                                                   const uint32_t nowMs) noexcept {
    owner_.service(nowMs);
    if (admissionAttempted_ || !admission.latchInitialized ||
        !battery_source_policy::resolveOnBattery(admission.sourceClassification) ||
        !admission.powerButtonWillBeEnabled) {
        return false;
    }

    HilArmedFaultIdentity armed{};
    if (!owner_.armedIdentity(HilFaultId::BatteryAdcInitFailOnce, armed) || armed.caseId != HilCaseId::Bsc16) {
        return false;
    }
    admissionAttempted_ = true;
    admission_ = admission;
    identity_.sessionHash = armed.sessionHash;
    identity_.armSequence = armed.armSequence;
    identity_.generation = nextGeneration();

    const HilReadyResult ready = owner_.controller().publishReady(
        HilCaseId::Bsc16, HilFaultId::BatteryAdcInitFailOnce, identity_.sessionHash, identity_.armSequence,
        identity_.generation, kAdcAdmissionPhase, nowMs, kAutomaticReleaseMs);
    if (ready.result != HilFaultResult::Ok) {
        return false;
    }
    identity_.readySequence = ready.readySequence;
    emitEvent("ready", "after_power_latch_before_adc_admission");

    const HilFaultResult fired = owner_.controller().fire(
        HilCaseId::Bsc16, HilFaultId::BatteryAdcInitFailOnce, identity_.sessionHash, identity_.armSequence,
        identity_.readySequence, identity_.generation, kAdcAdmissionPhase, nowMs);
    if (fired != HilFaultResult::Ok) {
        return false;
    }
    suppressionPending_ = true;
    emitEvent("fired", "adc_admission_fail_once");
    return true;
}

void BatteryBsc16HilFaultModule::completeAdcAdmissionSuppression(const uint32_t nowMs) noexcept {
    if (!suppressionPending_) {
        return;
    }
    suppressionPending_ = false;
    const HilFaultResult observed = owner_.controller().observeCompetingOperation(
        HilCaseId::Bsc16, HilFaultId::BatteryAdcInitFailOnce, identity_.sessionHash, identity_.armSequence,
        identity_.readySequence, identity_.generation, kAdcAdmissionPhase, nowMs);
    if (observed != HilFaultResult::Ok) {
        return;
    }
    const HilFaultResult released = owner_.controller().release(
        HilCaseId::Bsc16, HilFaultId::BatteryAdcInitFailOnce, identity_.sessionHash, identity_.armSequence,
        identity_.readySequence, identity_.generation, kAdcAdmissionPhase, nowMs);
    if (released != HilFaultResult::Ok) {
        return;
    }
    admissionSuppressed_ = true;
    emitEvent("released", "adc_admission_suppressed");
}

void BatteryBsc16HilFaultModule::service(const uint32_t nowMs) noexcept {
    owner_.service(nowMs);
}

HilFaultSnapshot BatteryBsc16HilFaultModule::controllerSnapshot() const noexcept {
    return owner_.controller().snapshot(HilFaultId::BatteryAdcInitFailOnce);
}

BatteryBsc16HilSnapshot BatteryBsc16HilFaultModule::snapshot() const noexcept {
    BatteryBsc16HilSnapshot result{};
    result.admissionAttempted = admissionAttempted_;
    result.suppressionPending = suppressionPending_;
    result.admissionSuppressed = admissionSuppressed_;
    result.latchInitialized = admission_.latchInitialized;
    result.powerButtonWillBeEnabled = admission_.powerButtonWillBeEnabled;
    result.sourceClassification = admission_.sourceClassification;
    result.armSequence = identity_.armSequence;
    result.readySequence = identity_.readySequence;
    result.generation = identity_.generation;
    return result;
}

BatteryBsc16HilFaultModule& batteryBsc16HilFaultModule() noexcept {
    static BatteryBsc16HilFaultModule module(hilFaultRuntimeOwner(), gBatteryBsc16NextBootRecord,
                                             gBatteryBsc16BootSequence);
    return module;
}

#if defined(ARDUINO_ARCH_ESP32)
namespace {
void deviceWriteEvidence(const char* line, void*) noexcept {
    if (line != nullptr) {
        Serial.print(line);
    }
}

uint64_t devicePersistentClockMs(void*) noexcept {
    return esp_rtc_get_time_us() / 1000ULL;
}
} // namespace
#endif

void configureBatteryBsc16HilDeviceRuntime() noexcept {
#if defined(ARDUINO_ARCH_ESP32)
    static bool configuredThisBoot = false;
    if (!configuredThisBoot) {
        ++gBatteryBsc16BootSequence;
        if (gBatteryBsc16BootSequence == 0) {
            ++gBatteryBsc16BootSequence;
        }
        configuredThisBoot = true;
    }
    BatteryBsc16HilRuntime runtime{};
    runtime.writeEvidence = deviceWriteEvidence;
    runtime.persistentClockMs = devicePersistentClockMs;
    batteryBsc16HilFaultModule().configure(runtime);
#endif
}

#endif // V1SIMPLE_HIL_FAULT_CONTROL

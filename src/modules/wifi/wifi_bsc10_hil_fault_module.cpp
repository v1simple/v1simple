#if defined(V1SIMPLE_HIL_FAULT_CONTROL)

#include "wifi_bsc10_hil_fault_module.h"

#include <cstdio>

#if defined(ARDUINO_ARCH_ESP32)
#include <Arduino.h>
#endif

WifiBsc10HilFaultModule::WifiBsc10HilFaultModule(HilFaultRuntimeOwner& owner,
                                                 const WifiBsc10HilRuntime& runtime) noexcept
    : owner_(owner), runtime_(runtime) {}

void WifiBsc10HilFaultModule::configure(const WifiBsc10HilRuntime& runtime) noexcept {
    runtime_ = runtime;
}

uint32_t WifiBsc10HilFaultModule::nextGeneration() noexcept {
    ++generationCounter_;
    if (generationCounter_ == 0) {
        ++generationCounter_;
    }
    return generationCounter_;
}

void WifiBsc10HilFaultModule::emitEvent(const char* event, const char* reason, const bool admitted) noexcept {
    if (runtime_.writeEvidence == nullptr || event == nullptr || reason == nullptr) {
        return;
    }
    char response[448]{};
    const int written = std::snprintf(
        response, sizeof(response),
        "{\"hil_event\":\"%s\",\"reason\":\"%s\",\"case_id\":\"BSC-10\","
        "\"fault_id\":\"wifi-enable-admission-fail-once\",\"arm_sequence\":%lu,"
        "\"ready_sequence\":%lu,\"generation\":%lu,\"phase\":%u,"
        "\"prior_enabled_state\":%s,\"prior_lifecycle_state\":%u,"
        "\"prior_selected_slot\":%d,\"post_span_mutation_count\":0,\"admitted\":%s}\n",
        event, reason, static_cast<unsigned long>(armSequence_), static_cast<unsigned long>(readySequence_),
        static_cast<unsigned long>(generation_), static_cast<unsigned>(kLifecycleAdmissionPhase),
        admission_.persistedEnabled ? "true" : "false", static_cast<unsigned>(admission_.lifecycleState),
        admission_.selectedSlot, admitted ? "true" : "false");
    if (written > 0 && static_cast<size_t>(written) < sizeof(response)) {
        runtime_.writeEvidence(response, runtime_.context);
    }
}

bool WifiBsc10HilFaultModule::admitLifecycleStart(const WifiBsc10Admission& admission, const uint32_t nowMs) noexcept {
    owner_.service(nowMs);
    if (admissionAttempted_) {
        return true;
    }
    HilArmedFaultIdentity armed{};
    if (!owner_.armedIdentity(HilFaultId::WifiEnableAdmissionFailOnce, armed) || armed.caseId != HilCaseId::Bsc10) {
        return true;
    }

    admissionAttempted_ = true;
    admission_ = admission;
    sessionHash_ = armed.sessionHash;
    armSequence_ = armed.armSequence;
    generation_ = nextGeneration();
    const HilReadyResult ready = owner_.controller().publishReady(
        HilCaseId::Bsc10, HilFaultId::WifiEnableAdmissionFailOnce, sessionHash_, armSequence_, generation_,
        kLifecycleAdmissionPhase, nowMs, kAutomaticReleaseMs);
    if (ready.result != HilFaultResult::Ok) {
        return true;
    }
    readySequence_ = ready.readySequence;
    emitEvent("ready", "before_lifecycle_admission", true);
    if (owner_.controller().fire(HilCaseId::Bsc10, HilFaultId::WifiEnableAdmissionFailOnce, sessionHash_, armSequence_,
                                 readySequence_, generation_, kLifecycleAdmissionPhase, nowMs) != HilFaultResult::Ok) {
        return true;
    }
    emitEvent("fired", "lifecycle_admission_rejected", false);
    if (owner_.controller().observeCompetingOperation(HilCaseId::Bsc10, HilFaultId::WifiEnableAdmissionFailOnce,
                                                      sessionHash_, armSequence_, readySequence_, generation_,
                                                      kLifecycleAdmissionPhase, nowMs) != HilFaultResult::Ok) {
        return true;
    }
    if (owner_.controller().release(HilCaseId::Bsc10, HilFaultId::WifiEnableAdmissionFailOnce, sessionHash_,
                                    armSequence_, readySequence_, generation_, kLifecycleAdmissionPhase,
                                    nowMs) != HilFaultResult::Ok) {
        return true;
    }
    rejectionApplied_ = true;
    emitEvent("released", "rejection_committed_before_mutation", false);
    return false;
}

void WifiBsc10HilFaultModule::service(const uint32_t nowMs) noexcept {
    owner_.service(nowMs);
}

HilFaultSnapshot WifiBsc10HilFaultModule::controllerSnapshot() const noexcept {
    return owner_.controller().snapshot(HilFaultId::WifiEnableAdmissionFailOnce);
}

WifiBsc10HilSnapshot WifiBsc10HilFaultModule::snapshot() const noexcept {
    return {admissionAttempted_, rejectionApplied_, armSequence_, readySequence_, generation_};
}

WifiBsc10HilFaultModule& wifiBsc10HilFaultModule() noexcept {
    static WifiBsc10HilFaultModule module(hilFaultRuntimeOwner());
    return module;
}

#if defined(ARDUINO_ARCH_ESP32)
namespace {
void deviceWriteEvidence(const char* line, void*) noexcept {
    if (line != nullptr) {
        Serial.print(line);
    }
}
} // namespace
#endif

void configureWifiBsc10HilDeviceRuntime() noexcept {
#if defined(ARDUINO_ARCH_ESP32)
    WifiBsc10HilRuntime runtime{};
    runtime.writeEvidence = deviceWriteEvidence;
    wifiBsc10HilFaultModule().configure(runtime);
#endif
}

#endif // V1SIMPLE_HIL_FAULT_CONTROL

#if defined(V1SIMPLE_HIL_FAULT_CONTROL)

#include "obd_bsc13_hil_fault_module.h"

#include <cstdio>

#if defined(ARDUINO_ARCH_ESP32)
#include <Arduino.h>
#endif

ObdBsc13HilFaultModule::ObdBsc13HilFaultModule(HilFaultRuntimeOwner& owner, const ObdBsc13HilRuntime& runtime) noexcept
    : owner_(owner), runtime_(runtime) {}

void ObdBsc13HilFaultModule::configure(const ObdBsc13HilRuntime& runtime) noexcept {
    runtime_ = runtime;
}

bool ObdBsc13HilFaultModule::identitiesEqual(const HilArmedFaultIdentity& armed,
                                             const ActiveIdentity& active) noexcept {
    return armed.caseId == HilCaseId::Bsc13 && armed.faultId == HilFaultId::ObdPhysicalLinkPreownershipBarrierOnce &&
           armed.armSequence == active.armSequence && armed.sessionHash == active.sessionHash;
}

const char* ObdBsc13HilFaultModule::runtimeStateName(const uint8_t stateCode) noexcept {
    return stateCode == kConnectingRuntimeStateCode ? "connecting" : "other";
}

void ObdBsc13HilFaultModule::refreshArm() noexcept {
    if (stage_ == Stage::Active) {
        return;
    }

    HilArmedFaultIdentity armed{};
    if (!owner_.armedIdentity(HilFaultId::ObdPhysicalLinkPreownershipBarrierOnce, armed) ||
        armed.caseId != HilCaseId::Bsc13) {
        if (stage_ != Stage::Idle) {
            identity_ = ActiveIdentity{};
            admission_ = ObdBsc13HilAdmission{};
            request_ = ObdPhysicalLinkPreownershipRequest{};
            cancellationObserved_ = false;
            linkDownConfirmed_ = false;
            controllerReleaseRecorded_ = false;
            sessionOwnershipPermitted_ = false;
            stage_ = Stage::Idle;
        }
        return;
    }

    if ((stage_ == Stage::Armed || stage_ == Stage::Completed) && identitiesEqual(armed, identity_)) {
        return;
    }

    identity_ = ActiveIdentity{};
    identity_.sessionHash = armed.sessionHash;
    identity_.armSequence = armed.armSequence;
    admission_ = ObdBsc13HilAdmission{};
    request_ = ObdPhysicalLinkPreownershipRequest{};
    readyTimestampMs_ = 0;
    completionTimestampMs_ = 0;
    cancellationEpoch_ = 0;
    cancellationObserved_ = false;
    linkDownConfirmed_ = false;
    controllerReleaseRecorded_ = false;
    sessionOwnershipPermitted_ = false;
    stage_ = Stage::Armed;
}

void ObdBsc13HilFaultModule::emitEvent(const char* event, const char* reason) noexcept {
    if (runtime_.writeEvidence == nullptr || event == nullptr || reason == nullptr) {
        return;
    }
    char response[768]{};
    const int written = std::snprintf(
        response, sizeof(response),
        "{\"hil_event\":\"%s\",\"reason\":\"%s\",\"case_id\":\"BSC-13\","
        "\"fault_id\":\"obd-physical-link-preownership-barrier-once\",\"arm_sequence\":%lu,"
        "\"ready_sequence\":%lu,\"generation\":%lu,\"phase\":%u,"
        "\"runtime_state\":\"%s\",\"dispatch_epoch\":%lu,\"cancellation_epoch\":%lu,"
        "\"ready_timestamp_ms\":%lu,\"completion_timestamp_ms\":%lu,"
        "\"cancellation_observed\":%s,\"matching_link_down_confirmed\":%s,"
        "\"controller_release_recorded\":%s}\n",
        event, reason, static_cast<unsigned long>(identity_.armSequence),
        static_cast<unsigned long>(identity_.readySequence), static_cast<unsigned long>(identity_.generation),
        static_cast<unsigned>(kPhysicalLinkPreownershipPhase), runtimeStateName(admission_.runtimeStateCode),
        static_cast<unsigned long>(request_.dispatchEpoch), static_cast<unsigned long>(cancellationEpoch_),
        static_cast<unsigned long>(readyTimestampMs_), static_cast<unsigned long>(completionTimestampMs_),
        cancellationObserved_ ? "true" : "false", linkDownConfirmed_ ? "true" : "false",
        controllerReleaseRecorded_ ? "true" : "false");
    if (written > 0 && static_cast<size_t>(written) < sizeof(response)) {
        runtime_.writeEvidence(response, runtime_.evidenceContext);
    }
}

void ObdBsc13HilFaultModule::completeExpired(const char* reason, const uint32_t nowMs) noexcept {
    completionTimestampMs_ = nowMs;
    sessionOwnershipPermitted_ = true;
    emitEvent("expired", reason);
    stage_ = Stage::Completed;
}

void ObdBsc13HilFaultModule::evaluateBarrier(const uint32_t nowMs) noexcept {
    if (stage_ != Stage::Active) {
        return;
    }

    const HilFaultState controllerState = controllerSnapshot().state;
    if (controllerState == HilFaultState::Expired) {
        completeExpired("automatic_timeout_resumed_session_ownership", nowMs);
        return;
    }
    if (controllerState == HilFaultState::Disarmed) {
        completeExpired("session_end_resumed_session_ownership", nowMs);
        return;
    }

    const ObdPhysicalLinkPreownershipObservation observation =
        ObdPhysicalLinkPreownershipBarrier::observe(request_, runtime_.barrier);
    cancellationEpoch_ = observation.cancellationEpoch;
    completionTimestampMs_ = observation.observedAtMs;
    if (observation.outcome == ObdPhysicalLinkPreownershipOutcome::HoldOwnership) {
        return;
    }
    if (observation.outcome == ObdPhysicalLinkPreownershipOutcome::CancellationObserved) {
        cancellationObserved_ = true;
        return;
    }
    if (observation.outcome == ObdPhysicalLinkPreownershipOutcome::PreemptionConfirmed) {
        cancellationObserved_ = true;
        linkDownConfirmed_ = true;
        const HilFaultResult observed = owner_.controller().observeCompetingOperation(
            HilCaseId::Bsc13, HilFaultId::ObdPhysicalLinkPreownershipBarrierOnce, identity_.sessionHash,
            identity_.armSequence, identity_.readySequence, identity_.generation, kPhysicalLinkPreownershipPhase,
            observation.observedAtMs);
        const HilFaultResult released =
            observed == HilFaultResult::Ok
                ? owner_.controller().release(HilCaseId::Bsc13, HilFaultId::ObdPhysicalLinkPreownershipBarrierOnce,
                                              identity_.sessionHash, identity_.armSequence, identity_.readySequence,
                                              identity_.generation, kPhysicalLinkPreownershipPhase,
                                              observation.observedAtMs)
                : observed;
        controllerReleaseRecorded_ = released == HilFaultResult::Ok;
        emitEvent(controllerReleaseRecorded_ ? "released" : "blocked",
                  controllerReleaseRecorded_ ? "matching_generation_callback_down_after_preemption"
                                             : "controller_refused_preemption_release");
        stage_ = Stage::Completed;
        return;
    }
    if (observation.outcome == ObdPhysicalLinkPreownershipOutcome::LinkDownWithoutCancellation) {
        linkDownConfirmed_ = true;
        emitEvent("blocked", "matching_link_down_without_preemption");
        stage_ = Stage::Completed;
        return;
    }
    completeExpired(observation.outcome == ObdPhysicalLinkPreownershipOutcome::DeadlineReached
                        ? "automatic_timeout_resumed_session_ownership"
                        : "invalid_runtime_resumed_session_ownership",
                    observation.observedAtMs);
}

bool ObdBsc13HilFaultModule::admitSessionOwnership(const ObdBsc13HilAdmission& admission,
                                                   const uint32_t nowMs) noexcept {
    service(nowMs);
    if (!admission.physicalLinkConnected || admission.activeGeneration == 0 ||
        admission.runtimeStateCode != kConnectingRuntimeStateCode) {
        return true;
    }

    if (stage_ == Stage::Active) {
        if (admission.activeGeneration != identity_.generation) {
            completionTimestampMs_ = nowMs;
            emitEvent("blocked", "active_generation_changed_before_completion");
            sessionOwnershipPermitted_ = true;
            stage_ = Stage::Completed;
            return true;
        }
        evaluateBarrier(nowMs);
        return stage_ != Stage::Active && sessionOwnershipPermitted_;
    }
    if (stage_ == Stage::Completed) {
        // The captured generation is never adopted after a confirmed teardown,
        // but the one-shot barrier must not strand the later reconnect that the
        // ordinary coordinator admits after preemption is removed.
        if (linkDownConfirmed_ && admission.activeGeneration != identity_.generation) {
            return true;
        }
        return sessionOwnershipPermitted_;
    }
    if (stage_ != Stage::Armed || !ObdPhysicalLinkPreownershipBarrier::runtimeValid(runtime_.barrier)) {
        return true;
    }

    admission_ = admission;
    identity_.generation = admission.activeGeneration;
    request_ = {runtime_.barrier.cancellationEpoch(runtime_.barrier.context), admission.activeGeneration, nowMs,
                kAutomaticReleaseMs};
    cancellationEpoch_ = request_.dispatchEpoch;
    const HilReadyResult ready = owner_.controller().publishReady(
        HilCaseId::Bsc13, HilFaultId::ObdPhysicalLinkPreownershipBarrierOnce, identity_.sessionHash,
        identity_.armSequence, identity_.generation, kPhysicalLinkPreownershipPhase, nowMs, kAutomaticReleaseMs);
    if (ready.result != HilFaultResult::Ok) {
        sessionOwnershipPermitted_ = true;
        stage_ = Stage::Completed;
        return true;
    }
    identity_.readySequence = ready.readySequence;
    readyTimestampMs_ =
        owner_.controller().snapshot(HilFaultId::ObdPhysicalLinkPreownershipBarrierOnce).readyTimestampMs;
    emitEvent("ready", "physical_link_before_session_ownership");

    const HilFaultResult fired = owner_.controller().fire(
        HilCaseId::Bsc13, HilFaultId::ObdPhysicalLinkPreownershipBarrierOnce, identity_.sessionHash,
        identity_.armSequence, identity_.readySequence, identity_.generation, kPhysicalLinkPreownershipPhase, nowMs);
    if (fired != HilFaultResult::Ok) {
        sessionOwnershipPermitted_ = true;
        stage_ = Stage::Completed;
        return true;
    }
    emitEvent("fired", "session_ownership_held");
    stage_ = Stage::Active;
    evaluateBarrier(nowMs);
    return stage_ != Stage::Active && sessionOwnershipPermitted_;
}

void ObdBsc13HilFaultModule::service(const uint32_t nowMs) noexcept {
    owner_.service(nowMs);
    evaluateBarrier(nowMs);
    refreshArm();
}

HilFaultSnapshot ObdBsc13HilFaultModule::controllerSnapshot() const noexcept {
    return owner_.controller().snapshot(HilFaultId::ObdPhysicalLinkPreownershipBarrierOnce);
}

ObdBsc13HilSnapshot ObdBsc13HilFaultModule::snapshot() const noexcept {
    return {stage_ == Stage::Armed, stage_ == Stage::Active,    stage_ == Stage::Completed, cancellationObserved_,
            linkDownConfirmed_,     controllerReleaseRecorded_, identity_.armSequence,      identity_.readySequence,
            identity_.generation,   request_.dispatchEpoch,     cancellationEpoch_};
}

ObdBsc13HilFaultModule& obdBsc13HilFaultModule() noexcept {
    static ObdBsc13HilFaultModule module(hilFaultRuntimeOwner());
    return module;
}

#if defined(ARDUINO_ARCH_ESP32)
namespace {
uint32_t deviceClockMs(void*) noexcept {
    return static_cast<uint32_t>(millis());
}

void deviceWriteEvidence(const char* line, void*) noexcept {
    if (line != nullptr) {
        Serial.print(line);
    }
}
} // namespace
#endif

void configureObdBsc13HilDeviceRuntime(
    const ObdPhysicalLinkPreownershipRuntime::ReadCancellationEpoch cancellationEpoch,
    const ObdPhysicalLinkPreownershipRuntime::LinkDownConfirmed linkDownConfirmed, void* const probeContext) noexcept {
#if defined(ARDUINO_ARCH_ESP32)
    ObdBsc13HilRuntime runtime{};
    runtime.barrier = {deviceClockMs, cancellationEpoch, linkDownConfirmed, probeContext};
    runtime.writeEvidence = deviceWriteEvidence;
    obdBsc13HilFaultModule().configure(runtime);
#else
    (void)cancellationEpoch;
    (void)linkDownConfirmed;
    (void)probeContext;
#endif
}

#endif // V1SIMPLE_HIL_FAULT_CONTROL

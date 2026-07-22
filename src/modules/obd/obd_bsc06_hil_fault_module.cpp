#if defined(V1SIMPLE_HIL_FAULT_CONTROL)

#include "obd_bsc06_hil_fault_module.h"

#include <cstdio>

#if defined(ARDUINO_ARCH_ESP32)
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

ObdBsc06HilFaultModule::ObdBsc06HilFaultModule(HilFaultRuntimeOwner& owner, const ObdBsc06HilRuntime& runtime) noexcept
    : owner_(owner), runtime_(runtime) {}

void ObdBsc06HilFaultModule::configure(const ObdBsc06HilRuntime& runtime) noexcept {
    runtime_ = runtime;
}

bool ObdBsc06HilFaultModule::identitiesEqual(const HilArmedFaultIdentity& armed,
                                             const ActiveIdentity& active) noexcept {
    return armed.caseId == HilCaseId::Bsc06 && armed.faultId == HilFaultId::ObdTransportOperationBarrierOnce &&
           armed.armSequence == active.armSequence && armed.sessionHash == active.sessionHash;
}

const char* ObdBsc06HilFaultModule::operationName(const ObdBsc06Operation operation) noexcept {
    return operation == ObdBsc06Operation::Write ? "write" : "none";
}

const char* ObdBsc06HilFaultModule::runtimeStateName(const uint8_t stateCode) noexcept {
    return stateCode == kPollingRuntimeStateCode ? "polling" : "other";
}

void ObdBsc06HilFaultModule::refreshArm() noexcept {
    const Stage stage = static_cast<Stage>(stage_.load());
    if (stage == Stage::Pausing || stage == Stage::Paused) {
        return;
    }

    HilArmedFaultIdentity armed{};
    if (!owner_.armedIdentity(HilFaultId::ObdTransportOperationBarrierOnce, armed) ||
        armed.caseId != HilCaseId::Bsc06) {
        if (stage != Stage::Idle) {
            identity_ = ActiveIdentity{};
            admission_ = ObdBsc06HilAdmission{};
            readyEventPending_ = false;
            firedEventPending_ = false;
            completionEventPending_ = false;
            operationSuppressed_ = false;
            controllerReleaseRecorded_ = false;
            stage_.store(static_cast<uint32_t>(Stage::Idle));
        }
        return;
    }

    if ((stage == Stage::Armed || stage == Stage::Completed) && identitiesEqual(armed, identity_)) {
        return;
    }

    identity_ = ActiveIdentity{};
    identity_.sessionHash = armed.sessionHash;
    identity_.armSequence = armed.armSequence;
    admission_ = ObdBsc06HilAdmission{};
    outcome_ = ObdTransportBarrierOutcome::InvalidRuntime;
    controllerStateAtCompletion_ = HilFaultState::Disarmed;
    readyTimestampMs_ = 0;
    completionTimestampMs_ = 0;
    cancellationEpochAtCompletion_ = 0;
    linkDownGeneration_ = 0;
    readyEventPending_ = false;
    firedEventPending_ = false;
    completionEventPending_ = false;
    operationSuppressed_ = false;
    controllerReleaseRecorded_ = false;
    stage_.store(static_cast<uint32_t>(Stage::Armed));
}

bool ObdBsc06HilFaultModule::continuePause(const uint32_t nowMs, void* const context) noexcept {
    if (context == nullptr) {
        return false;
    }
    auto& module = *static_cast<ObdBsc06HilFaultModule*>(context);
    module.owner_.controller().service(nowMs);
    return module.owner_.controller().shouldPause(HilCaseId::Bsc06, HilFaultId::ObdTransportOperationBarrierOnce,
                                                  module.identity_.sessionHash, module.identity_.armSequence,
                                                  module.identity_.readySequence, module.identity_.generation,
                                                  kPollingWritePhase, nowMs);
}

bool ObdBsc06HilFaultModule::routeOperation(const ObdBsc06HilAdmission& admission, const uint32_t nowMs) noexcept {
    if (admission.operation != ObdBsc06Operation::Write || admission.runtimeStateCode != kPollingRuntimeStateCode ||
        admission.activeGeneration == 0 || admission.requestId == 0 ||
        !ObdTransportOperationBarrier::runtimeValid(runtime_.barrier)) {
        return true;
    }

    uint32_t expected = static_cast<uint32_t>(Stage::Armed);
    if (!stage_.compareExchange(expected, static_cast<uint32_t>(Stage::Pausing))) {
        return true;
    }

    admission_ = admission;
    identity_.generation = admission.activeGeneration;
    const HilReadyResult ready = owner_.controller().publishReady(
        HilCaseId::Bsc06, HilFaultId::ObdTransportOperationBarrierOnce, identity_.sessionHash, identity_.armSequence,
        identity_.generation, kPollingWritePhase, nowMs, kAutomaticReleaseMs);
    if (ready.result != HilFaultResult::Ok) {
        stage_.store(static_cast<uint32_t>(Stage::Completed));
        return true;
    }
    identity_.readySequence = ready.readySequence;
    readyTimestampMs_ = owner_.controller().snapshot(HilFaultId::ObdTransportOperationBarrierOnce).readyTimestampMs;

    const HilFaultResult fired = owner_.controller().fire(
        HilCaseId::Bsc06, HilFaultId::ObdTransportOperationBarrierOnce, identity_.sessionHash, identity_.armSequence,
        identity_.readySequence, identity_.generation, kPollingWritePhase, nowMs);
    if (fired != HilFaultResult::Ok) {
        stage_.store(static_cast<uint32_t>(Stage::Completed));
        return true;
    }

    readyEventPending_ = true;
    firedEventPending_ = true;
    stage_.store(static_cast<uint32_t>(Stage::Paused));

    const ObdTransportBarrierRequest request{
        admission.dispatchEpoch,
        admission.activeGeneration,
        nowMs,
        kAutomaticReleaseMs,
    };
    outcome_ = ObdTransportOperationBarrier::wait(request, runtime_.barrier, continuePause, this);
    completionTimestampMs_ = runtime_.barrier.clockMs(runtime_.barrier.context);
    cancellationEpochAtCompletion_ = runtime_.barrier.cancellationEpoch(runtime_.barrier.context);
    if (outcome_ == ObdTransportBarrierOutcome::LinkDownConfirmed) {
        linkDownGeneration_ = admission.activeGeneration;
    }

    owner_.controller().service(completionTimestampMs_);
    const bool competingOperation = outcome_ == ObdTransportBarrierOutcome::CancellationEpochAdvanced ||
                                    outcome_ == ObdTransportBarrierOutcome::LinkDownConfirmed;
    if (competingOperation) {
        operationSuppressed_ = true;
        const HilFaultResult observed = owner_.controller().observeCompetingOperation(
            HilCaseId::Bsc06, HilFaultId::ObdTransportOperationBarrierOnce, identity_.sessionHash,
            identity_.armSequence, identity_.readySequence, identity_.generation, kPollingWritePhase,
            completionTimestampMs_);
        const HilFaultResult released =
            observed == HilFaultResult::Ok
                ? owner_.controller().release(HilCaseId::Bsc06, HilFaultId::ObdTransportOperationBarrierOnce,
                                              identity_.sessionHash, identity_.armSequence, identity_.readySequence,
                                              identity_.generation, kPollingWritePhase, completionTimestampMs_)
                : observed;
        controllerReleaseRecorded_ = released == HilFaultResult::Ok;
    }

    controllerStateAtCompletion_ = owner_.controller().snapshot(HilFaultId::ObdTransportOperationBarrierOnce).state;
    completionEventPending_ = true;
    stage_.store(static_cast<uint32_t>(Stage::Completed));
    return !operationSuppressed_;
}

void ObdBsc06HilFaultModule::emitEvent(const char* event, const char* reason) noexcept {
    if (runtime_.writeEvidence == nullptr || event == nullptr || reason == nullptr) {
        return;
    }
    char response[768]{};
    int written = 0;
    if (stage_.load() == static_cast<uint32_t>(Stage::Completed)) {
        written = std::snprintf(
            response, sizeof(response),
            "{\"hil_event\":\"%s\",\"reason\":\"%s\",\"case_id\":\"BSC-06\","
            "\"fault_id\":\"obd-transport-operation-barrier-once\",\"arm_sequence\":%lu,"
            "\"ready_sequence\":%lu,\"generation\":%lu,\"phase\":%u,"
            "\"request_id\":%lu,\"dispatch_epoch\":%lu,\"operation\":\"%s\","
            "\"runtime_state\":\"%s\",\"cancellation_epoch\":%lu,"
            "\"link_down_generation\":%lu,\"ready_timestamp_ms\":%lu,"
            "\"completion_timestamp_ms\":%lu,\"operation_suppressed\":%s,"
            "\"controller_release_recorded\":%s}\n",
            event, reason, static_cast<unsigned long>(identity_.armSequence),
            static_cast<unsigned long>(identity_.readySequence), static_cast<unsigned long>(identity_.generation),
            static_cast<unsigned>(kPollingWritePhase), static_cast<unsigned long>(admission_.requestId),
            static_cast<unsigned long>(admission_.dispatchEpoch), operationName(admission_.operation),
            runtimeStateName(admission_.runtimeStateCode), static_cast<unsigned long>(cancellationEpochAtCompletion_),
            static_cast<unsigned long>(linkDownGeneration_), static_cast<unsigned long>(readyTimestampMs_),
            static_cast<unsigned long>(completionTimestampMs_), operationSuppressed_ ? "true" : "false",
            controllerReleaseRecorded_ ? "true" : "false");
    } else {
        written = std::snprintf(
            response, sizeof(response),
            "{\"hil_event\":\"%s\",\"reason\":\"%s\",\"case_id\":\"BSC-06\","
            "\"fault_id\":\"obd-transport-operation-barrier-once\",\"arm_sequence\":%lu,"
            "\"ready_sequence\":%lu,\"generation\":%lu,\"phase\":%u,"
            "\"request_id\":%lu,\"dispatch_epoch\":%lu,\"operation\":\"%s\","
            "\"runtime_state\":\"%s\",\"ready_timestamp_ms\":%lu}\n",
            event, reason, static_cast<unsigned long>(identity_.armSequence),
            static_cast<unsigned long>(identity_.readySequence), static_cast<unsigned long>(identity_.generation),
            static_cast<unsigned>(kPollingWritePhase), static_cast<unsigned long>(admission_.requestId),
            static_cast<unsigned long>(admission_.dispatchEpoch), operationName(admission_.operation),
            runtimeStateName(admission_.runtimeStateCode), static_cast<unsigned long>(readyTimestampMs_));
    }
    if (written > 0 && static_cast<size_t>(written) < sizeof(response)) {
        runtime_.writeEvidence(response, runtime_.evidenceContext);
    }
}

void ObdBsc06HilFaultModule::emitPendingEvents() noexcept {
    const Stage stage = static_cast<Stage>(stage_.load());
    if (stage != Stage::Paused && stage != Stage::Completed) {
        return;
    }
    if (readyEventPending_) {
        emitEvent("ready", "polling_write_after_epoch_claim");
        readyEventPending_ = false;
    }
    if (firedEventPending_) {
        emitEvent("fired", "transport_owner_barrier_active");
        firedEventPending_ = false;
    }
    if (stage != Stage::Completed || !completionEventPending_) {
        return;
    }

    if (outcome_ == ObdTransportBarrierOutcome::CancellationEpochAdvanced) {
        emitEvent(controllerReleaseRecorded_ ? "released" : "blocked", controllerReleaseRecorded_
                                                                           ? "newer_cancellation_epoch_suppressed_write"
                                                                           : "controller_refused_cancellation_release");
    } else if (outcome_ == ObdTransportBarrierOutcome::LinkDownConfirmed) {
        emitEvent(controllerReleaseRecorded_ ? "released" : "blocked", controllerReleaseRecorded_
                                                                           ? "matching_link_down_suppressed_write"
                                                                           : "controller_refused_link_down_release");
    } else if (outcome_ == ObdTransportBarrierOutcome::DeadlineReached ||
               controllerStateAtCompletion_ == HilFaultState::Expired) {
        emitEvent("expired", "automatic_timeout_resumed_write");
    } else if (controllerStateAtCompletion_ == HilFaultState::Disarmed) {
        emitEvent("expired", "session_end_resumed_write");
    } else {
        emitEvent("blocked", "barrier_runtime_ended_without_competing_operation");
    }
    completionEventPending_ = false;
}

void ObdBsc06HilFaultModule::service(const uint32_t nowMs) noexcept {
    owner_.service(nowMs);
    emitPendingEvents();
    refreshArm();
}

HilFaultSnapshot ObdBsc06HilFaultModule::controllerSnapshot() const noexcept {
    return owner_.controller().snapshot(HilFaultId::ObdTransportOperationBarrierOnce);
}

ObdBsc06HilSnapshot ObdBsc06HilFaultModule::snapshot() const noexcept {
    const Stage stage = static_cast<Stage>(stage_.load());
    return {stage == Stage::Armed,         stage == Stage::Pausing || stage == Stage::Paused,
            stage == Stage::Completed,     operationSuppressed_,
            controllerReleaseRecorded_,    identity_.armSequence,
            identity_.readySequence,       identity_.generation,
            admission_.requestId,          admission_.dispatchEpoch,
            cancellationEpochAtCompletion_};
}

ObdBsc06HilFaultModule& obdBsc06HilFaultModule() noexcept {
    static ObdBsc06HilFaultModule module(hilFaultRuntimeOwner());
    return module;
}

#if defined(ARDUINO_ARCH_ESP32)
namespace {
uint32_t deviceClockMs(void*) noexcept {
    return static_cast<uint32_t>(millis());
}

void deviceYieldTransportOwner(void*) noexcept {
    vTaskDelay(1);
}

void deviceWriteEvidence(const char* line, void*) noexcept {
    if (line != nullptr) {
        Serial.print(line);
    }
}
} // namespace
#endif

void configureObdBsc06HilDeviceRuntime(const ObdTransportBarrierRuntime::ReadCancellationEpoch cancellationEpoch,
                                       const ObdTransportBarrierRuntime::LinkDownConfirmed linkDownConfirmed,
                                       void* const probeContext) noexcept {
#if defined(ARDUINO_ARCH_ESP32)
    ObdBsc06HilRuntime runtime{};
    runtime.barrier = {deviceClockMs, cancellationEpoch, linkDownConfirmed, deviceYieldTransportOwner, probeContext};
    runtime.writeEvidence = deviceWriteEvidence;
    obdBsc06HilFaultModule().configure(runtime);
#else
    (void)cancellationEpoch;
    (void)linkDownConfirmed;
    (void)probeContext;
#endif
}

#endif // V1SIMPLE_HIL_FAULT_CONTROL

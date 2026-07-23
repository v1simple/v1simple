#if defined(V1SIMPLE_HIL_FAULT_CONTROL)

#include "wifi_bsc02_hil_fault_module.h"

#include <cstdio>

#if defined(ARDUINO_ARCH_ESP32)
#include <Arduino.h>
#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <esp_rtc_time.h>
RTC_DATA_ATTR static WifiBsc02HilNextBootRecord gWifiBsc02NextBootRecord;
RTC_DATA_ATTR static uint32_t gWifiBsc02BootSequence;
#else
static WifiBsc02HilNextBootRecord gWifiBsc02NextBootRecord;
static uint32_t gWifiBsc02BootSequence;
#endif

WifiBsc02HilFaultModule::WifiBsc02HilFaultModule(HilFaultRuntimeOwner& owner,
                                                 WifiBsc02HilNextBootRecord& nextBootRecord,
                                                 const uint32_t currentBootSequence,
                                                 const WifiBsc02HilRuntime& runtime) noexcept
    : owner_(owner),
      nextBootStore_(owner, nextBootRecord, currentBootSequence, HilCaseId::Bsc02, HilFaultId::WifiApStartFailOnce,
                     kNextBootMagic, runtime.persistentClockMs, runtime.context),
      runtime_(runtime) {}

void WifiBsc02HilFaultModule::configure(const WifiBsc02HilRuntime& runtime) noexcept {
    runtime_ = runtime;
    nextBootStore_.configureClock(runtime.persistentClockMs, runtime.context);
}

void WifiBsc02HilFaultModule::configurePressurePlanner(
    const WifiBsc02HilPressurePlannerParameters& parameters) noexcept {
    pressurePlanner_ = parameters;
}

bool WifiBsc02HilFaultModule::stageNextBoot(const HilArmedFaultIdentity& identity, const uint32_t sessionDeadlineMs,
                                            const uint32_t stagedAtMs) noexcept {
    return nextBootStore_.stage(identity, sessionDeadlineMs, stagedAtMs);
}

void WifiBsc02HilFaultModule::clearNextBoot() noexcept {
    nextBootStore_.clear();
}

HilFaultResult WifiBsc02HilFaultModule::restoreNextBoot(const bool maintenanceActive, const uint32_t nowMs) noexcept {
    return nextBootStore_.restore(maintenanceActive, nowMs);
}

uint32_t WifiBsc02HilFaultModule::nextGeneration() noexcept {
    ++generationCounter_;
    if (generationCounter_ == 0) {
        ++generationCounter_;
    }
    return generationCounter_;
}

void WifiBsc02HilFaultModule::emitEvent(const HilFaultId faultId, const char* event, const char* reason,
                                        const ActiveIdentity& identity) noexcept {
    if (runtime_.writeEvidence == nullptr || event == nullptr || reason == nullptr) {
        return;
    }
    char response[224]{};
    const int written = std::snprintf(
        response, sizeof(response),
        "{\"hil_event\":\"%s\",\"reason\":\"%s\",\"case_id\":\"BSC-02\","
        "\"fault_id\":\"%s\",\"arm_sequence\":%lu,\"ready_sequence\":%lu,"
        "\"generation\":%lu,\"phase\":%u}\n",
        event, reason, HilFaultController::faultName(faultId), static_cast<unsigned long>(identity.armSequence),
        static_cast<unsigned long>(identity.readySequence), static_cast<unsigned long>(identity.generation),
        static_cast<unsigned>(identity.phase));
    if (written > 0 && static_cast<size_t>(written) < sizeof(response)) {
        runtime_.writeEvidence(response, runtime_.context);
    }
}

bool WifiBsc02HilFaultModule::shouldSuppressFreshApStart(const bool maintenanceActive, const bool freshSetupAdmission,
                                                         const bool serviceReachable, const uint32_t nowMs) noexcept {
    HilFaultController& controller = owner_.controller();
    owner_.service(nowMs);
    HilArmedFaultIdentity armed{};
    if (!maintenanceActive || !freshSetupAdmission || serviceReachable || apFirePending_.load() != 0 ||
        apCleanupPending_.load() != 0 || !owner_.armedIdentity(HilFaultId::WifiApStartFailOnce, armed)) {
        return false;
    }
    const HilFaultSnapshot snapshot = controller.snapshot(HilFaultId::WifiApStartFailOnce);
    if (snapshot.state != HilFaultState::Armed || snapshot.armSequence != armed.armSequence) {
        return false;
    }

    apIdentity_ = ActiveIdentity{};
    apIdentity_.sessionHash = armed.sessionHash;
    apIdentity_.armSequence = armed.armSequence;
    apIdentity_.generation = nextGeneration();
    apIdentity_.phase = kApAdmissionPhase;
    const HilReadyResult ready = controller.publishReady(
        HilCaseId::Bsc02, HilFaultId::WifiApStartFailOnce, apIdentity_.sessionHash, apIdentity_.armSequence,
        apIdentity_.generation, apIdentity_.phase, nowMs, kApAutomaticReleaseMs);
    if (ready.result != HilFaultResult::Ok) {
        return false;
    }
    apIdentity_.readySequence = ready.readySequence;
    apFired_.store(0);
    apCompetitionObserved_.store(0);
    apTerminalEmitted_.store(0);
    apCleanupPending_.store(0);
    emitEvent(HilFaultId::WifiApStartFailOnce, "ready", "fresh_ap_admission", apIdentity_);
    // The ready reservation is enough to suppress the single real softAP call.
    // If the controller mutation gate is momentarily busy, service() retries the
    // fire without letting the one-shot admission escape into production state.
    apFirePending_.store(1);
    attemptApLifecycle(nowMs);
    return true;
}

bool WifiBsc02HilFaultModule::finalizeSuppressedApStart(const bool interfaceActive, const bool serviceActive,
                                                        const bool serviceReachable, const uint32_t nowMs) noexcept {
    if (interfaceActive || serviceActive || serviceReachable || apIdentity_.readySequence == 0 ||
        apTerminalEmitted_.load() != 0) {
        return false;
    }
    apCleanupPending_.store(1);
    attemptApLifecycle(nowMs);
    return true;
}

void WifiBsc02HilFaultModule::attemptApLifecycle(const uint32_t nowMs) noexcept {
    HilAtomicLease attempt(apAttemptGate_);
    if (!attempt.acquired()) {
        return;
    }
    HilFaultController& controller = owner_.controller();
    if (apFirePending_.load() != 0) {
        const HilFaultResult fired = controller.fire(
            HilCaseId::Bsc02, HilFaultId::WifiApStartFailOnce, apIdentity_.sessionHash, apIdentity_.armSequence,
            apIdentity_.readySequence, apIdentity_.generation, apIdentity_.phase, nowMs);
        const HilFaultSnapshot snapshot = controller.snapshot(HilFaultId::WifiApStartFailOnce);
        const bool alreadyFired =
            fired == HilFaultResult::DuplicateFire && snapshot.state == HilFaultState::Fired &&
            snapshot.armSequence == apIdentity_.armSequence && snapshot.readySequence == apIdentity_.readySequence &&
            snapshot.activeGeneration == apIdentity_.generation && snapshot.exactPhase == apIdentity_.phase;
        const bool retryableReady =
            fired == HilFaultResult::WrongState && snapshot.state == HilFaultState::Ready &&
            snapshot.armSequence == apIdentity_.armSequence && snapshot.readySequence == apIdentity_.readySequence &&
            snapshot.activeGeneration == apIdentity_.generation && snapshot.exactPhase == apIdentity_.phase;
        if (fired == HilFaultResult::Ok || alreadyFired) {
            apFirePending_.store(0);
            if (apFired_.load() == 0) {
                apFired_.store(1);
                emitEvent(HilFaultId::WifiApStartFailOnce, "fired", "softap_admission_suppressed", apIdentity_);
            }
        } else if (fired != HilFaultResult::Busy && !retryableReady) {
            apFirePending_.store(0);
        }
    }

    if (apCleanupPending_.load() == 0 || apFired_.load() == 0 || apTerminalEmitted_.load() != 0) {
        return;
    }
    if (apCompetitionObserved_.load() == 0) {
        const HilFaultResult observed = controller.observeCompetingOperation(
            HilCaseId::Bsc02, HilFaultId::WifiApStartFailOnce, apIdentity_.sessionHash, apIdentity_.armSequence,
            apIdentity_.readySequence, apIdentity_.generation, apIdentity_.phase, nowMs);
        if (observed == HilFaultResult::Busy) {
            return;
        }
        if (observed != HilFaultResult::Ok) {
            return;
        }
        apCompetitionObserved_.store(1);
    }
    const HilFaultResult terminal = controller.release(
        HilCaseId::Bsc02, HilFaultId::WifiApStartFailOnce, apIdentity_.sessionHash, apIdentity_.armSequence,
        apIdentity_.readySequence, apIdentity_.generation, apIdentity_.phase, nowMs);
    if (terminal == HilFaultResult::Busy) {
        return;
    }
    if (terminal == HilFaultResult::Ok) {
        apCleanupPending_.store(0);
        apTerminalEmitted_.store(1);
        emitEvent(HilFaultId::WifiApStartFailOnce, "terminal", "released_after_suppression", apIdentity_);
    }
}

WifiBsc02HilPressureDecision
WifiBsc02HilFaultModule::planPressureStep(const WifiBsc02HilPressurePlannerParameters& parameters,
                                          const WifiBsc02HilHeapMetrics& metrics,
                                          const uint32_t allocatedBytes) noexcept {
    if (parameters.triggerFreeBytes <= parameters.safetyFreeBytes ||
        parameters.triggerLargestBlockBytes <= parameters.safetyLargestBlockBytes ||
        parameters.safetyFreeBytes <= parameters.absoluteMinimumFreeBytes ||
        parameters.safetyLargestBlockBytes <= parameters.absoluteMinimumLargestBlockBytes ||
        parameters.allocationCapBytes == 0 || parameters.allocationCapBytes > kMaximumPressureBytes ||
        parameters.chunkBytes != kPressureChunkBytes || parameters.allocationReserveBytes == 0) {
        return WifiBsc02HilPressureDecision::InvalidParameters;
    }
    if (metrics.freeBytes < parameters.safetyFreeBytes ||
        metrics.largestBlockBytes < parameters.safetyLargestBlockBytes) {
        return WifiBsc02HilPressureDecision::SafetyBreach;
    }
    if (metrics.freeBytes < parameters.triggerFreeBytes ||
        metrics.largestBlockBytes < parameters.triggerLargestBlockBytes) {
        return WifiBsc02HilPressureDecision::TriggerReached;
    }
    if (allocatedBytes >= parameters.allocationCapBytes ||
        parameters.allocationCapBytes - allocatedBytes < parameters.chunkBytes) {
        return WifiBsc02HilPressureDecision::CapReached;
    }
    const uint32_t freeHeadroom = metrics.freeBytes - parameters.safetyFreeBytes;
    const uint32_t largestHeadroom = metrics.largestBlockBytes - parameters.safetyLargestBlockBytes;
    const uint32_t required = parameters.chunkBytes + parameters.allocationReserveBytes;
    if (freeHeadroom < required || largestHeadroom < required) {
        return WifiBsc02HilPressureDecision::CapReached;
    }
    return WifiBsc02HilPressureDecision::Allocate;
}

void WifiBsc02HilFaultModule::serviceSramPressure(const bool maintenanceActive, const bool serviceReachable,
                                                  const uint32_t nowMs) noexcept {
    HilFaultController& controller = owner_.controller();
    owner_.service(nowMs);
    HilArmedFaultIdentity armed{};
    if (!maintenanceActive || !serviceReachable || !owner_.armedIdentity(HilFaultId::WifiInternalSramHold, armed) ||
        pressureTaskActive_.load() != 0 || runtime_.freeInternal == nullptr || runtime_.largestInternal == nullptr ||
        runtime_.allocateInternal == nullptr || runtime_.releaseInternal == nullptr ||
        runtime_.startPressureTask == nullptr || pressurePlanner_.triggerFreeBytes == 0 ||
        pressurePlanner_.triggerLargestBlockBytes == 0 || pressurePlanner_.safetyFreeBytes == 0 ||
        pressurePlanner_.safetyLargestBlockBytes == 0) {
        return;
    }
    const HilFaultSnapshot snapshot = controller.snapshot(HilFaultId::WifiInternalSramHold);
    if (snapshot.state != HilFaultState::Armed || snapshot.armSequence != armed.armSequence) {
        return;
    }

    pressureIdentity_ = ActiveIdentity{};
    pressureIdentity_.sessionHash = armed.sessionHash;
    pressureIdentity_.armSequence = armed.armSequence;
    pressureIdentity_.generation = nextGeneration();
    pressureIdentity_.phase = kSramPressurePhase;
    const HilReadyResult ready =
        controller.publishReady(HilCaseId::Bsc02, HilFaultId::WifiInternalSramHold, pressureIdentity_.sessionHash,
                                pressureIdentity_.armSequence, pressureIdentity_.generation, pressureIdentity_.phase,
                                nowMs, kPressureAutomaticReleaseMs);
    if (ready.result != HilFaultResult::Ok) {
        return;
    }
    pressureIdentity_.readySequence = ready.readySequence;
    pressureAllocationCount_ = 0;
    pressureAllocatedBytes_.store(0);
    pressurePreTaskFree_.store(runtime_.freeInternal(runtime_.context));
    pressureTaskOverheadBytes_.store(0);
    pressureBaselineCaptured_.store(0);
    pressureFired_.store(0);
    pressureAllocationFinished_.store(0);
    pressureTriggerReached_.store(0);
    pressureSafetyBreach_.store(0);
    pressureAbortReason_.store(static_cast<uint32_t>(PressureAbortReason::None));
    pressureHeapStopPending_.store(0);
    pressureHeapStopTimestampMs_.store(0);
    pressureCompetingObserved_.store(0);
    pressureFreeBefore_.store(0);
    pressureLargestBefore_.store(0);
    pressureFreeAfter_.store(0);
    pressureLargestAfter_.store(0);
    pressureMinimumFree_.store(0);
    pressureMinimumLargest_.store(0);
    pressureTaskActive_.store(1);
    pressureTerminalEmitted_.store(0);
    emitEvent(HilFaultId::WifiInternalSramHold, "ready", "pressure_task_admission", pressureIdentity_);
    if (!runtime_.startPressureTask(runtime_.context)) {
        pressureTaskActive_.store(0);
        HilArmedFaultIdentity failedIdentity{};
        failedIdentity.caseId = HilCaseId::Bsc02;
        failedIdentity.faultId = HilFaultId::WifiInternalSramHold;
        failedIdentity.sessionHash = pressureIdentity_.sessionHash;
        failedIdentity.armSequence = pressureIdentity_.armSequence;
        (void)owner_.abortSession(failedIdentity);
        pressureTerminalEmitted_.store(1);
        emitEvent(HilFaultId::WifiInternalSramHold, "terminal", "pressure_task_start_failed", pressureIdentity_);
    }
}

WifiBsc02HilHeapMetrics WifiBsc02HilFaultModule::samplePressureMetrics() noexcept {
    WifiBsc02HilHeapMetrics metrics{};
    metrics.freeBytes = runtime_.freeInternal(runtime_.context);
    metrics.largestBlockBytes = runtime_.largestInternal(runtime_.context);
    const auto updateMinimum = [](HilAtomicUint32& minimum, const uint32_t value) noexcept {
        uint32_t current = minimum.load();
        while ((current == 0 || value < current) && !minimum.compareExchange(current, value)) {
        }
    };
    updateMinimum(pressureMinimumFree_, metrics.freeBytes);
    updateMinimum(pressureMinimumLargest_, metrics.largestBlockBytes);
    return metrics;
}

bool WifiBsc02HilFaultModule::pressureSafetyBreached(const WifiBsc02HilHeapMetrics& metrics) const noexcept {
    return metrics.freeBytes < pressurePlanner_.safetyFreeBytes ||
           metrics.largestBlockBytes < pressurePlanner_.safetyLargestBlockBytes;
}

void WifiBsc02HilFaultModule::requestPressureAbort(const PressureAbortReason reason) noexcept {
    releasePressureAllocations();
    pressureSafetyBreach_.store(1);
    pressureTaskActive_.store(0);
    pressureAbortReason_.store(static_cast<uint32_t>(reason));
}

const char* WifiBsc02HilFaultModule::pressureAbortReasonName(const PressureAbortReason reason) noexcept {
    switch (reason) {
    case PressureAbortReason::BaselineUnsafe:
        return "pressure_baseline_unsafe";
    case PressureAbortReason::FireFailed:
        return "pressure_fire_failed";
    case PressureAbortReason::SafetyFloorBreach:
        return "pressure_safety_floor_breach";
    case PressureAbortReason::PlannerAbort:
        return "pressure_planner_abort";
    case PressureAbortReason::PostAllocationBreach:
        return "pressure_post_alloc_breach";
    case PressureAbortReason::None:
        break;
    }
    return "pressure_abort_unknown";
}

void WifiBsc02HilFaultModule::consumePendingPressureAbort() noexcept {
    const PressureAbortReason reason = static_cast<PressureAbortReason>(pressureAbortReason_.load());
    if (reason == PressureAbortReason::None) {
        return;
    }
    HilArmedFaultIdentity identity{};
    identity.caseId = HilCaseId::Bsc02;
    identity.faultId = HilFaultId::WifiInternalSramHold;
    identity.sessionHash = pressureIdentity_.sessionHash;
    identity.armSequence = pressureIdentity_.armSequence;
    const HilFaultResult aborted = owner_.abortSession(identity);
    if (aborted == HilFaultResult::Busy) {
        return;
    }
    pressureAbortReason_.store(static_cast<uint32_t>(PressureAbortReason::None));
    if (pressureTerminalEmitted_.load() == 0) {
        pressureTerminalEmitted_.store(1);
        emitEvent(HilFaultId::WifiInternalSramHold, "terminal", pressureAbortReasonName(reason), pressureIdentity_);
    }
}

bool WifiBsc02HilFaultModule::pressureTaskTick(const uint32_t nowMs) noexcept {
    if (pressureTaskActive_.load() == 0) {
        return false;
    }
    HilFaultController& controller = owner_.controller();
    // The runtime owner is serviced only by the main loop. Its session binding
    // fields are intentionally not cross-task state; the pressure task talks
    // only to the controller's atomic API and samples its own pressure state.
    if (pressureBaselineCaptured_.load() == 0) {
        const WifiBsc02HilHeapMetrics baseline = samplePressureMetrics();
        const uint32_t preTaskFree = pressurePreTaskFree_.load();
        const uint32_t taskOverhead = preTaskFree > baseline.freeBytes ? preTaskFree - baseline.freeBytes : 0;
        pressureTaskOverheadBytes_.store(taskOverhead);
        pressureFreeBefore_.store(baseline.freeBytes);
        pressureLargestBefore_.store(baseline.largestBlockBytes);
        pressureBaselineCaptured_.store(1);
        const WifiBsc02HilPressureDecision admission = planPressureStep(pressurePlanner_, baseline, 0);
        if (taskOverhead > kMaximumPressureTaskOverheadBytes ||
            admission == WifiBsc02HilPressureDecision::InvalidParameters ||
            admission == WifiBsc02HilPressureDecision::SafetyBreach) {
            requestPressureAbort(PressureAbortReason::BaselineUnsafe);
            return false;
        }
    }

    if (pressureFired_.load() == 0) {
        const HilFaultResult fired =
            controller.fire(HilCaseId::Bsc02, HilFaultId::WifiInternalSramHold, pressureIdentity_.sessionHash,
                            pressureIdentity_.armSequence, pressureIdentity_.readySequence,
                            pressureIdentity_.generation, pressureIdentity_.phase, nowMs);
        const HilFaultSnapshot fireSnapshot = controller.snapshot(HilFaultId::WifiInternalSramHold);
        const bool retryableReady = fired == HilFaultResult::WrongState && fireSnapshot.state == HilFaultState::Ready &&
                                    fireSnapshot.armSequence == pressureIdentity_.armSequence &&
                                    fireSnapshot.readySequence == pressureIdentity_.readySequence &&
                                    fireSnapshot.activeGeneration == pressureIdentity_.generation &&
                                    fireSnapshot.exactPhase == pressureIdentity_.phase;
        if (fired == HilFaultResult::Busy || retryableReady) {
            return true;
        }
        if (fired != HilFaultResult::Ok) {
            requestPressureAbort(PressureAbortReason::FireFailed);
            return false;
        }
        pressureFired_.store(1);
        emitEvent(HilFaultId::WifiInternalSramHold, "fired", "pressure_task_start", pressureIdentity_);
    }

    const WifiBsc02HilHeapMetrics heldMetrics = samplePressureMetrics();
    if (pressureSafetyBreached(heldMetrics)) {
        requestPressureAbort(PressureAbortReason::SafetyFloorBreach);
        return false;
    }
    attemptPendingHeapStopObservation(nowMs);
    const bool hold =
        controller.shouldPause(HilCaseId::Bsc02, HilFaultId::WifiInternalSramHold, pressureIdentity_.sessionHash,
                               pressureIdentity_.armSequence, pressureIdentity_.readySequence,
                               pressureIdentity_.generation, pressureIdentity_.phase, nowMs);
    if (!hold) {
        releasePressureAllocations();
        pressureTaskActive_.store(0);
        if (pressureTerminalEmitted_.load() == 0) {
            const HilFaultState terminalState = controller.snapshot(HilFaultId::WifiInternalSramHold).state;
            emitEvent(HilFaultId::WifiInternalSramHold, "terminal",
                      terminalState == HilFaultState::Released ? "released" : "expired_auto_release",
                      pressureIdentity_);
            pressureTerminalEmitted_.store(1);
        }
        return false;
    }

    if (pressureAllocationFinished_.load() == 0) {
        const WifiBsc02HilPressureDecision decision =
            planPressureStep(pressurePlanner_, heldMetrics, pressureAllocatedBytes_.load());
        if (decision == WifiBsc02HilPressureDecision::TriggerReached) {
            pressureTriggerReached_.store(1);
            pressureAllocationFinished_.store(1);
        } else if (decision == WifiBsc02HilPressureDecision::CapReached) {
            pressureAllocationFinished_.store(1);
        } else if (decision == WifiBsc02HilPressureDecision::SafetyBreach ||
                   decision == WifiBsc02HilPressureDecision::InvalidParameters) {
            requestPressureAbort(PressureAbortReason::PlannerAbort);
            return false;
        } else if (pressureAllocationCount_ < pressureAllocations_.size()) {
            void* allocation = runtime_.allocateInternal(pressurePlanner_.chunkBytes, runtime_.context);
            if (allocation == nullptr) {
                pressureAllocationFinished_.store(1);
            } else {
                pressureAllocations_[pressureAllocationCount_++] = allocation;
                pressureAllocatedBytes_.store(pressureAllocatedBytes_.load() + pressurePlanner_.chunkBytes);
                const WifiBsc02HilHeapMetrics after = samplePressureMetrics();
                if (pressureSafetyBreached(after)) {
                    requestPressureAbort(PressureAbortReason::PostAllocationBreach);
                    return false;
                }
                if (after.freeBytes < pressurePlanner_.triggerFreeBytes ||
                    after.largestBlockBytes < pressurePlanner_.triggerLargestBlockBytes) {
                    pressureTriggerReached_.store(1);
                    pressureAllocationFinished_.store(1);
                }
            }
        }
    }
    attemptPendingHeapStopObservation(nowMs);
    return true;
}

bool WifiBsc02HilFaultModule::observeHeapGuardStop(const uint32_t nowMs, const uint32_t lowHeapSinceMs,
                                                   const uint32_t minimumPersistMs) noexcept {
    if (pressureTaskActive_.load() == 0 || pressureCompetingObserved_.load() != 0 || lowHeapSinceMs == 0 ||
        minimumPersistMs == 0 || static_cast<uint32_t>(nowMs - lowHeapSinceMs) < minimumPersistMs) {
        return false;
    }
    pressureHeapStopTimestampMs_.store(nowMs);
    pressureHeapStopPending_.store(1);
    attemptPendingHeapStopObservation(nowMs);
    return true;
}

void WifiBsc02HilFaultModule::attemptPendingHeapStopObservation(const uint32_t nowMs) noexcept {
    if (pressureHeapStopPending_.load() == 0 || pressureFired_.load() == 0 || pressureCompetingObserved_.load() != 0) {
        return;
    }
    HilFaultController& controller = owner_.controller();
    const HilFaultResult observed = controller.observeCompetingOperation(
        HilCaseId::Bsc02, HilFaultId::WifiInternalSramHold, pressureIdentity_.sessionHash,
        pressureIdentity_.armSequence, pressureIdentity_.readySequence, pressureIdentity_.generation,
        pressureIdentity_.phase,
        pressureHeapStopTimestampMs_.load() == 0 ? nowMs : pressureHeapStopTimestampMs_.load());
    if (observed == HilFaultResult::Busy || observed == HilFaultResult::WrongState) {
        return;
    }
    if (observed != HilFaultResult::Ok) {
        pressureHeapStopPending_.store(0);
        return;
    }
    pressureCompetingObserved_.store(1);
    pressureHeapStopPending_.store(0);
    emitEvent(HilFaultId::WifiInternalSramHold, "competing_observed", "wifi_heap_guard_stop", pressureIdentity_);
}

void WifiBsc02HilFaultModule::releasePressureAllocations() noexcept {
    while (pressureAllocationCount_ > 0) {
        --pressureAllocationCount_;
        void* allocation = pressureAllocations_[pressureAllocationCount_];
        pressureAllocations_[pressureAllocationCount_] = nullptr;
        if (allocation != nullptr && runtime_.releaseInternal != nullptr) {
            runtime_.releaseInternal(allocation, runtime_.context);
        }
    }
    if (runtime_.freeInternal != nullptr) {
        pressureFreeAfter_.store(runtime_.freeInternal(runtime_.context));
    }
    if (runtime_.largestInternal != nullptr) {
        pressureLargestAfter_.store(runtime_.largestInternal(runtime_.context));
    }
}

void WifiBsc02HilFaultModule::service(const uint32_t nowMs) noexcept {
    // Consume task-side requests before servicing expiry so the main loop is
    // the sole reader/writer of the runtime owner's non-atomic session state.
    consumePendingPressureAbort();
    owner_.service(nowMs);
    attemptApLifecycle(nowMs);
    attemptPendingHeapStopObservation(nowMs);
}

HilFaultSnapshot WifiBsc02HilFaultModule::controllerSnapshot(const HilFaultId faultId) const noexcept {
    return owner_.controller().snapshot(faultId);
}

WifiBsc02HilPressureSnapshot WifiBsc02HilFaultModule::pressureSnapshot() const noexcept {
    WifiBsc02HilPressureSnapshot snapshot{};
    snapshot.freeInternalBefore = pressureFreeBefore_.load();
    snapshot.freeInternalAfter = pressureFreeAfter_.load();
    snapshot.largestInternalBefore = pressureLargestBefore_.load();
    snapshot.largestInternalAfter = pressureLargestAfter_.load();
    snapshot.allocatedBytes = pressureAllocatedBytes_.load();
    snapshot.taskOverheadBytes = pressureTaskOverheadBytes_.load();
    snapshot.triggerFreeBytes = pressurePlanner_.triggerFreeBytes;
    snapshot.triggerLargestBlockBytes = pressurePlanner_.triggerLargestBlockBytes;
    snapshot.safetyFloorBytes = pressurePlanner_.safetyFreeBytes;
    snapshot.largestBlockSafetyFloorBytes = pressurePlanner_.safetyLargestBlockBytes;
    snapshot.minimumFreeBytes = pressureMinimumFree_.load();
    snapshot.minimumLargestBlockBytes = pressureMinimumLargest_.load();
    snapshot.readySequence = pressureIdentity_.readySequence;
    snapshot.taskActive = pressureTaskActive_.load() != 0;
    snapshot.triggerReached = pressureTriggerReached_.load() != 0;
    snapshot.safetyBreach = pressureSafetyBreach_.load() != 0;
    snapshot.heapStopPending = pressureHeapStopPending_.load() != 0;
    snapshot.competingOperationObserved = pressureCompetingObserved_.load() != 0;
    return snapshot;
}

WifiBsc02HilFaultModule& wifiBsc02HilFaultModule() noexcept {
    static WifiBsc02HilFaultModule module(hilFaultRuntimeOwner(), gWifiBsc02NextBootRecord, gWifiBsc02BootSequence);
    return module;
}

#if defined(ARDUINO_ARCH_ESP32)
namespace {
uint32_t deviceFreeInternal(void*) noexcept {
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

uint32_t deviceLargestInternal(void*) noexcept {
    return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

void* deviceAllocateInternal(const size_t bytes, void*) noexcept {
    return heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

void deviceReleaseInternal(void* allocation, void*) noexcept {
    heap_caps_free(allocation);
}

void deviceWriteEvidence(const char* line, void*) noexcept {
    if (line != nullptr) {
        Serial.print(line);
    }
}

uint64_t devicePersistentClockMs(void*) noexcept {
    return esp_rtc_get_time_us() / 1000ULL;
}

void devicePressureTask(void*) {
    while (wifiBsc02HilFaultModule().pressureTaskTick(millis())) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    vTaskDelete(nullptr);
}

bool deviceStartPressureTask(void*) noexcept {
    return xTaskCreatePinnedToCore(devicePressureTask, "bsc02_sram", 3072, nullptr, 1, nullptr, 0) == pdPASS;
}

} // namespace
#endif

void configureWifiBsc02HilDeviceRuntime(const uint32_t triggerFreeBytes,
                                        const uint32_t triggerLargestBlockBytes) noexcept {
#if defined(ARDUINO_ARCH_ESP32)
    static bool configuredThisBoot = false;
    if (!configuredThisBoot) {
        ++gWifiBsc02BootSequence;
        if (gWifiBsc02BootSequence == 0) {
            ++gWifiBsc02BootSequence;
        }
        configuredThisBoot = true;
    }
    WifiBsc02HilRuntime runtime{};
    runtime.freeInternal = deviceFreeInternal;
    runtime.largestInternal = deviceLargestInternal;
    runtime.allocateInternal = deviceAllocateInternal;
    runtime.releaseInternal = deviceReleaseInternal;
    runtime.startPressureTask = deviceStartPressureTask;
    runtime.writeEvidence = deviceWriteEvidence;
    runtime.persistentClockMs = devicePersistentClockMs;
    wifiBsc02HilFaultModule().configure(runtime);
    WifiBsc02HilPressurePlannerParameters planner{};
    planner.triggerFreeBytes = triggerFreeBytes;
    planner.triggerLargestBlockBytes = triggerLargestBlockBytes;
    planner.safetyFreeBytes = WifiBsc02HilFaultModule::kPressureSafetyFreeBytes;
    planner.safetyLargestBlockBytes = WifiBsc02HilFaultModule::kPressureSafetyLargestBlockBytes;
    planner.absoluteMinimumFreeBytes = WifiBsc02HilFaultModule::kAbsoluteMinimumFreeBytes;
    planner.absoluteMinimumLargestBlockBytes = WifiBsc02HilFaultModule::kAbsoluteMinimumLargestBlockBytes;
    planner.allocationCapBytes = WifiBsc02HilFaultModule::kMaximumPressureBytes;
    planner.chunkBytes = WifiBsc02HilFaultModule::kPressureChunkBytes;
    planner.allocationReserveBytes = WifiBsc02HilFaultModule::kPressureAllocationReserveBytes;
    wifiBsc02HilFaultModule().configurePressurePlanner(planner);
    hilFaultRuntimeOwner().configureSerial(deviceWriteEvidence, nullptr);
#else
    (void)triggerFreeBytes;
    (void)triggerLargestBlockBytes;
#endif
}

#endif

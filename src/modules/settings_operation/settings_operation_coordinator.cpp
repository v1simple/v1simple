#include "settings_operation_coordinator.h"

#include "ble_client.h"
#include "modules/auto_push/auto_push_module.h"
#include "v1_firmware_compat.h"

namespace {
constexpr uint32_t kFreshObservationWaitMs = 5000;

bool settingsSnapshotComplete(const V1DetectorSnapshot& snapshot) {
    const V1FirmwareCompat::Capabilities capabilities =
        V1FirmwareCompat::capabilities(snapshot.firmwareVersion);
    return snapshot.hasUserBytes &&
        (!capabilities.allVolume || snapshot.hasCurrentVolume) &&
        (!capabilities.modeObservation || snapshot.hasMode) &&
        (!capabilities.displayActive || snapshot.hasDisplayOn) &&
        (!capabilities.customSweeps || (snapshot.hasSweepSections && snapshot.hasMaxSweepIndex &&
                                         snapshot.hasSweepDefinitions));
}

} // namespace

void SettingsOperationCoordinator::beginNormalBoot() {
    const bool operationRecovered = settingsOperations_.beginNormalBoot();
    const auto bootOperation = settingsOperations_.snapshot();
    settingsOperationBootRecoveryPending_ =
        !operationRecovered && bootOperation.available && bootOperation.valid;
    settingsOperationBootRecoveryAttemptMs_ = millis();
}

void SettingsOperationCoordinator::process(uint32_t nowMs) {
    serviceOperation(nowMs);
    // Deadlines/cancellation precede the next wire-executor tick even when the
    // durable operation service returns early.
    autoPush_.process();
}

bool SettingsOperationCoordinator::acknowledgePendingMaintenanceReturn() {
    if (!settingsOperations_.isTerminal() || !settingsOperations_.snapshot().returnToMaintenance) return true;
    return settingsOperations_.acknowledgeReturnToMaintenance();
}

bool SettingsOperationCoordinator::beginSlotApply(int newSlot) {
    if (autoPush_.isActive() || settingsOperations_.requiresExclusiveDetectorAdmission()) return false;
    String address;
    if (!effects_.readConnectedAddress(address, effects_.context)) return false;
    const auto started = settingsOperations_.startApply(
        newSlot, address.c_str(), V1SettingsOperationStore::Source::TripleTap,
        false, false);
    if (started.status != V1SettingsOperationStore::StartStatus::Started) return false;
    if (!settingsOperations_.markPreparing()) {
        (void)settingsOperations_.finish(V1SettingsOperationStore::State::Failed,
                                         V1SettingsOperationStore::Reason::StorageUnavailable);
        return false;
    }
    observedSettingsOperationId_ = 0;
    return true;
}

void SettingsOperationCoordinator::finishSettingsRecapture(
    const V1SettingsOperationStore::Snapshot& operation, uint32_t nowMs) {
    String address;
    if (!effects_.readConnectedAddress(address, effects_.context) || !settingsOperations_.targetMatches(address.c_str())) return;
    V1DetectorSnapshot snapshot = effects_.captureSnapshot(settingsRecaptureIngressBoundary_, effects_.context);
    const bool complete = settingsSnapshotComplete(snapshot);
    const uint32_t settingsSessionGeneration = ble_.sessionGeneration();
    if (settingsFreshObservationGate_.shouldWait(
            complete, nowMs, kFreshObservationWaitMs, settingsSessionGeneration)) return;
    snapshot.captureTimedOut = snapshot.captureTimedOut || !complete;

    const SnapshotPersistResult persisted =
        effects_.persistSnapshot(address, snapshot, true, effects_.context);
    if (persisted != SnapshotPersistResult::Ok) {
        (void)settingsOperations_.finish(V1SettingsOperationStore::State::Failed,
            persisted == SnapshotPersistResult::StoreUnavailable
                ? V1SettingsOperationStore::Reason::SnapshotStoreUnavailable
                : V1SettingsOperationStore::Reason::SnapshotPersistFailed);
        return;
    }

    V1SettingsOperationStore::State terminal = V1SettingsOperationStore::State::Succeeded;
    V1SettingsOperationStore::Reason reason = V1SettingsOperationStore::Reason::None;
    bool preserveInterruptedFactoryTruth = false;
    if (operation.kind == V1SettingsOperationStore::Kind::FactoryReset) {
        if (!V1SettingsOperationStore::hasDurableFactoryResetSend(operation)) {
            // A reset may have reached the detector before power loss, but no
            // durable record proves it. Fresh factory-looking values cannot be
            // promoted into evidence of that destructive send.
            terminal = V1SettingsOperationStore::State::Partial;
            reason = V1SettingsOperationStore::Reason::Interrupted;
            preserveInterruptedFactoryTruth = true;
        } else {
            auto components = operation.components;
            auto& user = components[static_cast<size_t>(V1SettingsOperationStore::Component::UserSettings)];
            const bool userDefaultsMatch = V1SettingsOperationStore::factoryUserDefaultsMatch(
                snapshot.userBytes, snapshot.hasUserBytes);
            if (userDefaultsMatch) {
                user.verified = true;
                user.outcome = V1SettingsOperationStore::ComponentOutcome::Verified;
                user.reason = V1SettingsOperationStore::ComponentReason::None;
            } else if (snapshot.hasUserBytes) {
                user.verified = false;
                user.outcome = V1SettingsOperationStore::ComponentOutcome::Mismatch;
                user.reason = V1SettingsOperationStore::ComponentReason::FactoryUserDefaultsMismatch;
            }
            if (!settingsOperations_.updateComponents(components, 0)) return;
            terminal = V1SettingsOperationStore::State::Partial;
            reason = snapshot.hasUserBytes && !userDefaultsMatch
                ? V1SettingsOperationStore::Reason::FactoryResetDefaultsMismatch
                : V1SettingsOperationStore::Reason::FactoryResetScopeUnverified;
        }
    } else if (operation.reason == V1SettingsOperationStore::Reason::ApplyFailed) {
        terminal = V1SettingsOperationStore::State::Failed;
        reason = operation.reason;
    } else if (operation.reason == V1SettingsOperationStore::Reason::ApplyPartial ||
               operation.reason == V1SettingsOperationStore::Reason::InTheBoxPersistFailed ||
               operation.reason == V1SettingsOperationStore::Reason::Interrupted) {
        terminal = V1SettingsOperationStore::State::Partial;
        reason = operation.reason;
    }
    if (!complete && terminal != V1SettingsOperationStore::State::Failed &&
        reason != V1SettingsOperationStore::Reason::InTheBoxPersistFailed &&
        !preserveInterruptedFactoryTruth) {
        terminal = V1SettingsOperationStore::State::Partial;
        reason = V1SettingsOperationStore::Reason::RecaptureTimedOut;
    }
    (void)settingsOperations_.finish(terminal, reason);
}

void SettingsOperationCoordinator::serviceOperation(uint32_t nowMs) {
    // A durable Running record can describe a reset already sent before reboot.
    // Do not admit any operation until its boot transition has been persisted.
    if (settingsOperationBootRecoveryPending_) {
        if (static_cast<uint32_t>(nowMs - settingsOperationBootRecoveryAttemptMs_) < 1000u) return;
        settingsOperationBootRecoveryAttemptMs_ = nowMs;
        if (!settingsOperations_.beginNormalBoot()) return;
        settingsOperationBootRecoveryPending_ = false;
    }
    if (settingsWrongDetectorDisconnectPending_) {
        settingsWrongDetectorDisconnectPending_ = false;
        ble_.disconnect();
        return;
    }
    const V1SettingsOperationStore::Snapshot initial = settingsOperations_.snapshot();
    if (!initial.available || !initial.valid) return;
    if (initial.operationId != observedSettingsOperationId_) {
        ble_.cancelCommand(V1CommandOwner::FactoryReset);
        observedSettingsOperationId_ = initial.operationId;
        observedSettingsOperationState_ = initial.state;
        settingsOperationStartedMs_ = nowMs;
        settingsOperationNextActionMs_ = 0;
        settingsRecaptureIngressBoundary_ = 0;
        settingsRecaptureStarted_ = false;
        settingsFreshObservationGate_.reset();
        factoryResetSendLatch_.reset();
        factoryResetSummaryPersistedThisBoot_ = false;
        factoryResetSentComponents_ = {};
        settingsWrongDetectorDisconnectPending_ = false;
        settingsReturnRetry_.clear();
    } else if (initial.state != observedSettingsOperationState_) {
        observedSettingsOperationState_ = initial.state;
        settingsOperationNextActionMs_ = 0;
        if (initial.state == V1SettingsOperationStore::State::Recapturing) {
            settingsRecaptureIngressBoundary_ = 0;
            settingsRecaptureStarted_ = false;
            settingsFreshObservationGate_.reset();
        }
    }

    if (settingsOperations_.isTerminal()) {
        ble_.cancelCommand(V1CommandOwner::FactoryReset);
        if (initial.returnToMaintenance && settingsReturnRetry_.due(nowMs)) {
            settingsReturnRetry_.defer(nowMs);
            effects_.requestMaintenanceRestart(effects_.context);
        }
        return;
    }

    constexpr uint32_t kDetectorWaitTimeoutMs = 60000u;
    constexpr uint32_t kOperationTimeoutMs = 90000u;
    if ((initial.state == V1SettingsOperationStore::State::WaitingForDetector ||
         initial.state == V1SettingsOperationStore::State::Preparing) &&
        static_cast<uint32_t>(nowMs - settingsOperationStartedMs_) >= kDetectorWaitTimeoutMs) {
        ble_.cancelCommand(V1CommandOwner::FactoryReset);
        (void)settingsOperations_.finish(V1SettingsOperationStore::State::Failed,
            V1SettingsOperationStore::Reason::DetectorTimeout);
        return;
    }
    if (static_cast<uint32_t>(nowMs - settingsOperationStartedMs_) >= kOperationTimeoutMs) {
        ble_.cancelCommand(V1CommandOwner::FactoryReset);
        bool detectorWritesSent = false;
        if (initial.state == V1SettingsOperationStore::State::Running &&
            initial.kind != V1SettingsOperationStore::Kind::FactoryReset) {
            auto summary = autoPush_.executionSummary();
            if (summary.operationId != 0 &&
                (initial.executorOperationId == 0 || summary.operationId == initial.executorOperationId)) {
                // Stop the executor before publishing a terminal result, even
                // if persisting that result must be retried on a later loop.
                autoPush_.cancelForTimeout();
                summary = autoPush_.executionSummary();
                if (!settingsOperations_.updateComponents(summary.components, summary.operationId)) return;
                for (const auto& component : summary.components) {
                    detectorWritesSent = detectorWritesSent || component.sent;
                }
            }
        }
        const auto terminal = initial.state == V1SettingsOperationStore::State::Recapturing || detectorWritesSent
            ? V1SettingsOperationStore::State::Partial
            : V1SettingsOperationStore::State::Failed;
        (void)settingsOperations_.finish(terminal,
            initial.state == V1SettingsOperationStore::State::Recapturing
                ? V1SettingsOperationStore::Reason::RecaptureTimedOut
                : V1SettingsOperationStore::Reason::DetectorTimeout);
        return;
    }

    String address;
    const bool matchingDetector = effects_.readConnectedAddress(address, effects_.context) &&
        settingsOperations_.targetMatches(address.c_str());
    const uint32_t settingsSessionGeneration = ble_.sessionGeneration();
    if (matchingDetector &&
        settingsFreshObservationGate_.observeSession(settingsSessionGeneration) &&
        (initial.state == V1SettingsOperationStore::State::Preparing ||
         initial.state == V1SettingsOperationStore::State::Recapturing)) {
        // A recapture started for an earlier BLE session cannot establish
        // freshness for this one. Either the new connection follow-up will
        // complete first, or this state starts a fresh explicit recapture.
        settingsRecaptureIngressBoundary_ = 0;
        settingsRecaptureStarted_ = false;
    }
    if ((initial.state == V1SettingsOperationStore::State::WaitingForDetector ||
         initial.state == V1SettingsOperationStore::State::Preparing) &&
        matchingDetector && settingsFreshObservationGate_.completeFor(settingsSessionGeneration)) {
        const uint32_t requiredBoundary = V1SettingsOperationStore::requiredPreApplyIngressBoundary(
            initial.state, settingsRecaptureIngressBoundary_);
        const V1DetectorSnapshot snapshot = effects_.captureSnapshot(requiredBoundary, effects_.context);
        const bool complete = settingsSnapshotComplete(snapshot);
        if (settingsFreshObservationGate_.shouldWait(
                complete, nowMs, kFreshObservationWaitMs, settingsSessionGeneration)) return;
        if (!(effects_.persistSnapshot(address, snapshot, false, effects_.context) == SnapshotPersistResult::Ok)) {
            (void)settingsOperations_.finish(V1SettingsOperationStore::State::Failed,
                V1SettingsOperationStore::Reason::SnapshotStoreUnavailable);
            return;
        }
        if (initial.kind == V1SettingsOperationStore::Kind::FactoryReset) {
            auto components = initial.components;
            for (auto& component : components) {
                component.requested = true;
                component.outcome = V1SettingsOperationStore::ComponentOutcome::Pending;
                component.reason = V1SettingsOperationStore::ComponentReason::None;
            }
            if (!settingsOperations_.updateComponents(components, 0)) return;
            if (!settingsOperations_.markRunning(0)) return;
            attemptFactoryReset(settingsOperations_.snapshot());
        } else {
            queueSettingsApply(initial, snapshot);
        }
        return;
    }

    if (initial.state == V1SettingsOperationStore::State::Preparing && matchingDetector &&
        !settingsRecaptureStarted_) {
        settingsRecaptureIngressBoundary_ = ble_.latestV1NotificationIngressSequence();
        if (ble_.beginSettingsRecapture()) {
            settingsFreshObservationGate_.beginCapture(settingsSessionGeneration);
            settingsRecaptureStarted_ = true;
        }
        return;
    }

    if (initial.state == V1SettingsOperationStore::State::Running) {
        if (initial.kind == V1SettingsOperationStore::Kind::FactoryReset) {
            if (matchingDetector &&
                (settingsOperationNextActionMs_ == 0 ||
                 static_cast<int32_t>(nowMs - settingsOperationNextActionMs_) >= 0)) {
                settingsOperationNextActionMs_ = nowMs + 50u;
                attemptFactoryReset(initial);
            }
            return;
        }
        const AutoPushModule::ExecutionSummary summary = autoPush_.executionSummary();
        if (summary.operationId == 0 ||
            (initial.executorOperationId != 0 && summary.operationId != initial.executorOperationId)) return;
        bool changed = initial.executorOperationId != summary.operationId;
        for (size_t index = 0; index < V1SettingsOperationStore::kComponentCount && !changed; ++index) {
            const auto& left = initial.components[index];
            const auto& right = summary.components[index];
            changed = left.requested != right.requested || left.sent != right.sent ||
                      left.verified != right.verified || left.outcome != right.outcome ||
                      left.reason != right.reason;
        }
        if (changed && !settingsOperations_.updateComponents(summary.components, summary.operationId)) return;
        if (summary.active || summary.result == AutoPushModule::PublicResult::Queued ||
            summary.result == AutoPushModule::PublicResult::InProgress) return;
        V1SettingsOperationStore::Reason reason = V1SettingsOperationStore::Reason::ApplyFailed;
        if (summary.result == AutoPushModule::PublicResult::Succeeded) {
            reason = V1SettingsOperationStore::Reason::None;
        } else if (summary.result == AutoPushModule::PublicResult::Partial) {
            reason = V1SettingsOperationStore::Reason::ApplyPartial;
        }
        if (summary.inTheBoxPersistFailed)
            reason = V1SettingsOperationStore::Reason::InTheBoxPersistFailed;
        (void)settingsOperations_.markRecapturing(reason);
        return;
    }

    if (initial.state == V1SettingsOperationStore::State::Recapturing) {
        if (!matchingDetector) return;
        if (!settingsRecaptureStarted_ &&
            !settingsFreshObservationGate_.completeFor(settingsSessionGeneration)) {
            settingsRecaptureIngressBoundary_ = ble_.latestV1NotificationIngressSequence();
            if (ble_.beginSettingsRecapture()) {
                settingsFreshObservationGate_.beginCapture(settingsSessionGeneration);
                settingsRecaptureStarted_ = true;
            }
            return;
        }
        if (settingsFreshObservationGate_.completeFor(settingsSessionGeneration)) {
            finishSettingsRecapture(initial, nowMs);
        }
    }
}

void SettingsOperationCoordinator::queueSettingsApply(const V1SettingsOperationStore::Snapshot& operation,
                                      const V1DetectorSnapshot& preApply) {
    autoPush_.setPreApplySnapshot(preApply);
    if (!settingsOperations_.markRunning(0)) return;
    AutoPushModule::QueueResult queued = AutoPushModule::QueueResult::STAGING_UNAVAILABLE;
    if (operation.kind == V1SettingsOperationStore::Kind::ApplySlot) {
        const bool activate = operation.source == V1SettingsOperationStore::Source::TripleTap;
        queued = autoPush_.queueSlotPush(operation.slot, activate, false);
    } else {
        AutoPushModule::PushNowRequest request;
        request.slotIndex = effects_.readActiveSlot(effects_.context);
        request.hasProfileOverride = true;
        request.profileName = operation.profileName;
        queued = autoPush_.queuePushNow(request);
    }
    if (queued != AutoPushModule::QueueResult::QUEUED) {
        (void)settingsOperations_.finish(V1SettingsOperationStore::State::Failed,
                                         AutoPushModule::durableReasonForQueueResult(queued));
        return;
    }
    if (operation.source == V1SettingsOperationStore::Source::TripleTap) {
        effects_.presentQueuedProfile(operation.slot, effects_.context);
    }
    const AutoPushModule::ExecutionSummary summary = autoPush_.executionSummary();
    (void)settingsOperations_.updateComponents(summary.components, summary.operationId);
}

void SettingsOperationCoordinator::attemptFactoryReset(const V1SettingsOperationStore::Snapshot& operation) {
    if (operation.kind != V1SettingsOperationStore::Kind::FactoryReset || !ble_.isConnected()) return;
    if (factoryResetSendLatch_.sent()) {
        if (!factoryResetSummaryPersistedThisBoot_) {
            if (!settingsOperations_.updateComponents(factoryResetSentComponents_, 0)) return;
            factoryResetSummaryPersistedThisBoot_ = true;
        }
        (void)settingsOperations_.markRecapturing(
            V1SettingsOperationStore::Reason::FactoryResetScopeUnverified);
        return;
    }
    auto components = operation.components;
    V1BLEClient::CommandScope commandScope(ble_, V1CommandOwner::FactoryReset);
    const uint32_t session = ble_.sessionGeneration();
    // A pending request may already have reset the detector before its ACK
    // disappeared. Only poll that exact admitted job; never recreate it after
    // reconnect or after an error whose durable failure write needs retrying.
    const bool admittedJobLost = factoryResetSendLatch_.admitted() &&
        (factoryResetSendLatch_.admittedSession() != session ||
         !ble_.commandInFlight(V1CommandOwner::FactoryReset));
    const SendResult result = admittedJobLost ? SendResult::FAILED : ble_.factoryResetDetector();
    if (result == SendResult::IN_FLIGHT) {
        factoryResetSendLatch_.noteAdmitted(session);
        return;
    }
    if (result == SendResult::NOT_YET) return;
    if (result != SendResult::SENT) {
        const bool unacknowledged = factoryResetSendLatch_.admitted();
        for (size_t index = 0; index < V1SettingsOperationStore::kComponentCount; ++index) {
            components[index].outcome = index == static_cast<size_t>(V1SettingsOperationStore::Component::FactoryReset)
                ? V1SettingsOperationStore::ComponentOutcome::WriteFailed
                : V1SettingsOperationStore::ComponentOutcome::Blocked;
            components[index].reason = unacknowledged
                ? V1SettingsOperationStore::ComponentReason::FactoryDefaultUnverified
                : V1SettingsOperationStore::ComponentReason::FactoryResetWriteFailed;
        }
        (void)settingsOperations_.updateComponents(components, 0);
        (void)settingsOperations_.finish(V1SettingsOperationStore::State::Failed,
            unacknowledged ? V1SettingsOperationStore::Reason::FactoryResetScopeUnverified
                          : V1SettingsOperationStore::Reason::FactoryResetSendFailed);
        return;
    }
    for (size_t index = 0; index < V1SettingsOperationStore::kComponentCount; ++index) {
        components[index].sent = true;
        components[index].verified = false;
        components[index].outcome = V1SettingsOperationStore::ComponentOutcome::Sent;
        components[index].reason = index == static_cast<size_t>(V1SettingsOperationStore::Component::FactoryReset)
            ? V1SettingsOperationStore::ComponentReason::None
            : V1SettingsOperationStore::ComponentReason::FactoryDefaultUnverified;
    }
    // Latch before either persistence write. A returned SENT must never lead
    // to a second destructive frame merely because NVS later failed.
    factoryResetSendLatch_.noteSent();
    factoryResetSentComponents_ = components;
    if (!settingsOperations_.updateComponents(components, 0)) return;
    factoryResetSummaryPersistedThisBoot_ = true;
    (void)settingsOperations_.markRecapturing(
        V1SettingsOperationStore::Reason::FactoryResetScopeUnverified);
}

SettingsOperationCoordinator::StableAdmission SettingsOperationCoordinator::onStableConnection() {
    // An unreadable durable job cannot authorize a competing automatic Apply.
    // Keep the connection for alerts; there is no known target to disconnect.
    if (!settingsOperations_.snapshot().valid) return StableAdmission::OwnedOrBlocked;
    if (!settingsOperations_.requiresExclusiveDetectorAdmission()) return StableAdmission::AllowOrdinaryAutoPush;
    String address;
    if (!effects_.readConnectedAddress(address, effects_.context) || !settingsOperations_.targetMatches(address.c_str())) {
        // A target-bound job owns detector mutation admission until it finishes
        // and any durable return-to-maintenance intent has been acknowledged.
        // Ordinary Auto-Push must not mutate another V1 in that interval.
        settingsWrongDetectorDisconnectPending_ = ble_.isConnected();
        return StableAdmission::OwnedOrBlocked;
    }
    const V1SettingsOperationStore::Snapshot operation = settingsOperations_.snapshot();
    if (operation.state == V1SettingsOperationStore::State::Recapturing) {
        settingsFreshObservationGate_.noteFollowupComplete(
            static_cast<uint32_t>(millis()), ble_.sessionGeneration());
        return StableAdmission::OwnedOrBlocked;
    }
    if (operation.state != V1SettingsOperationStore::State::WaitingForDetector &&
        operation.state != V1SettingsOperationStore::State::Preparing) return StableAdmission::OwnedOrBlocked;
    settingsFreshObservationGate_.noteFollowupComplete(
        static_cast<uint32_t>(millis()), ble_.sessionGeneration());
    return StableAdmission::OwnedOrBlocked;
}

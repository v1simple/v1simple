#if defined(V1SIMPLE_HIL_FAULT_CONTROL)

#include "ble_bsc05_hil_fault_module.h"

#include <cstdio>

#if defined(ARDUINO_ARCH_ESP32)
#include <Arduino.h>
#endif

BleBsc05HilFaultModule::BleBsc05HilFaultModule(HilFaultRuntimeOwner& owner, const BleBsc05HilRuntime& runtime) noexcept
    : owner_(owner), runtime_(runtime) {}

void BleBsc05HilFaultModule::configure(const BleBsc05HilRuntime& runtime) noexcept {
    runtime_ = runtime;
}

bool BleBsc05HilFaultModule::identitiesEqual(const HilArmedFaultIdentity& armed,
                                             const ActiveIdentity& active) noexcept {
    return armed.caseId == HilCaseId::Bsc05 && armed.faultId == HilFaultId::V1NotificationDelayOnce &&
           armed.armSequence == active.armSequence && armed.sessionHash == active.sessionHash;
}

const char* BleBsc05HilFaultModule::characteristicClass(const uint16_t characteristicUuid) noexcept {
    if (characteristicUuid == 0xB2CE) {
        return "display";
    }
    if (characteristicUuid == 0xB4E0) {
        return "long";
    }
    return "other";
}

void BleBsc05HilFaultModule::refreshArm() noexcept {
    const Stage stage = static_cast<Stage>(stage_.load());
    if (stage == Stage::Capturing || stage == Stage::Captured) {
        return;
    }

    HilArmedFaultIdentity armed{};
    if (!owner_.armedIdentity(HilFaultId::V1NotificationDelayOnce, armed) || armed.caseId != HilCaseId::Bsc05) {
        if (stage != Stage::Idle) {
            (void)delayGate_.discard();
            identity_ = ActiveIdentity{};
            releasedNotification_ = BleDelayedNotification{};
            readyEventPending_ = false;
            firedEventPending_ = false;
            releaseAttempted_ = false;
            wrongGenerationRejected_ = false;
            stage_.store(static_cast<uint32_t>(Stage::Idle));
        }
        return;
    }

    if (stage == Stage::Armed && identitiesEqual(armed, identity_)) {
        return;
    }
    if (stage == Stage::Completed && identitiesEqual(armed, identity_)) {
        return;
    }

    (void)delayGate_.discard();
    identity_ = ActiveIdentity{};
    identity_.sessionHash = armed.sessionHash;
    identity_.armSequence = armed.armSequence;
    releasedNotification_ = BleDelayedNotification{};
    readyEventPending_ = false;
    firedEventPending_ = false;
    releaseAttempted_ = false;
    wrongGenerationRejected_ = false;
    stage_.store(static_cast<uint32_t>(Stage::Armed));
}

bool BleBsc05HilFaultModule::routeNotification(const uint8_t* data, const size_t length,
                                               const uint16_t characteristicUuid, const uint32_t sessionGeneration,
                                               const uint32_t nowMs) noexcept {
    uint32_t expected = static_cast<uint32_t>(Stage::Armed);
    if (!stage_.compareExchange(expected, static_cast<uint32_t>(Stage::Capturing))) {
        return true;
    }

    const BleNotificationDelayCaptureResult capture =
        delayGate_.capture(data, length, characteristicUuid, sessionGeneration);
    if (capture != BleNotificationDelayCaptureResult::Captured) {
        stage_.store(static_cast<uint32_t>(capture == BleNotificationDelayCaptureResult::Occupied ? Stage::Completed
                                                                                                  : Stage::Armed));
        return true;
    }

    identity_.oldGeneration = sessionGeneration;
    releasedNotification_.characteristicUuid = characteristicUuid;
    releasedNotification_.oldGeneration = sessionGeneration;
    const HilReadyResult ready = owner_.controller().publishReady(
        HilCaseId::Bsc05, HilFaultId::V1NotificationDelayOnce, identity_.sessionHash, identity_.armSequence,
        identity_.oldGeneration, kNotificationAdmissionPhase, nowMs, kAutomaticReleaseMs);
    if (ready.result != HilFaultResult::Ok) {
        (void)delayGate_.discard();
        stage_.store(static_cast<uint32_t>(Stage::Completed));
        return true;
    }
    identity_.readySequence = ready.readySequence;

    const HilFaultResult fired = owner_.controller().fire(
        HilCaseId::Bsc05, HilFaultId::V1NotificationDelayOnce, identity_.sessionHash, identity_.armSequence,
        identity_.readySequence, identity_.oldGeneration, kNotificationAdmissionPhase, nowMs);
    if (fired != HilFaultResult::Ok) {
        (void)delayGate_.discard();
        stage_.store(static_cast<uint32_t>(Stage::Completed));
        return true;
    }

    readyEventPending_ = true;
    firedEventPending_ = true;
    stage_.store(static_cast<uint32_t>(Stage::Captured));
    return false;
}

void BleBsc05HilFaultModule::recordSessionClosed(const uint32_t generation, const uint32_t nowMs) noexcept {
    delayGate_.recordSessionClosed(generation, nowMs);
}

void BleBsc05HilFaultModule::recordSessionOpened(const uint32_t generation, const uint32_t nowMs) noexcept {
    delayGate_.recordSessionOpened(generation, nowMs);
}

void BleBsc05HilFaultModule::emitEvent(const char* event, const char* reason) noexcept {
    if (runtime_.writeEvidence == nullptr || event == nullptr || reason == nullptr) {
        return;
    }
    const uint16_t characteristicUuid =
        releasedNotification_.characteristicUuid == 0 ? 0 : releasedNotification_.characteristicUuid;
    char response[640]{};
    const int written = std::snprintf(
        response, sizeof(response),
        "{\"hil_event\":\"%s\",\"reason\":\"%s\",\"case_id\":\"BSC-05\","
        "\"fault_id\":\"v1-notification-delay-once\",\"arm_sequence\":%lu,"
        "\"ready_sequence\":%lu,\"generation\":%lu,\"phase\":%u,"
        "\"old_generation\":%lu,\"new_generation\":%lu,"
        "\"characteristic_class\":\"%s\",\"old_session_closed_at_ms\":%lu,"
        "\"new_session_opened_at_ms\":%lu,\"wrong_generation_rejected\":%s}\n",
        event, reason, static_cast<unsigned long>(identity_.armSequence),
        static_cast<unsigned long>(identity_.readySequence), static_cast<unsigned long>(identity_.oldGeneration),
        static_cast<unsigned>(kNotificationAdmissionPhase), static_cast<unsigned long>(identity_.oldGeneration),
        static_cast<unsigned long>(releasedNotification_.newGeneration), characteristicClass(characteristicUuid),
        static_cast<unsigned long>(releasedNotification_.oldSessionClosedAtMs),
        static_cast<unsigned long>(releasedNotification_.newSessionOpenedAtMs),
        wrongGenerationRejected_ ? "true" : "false");
    if (written > 0 && static_cast<size_t>(written) < sizeof(response)) {
        runtime_.writeEvidence(response, runtime_.evidenceContext);
    }
}

void BleBsc05HilFaultModule::emitCapturedEvents() noexcept {
    if (releasedNotification_.oldGeneration == 0) {
        releasedNotification_.oldGeneration = identity_.oldGeneration;
    }
    if (readyEventPending_) {
        emitEvent("ready", "notification_copied_without_callback_pointer");
        readyEventPending_ = false;
    }
    if (firedEventPending_) {
        emitEvent("fired", "old_generation_notification_delayed");
        firedEventPending_ = false;
    }
}

void BleBsc05HilFaultModule::completeExpired(const char* reason) noexcept {
    emitCapturedEvents();
    (void)delayGate_.discard();
    emitEvent("expired", reason);
    stage_.store(static_cast<uint32_t>(Stage::Completed));
}

void BleBsc05HilFaultModule::service(const uint32_t nowMs) noexcept {
    owner_.service(nowMs);
    refreshArm();
    if (static_cast<Stage>(stage_.load()) != Stage::Captured) {
        return;
    }

    emitCapturedEvents();
    const HilFaultSnapshot controller = controllerSnapshot();
    if (controller.state == HilFaultState::Expired) {
        completeExpired("automatic_timeout_discarded_copy");
        return;
    }
    if (controller.state == HilFaultState::Disarmed) {
        completeExpired("session_end_discarded_copy");
        refreshArm();
        return;
    }

    BleDelayedNotification notification{};
    if (!delayGate_.claimEligible(notification)) {
        return;
    }
    releasedNotification_ = notification;
    releaseAttempted_ = true;
    if (runtime_.forwardNotification == nullptr) {
        emitEvent("blocked", "notification_queue_adapter_unavailable");
        (void)delayGate_.discard();
        stage_.store(static_cast<uint32_t>(Stage::Completed));
        return;
    }
    const bool accepted =
        runtime_.forwardNotification(notification.data.data(), notification.length, notification.characteristicUuid,
                                     notification.oldGeneration, runtime_.forwardContext);
    wrongGenerationRejected_ = !accepted;
    if (!wrongGenerationRejected_) {
        emitEvent("blocked", "old_generation_was_unexpectedly_accepted");
        (void)delayGate_.discard();
        stage_.store(static_cast<uint32_t>(Stage::Completed));
        return;
    }

    const HilFaultResult observed = owner_.controller().observeCompetingOperation(
        HilCaseId::Bsc05, HilFaultId::V1NotificationDelayOnce, identity_.sessionHash, identity_.armSequence,
        identity_.readySequence, identity_.oldGeneration, kNotificationAdmissionPhase, nowMs);
    const HilFaultResult released =
        observed == HilFaultResult::Ok
            ? owner_.controller().release(HilCaseId::Bsc05, HilFaultId::V1NotificationDelayOnce, identity_.sessionHash,
                                          identity_.armSequence, identity_.readySequence, identity_.oldGeneration,
                                          kNotificationAdmissionPhase, nowMs)
            : observed;
    (void)delayGate_.discard();
    if (released == HilFaultResult::Ok) {
        emitEvent("released", "new_session_rejected_old_generation_copy");
    } else {
        emitEvent("blocked", "controller_refused_release");
    }
    stage_.store(static_cast<uint32_t>(Stage::Completed));
}

HilFaultSnapshot BleBsc05HilFaultModule::controllerSnapshot() const noexcept {
    return owner_.controller().snapshot(HilFaultId::V1NotificationDelayOnce);
}

BleBsc05HilSnapshot BleBsc05HilFaultModule::snapshot() const noexcept {
    const Stage stage = static_cast<Stage>(stage_.load());
    return {stage == Stage::Armed,
            stage == Stage::Captured,
            releasedNotification_.oldSessionClosedAtMs != 0,
            releasedNotification_.newGeneration != 0,
            releaseAttempted_,
            wrongGenerationRejected_,
            identity_.armSequence,
            identity_.readySequence,
            identity_.oldGeneration,
            releasedNotification_.newGeneration};
}

BleBsc05HilFaultModule& bleBsc05HilFaultModule() noexcept {
    static BleBsc05HilFaultModule module(hilFaultRuntimeOwner());
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

void configureBleBsc05HilDeviceRuntime(const BleBsc05HilRuntime::ForwardNotification forwardNotification,
                                       void* forwardContext) noexcept {
    BleBsc05HilRuntime runtime{};
    runtime.forwardNotification = forwardNotification;
    runtime.forwardContext = forwardContext;
#if defined(ARDUINO_ARCH_ESP32)
    runtime.writeEvidence = deviceWriteEvidence;
#endif
    bleBsc05HilFaultModule().configure(runtime);
}

#endif // V1SIMPLE_HIL_FAULT_CONTROL

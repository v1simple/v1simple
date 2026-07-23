#if defined(V1SIMPLE_HIL_FAULT_CONTROL)

#include "storage_bsc14_hil_fault_module.h"

#include <cstdio>

#if defined(ARDUINO_ARCH_ESP32)
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#endif

StorageBsc14HilFaultModule::StorageBsc14HilFaultModule(HilFaultRuntimeOwner& owner,
                                                       const StorageBsc14HilRuntime& runtime) noexcept
    : owner_(owner), runtime_(runtime), lifecycle_(runtime.mutex) {}

void StorageBsc14HilFaultModule::configure(const StorageBsc14HilRuntime& runtime) noexcept {
    runtime_ = runtime;
    lifecycle_.configure(runtime.mutex);
}

bool StorageBsc14HilFaultModule::identitiesEqual(const HilArmedFaultIdentity& armed,
                                                 const ActiveIdentity& active) noexcept {
    return armed.caseId == HilCaseId::Bsc14 && armed.faultId == HilFaultId::SdMutexHold &&
           armed.armSequence == active.armSequence && armed.sessionHash == active.sessionHash;
}

const char* StorageBsc14HilFaultModule::gestureName(const StorageBsc14Gesture gesture) noexcept {
    switch (gesture) {
    case StorageBsc14Gesture::SliderExit:
        return "slider_exit";
    case StorageBsc14Gesture::StealthDoublePress:
        return "stealth_double_press";
    case StorageBsc14Gesture::ProfileTripleTap:
        return "profile_triple_tap";
    case StorageBsc14Gesture::Invalid:
        return "none";
    }
    return "none";
}

uint32_t StorageBsc14HilFaultModule::nextGeneration() noexcept {
    ++generationCounter_;
    if (generationCounter_ == 0u) {
        ++generationCounter_;
    }
    return generationCounter_;
}

void StorageBsc14HilFaultModule::emitEvent(const char* const event, const char* const reason,
                                           const StorageBsc14Gesture gesture, const uint32_t previousRevision,
                                           const uint32_t persistedRevision) noexcept {
    if (runtime_.writeEvidence == nullptr || event == nullptr || reason == nullptr) {
        return;
    }
    char response[512]{};
    const int written = std::snprintf(
        response, sizeof(response),
        "{\"hil_event\":\"%s\",\"reason\":\"%s\",\"case_id\":\"BSC-14\","
        "\"fault_id\":\"sd-mutex-hold\",\"arm_sequence\":%lu,\"ready_sequence\":%lu,"
        "\"generation\":%lu,\"phase\":%u,\"gesture_kind\":\"%s\","
        "\"previous_revision\":%lu,\"nvs_revision\":%lu,\"gesture_mask\":%lu}\n",
        event, reason, static_cast<unsigned long>(identity_.armSequence),
        static_cast<unsigned long>(identity_.readySequence), static_cast<unsigned long>(identity_.generation),
        static_cast<unsigned>(kSdMutexHoldPhase), gestureName(gesture), static_cast<unsigned long>(previousRevision),
        static_cast<unsigned long>(persistedRevision), static_cast<unsigned long>(gestureMask_.load()));
    if (written > 0 && static_cast<size_t>(written) < sizeof(response)) {
        runtime_.writeEvidence(response, runtime_.evidenceContext);
    }
}

void StorageBsc14HilFaultModule::refreshArm() noexcept {
    const Stage stage = static_cast<Stage>(stage_.load());
    if (stage == Stage::Acquiring || stage == Stage::Holding) {
        return;
    }

    HilArmedFaultIdentity armed{};
    if (!owner_.armedIdentity(HilFaultId::SdMutexHold, armed) || armed.caseId != HilCaseId::Bsc14) {
        if (stage != Stage::Idle) {
            identity_ = ActiveIdentity{};
            gestureMask_.store(0);
            completionRecorded_.store(0);
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
    identity_.generation = nextGeneration();
    gestureMask_.store(0);
    completionRecorded_.store(0);
    lifecycle_.configure(runtime_.mutex);
    stage_.store(static_cast<uint32_t>(Stage::Armed));
}

void StorageBsc14HilFaultModule::service(const uint32_t nowMs) noexcept {
    owner_.service(nowMs);
    refreshArm();
}

void StorageBsc14HilFaultModule::completeHold(const uint32_t nowMs, const char* const reason) noexcept {
    const HilFaultState state = owner_.controller().snapshot(HilFaultId::SdMutexHold).state;
    completionRecorded_.store(1);
    stage_.store(static_cast<uint32_t>(Stage::Completed));
    emitEvent(state == HilFaultState::Released ? "released" : "expired", reason);
    (void)nowMs;
}

bool StorageBsc14HilFaultModule::holderTaskTick(const uint32_t nowMs) noexcept {
    Stage stage = static_cast<Stage>(stage_.load());
    if (stage == Stage::Armed) {
        uint32_t expected = static_cast<uint32_t>(Stage::Armed);
        if (stage_.compareExchange(expected, static_cast<uint32_t>(Stage::Acquiring))) {
            if (!lifecycle_.begin(nowMs, kAcquisitionWindowMs)) {
                completionRecorded_.store(1);
                stage_.store(static_cast<uint32_t>(Stage::Completed));
                emitEvent("expired", "invalid_mutex_runtime");
                return true;
            }
        }
        stage = static_cast<Stage>(stage_.load());
    }

    if (stage == Stage::Acquiring) {
        const HilFaultSnapshot controller = owner_.controller().snapshot(HilFaultId::SdMutexHold);
        const bool armStillActive =
            controller.state == HilFaultState::Armed && controller.armSequence == identity_.armSequence;
        const SdMutexHoldStep step = lifecycle_.step(nowMs, armStillActive);
        if (step == SdMutexHoldStep::Waiting) {
            return true;
        }
        if (step != SdMutexHoldStep::Acquired) {
            completionRecorded_.store(1);
            stage_.store(static_cast<uint32_t>(Stage::Completed));
            emitEvent("expired", "mutex_acquisition_not_completed");
            return true;
        }

        const HilReadyResult ready = owner_.controller().publishReady(
            HilCaseId::Bsc14, HilFaultId::SdMutexHold, identity_.sessionHash, identity_.armSequence,
            identity_.generation, kSdMutexHoldPhase, nowMs, kAutomaticReleaseMs);
        if (ready.result != HilFaultResult::Ok) {
            (void)lifecycle_.step(nowMs, false);
            completionRecorded_.store(1);
            stage_.store(static_cast<uint32_t>(Stage::Completed));
            emitEvent("expired", "ready_publication_failed");
            return true;
        }
        identity_.readySequence = ready.readySequence;
        emitEvent("ready", "real_sd_mutex_owned");
        const HilFaultResult fired = owner_.controller().fire(
            HilCaseId::Bsc14, HilFaultId::SdMutexHold, identity_.sessionHash, identity_.armSequence,
            identity_.readySequence, identity_.generation, kSdMutexHoldPhase, nowMs);
        if (fired != HilFaultResult::Ok) {
            (void)lifecycle_.step(nowMs, false);
            completionRecorded_.store(1);
            stage_.store(static_cast<uint32_t>(Stage::Completed));
            emitEvent("expired", "hold_fire_failed");
            return true;
        }
        stage_.store(static_cast<uint32_t>(Stage::Holding));
        emitEvent("fired", "bounded_sd_mutex_hold_active");
        return true;
    }

    if (stage == Stage::Holding) {
        owner_.controller().service(nowMs);
        const bool continueHolding = owner_.controller().shouldPause(
            HilCaseId::Bsc14, HilFaultId::SdMutexHold, identity_.sessionHash, identity_.armSequence,
            identity_.readySequence, identity_.generation, kSdMutexHoldPhase, nowMs);
        if (lifecycle_.step(nowMs, continueHolding) == SdMutexHoldStep::Released) {
            completeHold(nowMs, continueHolding ? "holder_released" : "bounded_hold_finished");
        }
    }
    return true;
}

void StorageBsc14HilFaultModule::recordGesturePersisted(const StorageBsc14Gesture gesture,
                                                        const uint32_t previousRevision,
                                                        const uint32_t persistedRevision,
                                                        const uint32_t nowMs) noexcept {
    if (gesture >= StorageBsc14Gesture::Invalid || previousRevision == persistedRevision || persistedRevision == 0u ||
        static_cast<Stage>(stage_.load()) != Stage::Holding) {
        return;
    }
    const uint32_t bit = 1u << static_cast<uint8_t>(gesture);
    uint32_t observed = gestureMask_.load();
    while ((observed & bit) == 0u && !gestureMask_.compareExchange(observed, observed | bit)) {
    }
    if ((observed & bit) != 0u) {
        return;
    }
    if ((observed | bit) == bit) {
        (void)owner_.controller().observeCompetingOperation(
            HilCaseId::Bsc14, HilFaultId::SdMutexHold, identity_.sessionHash, identity_.armSequence,
            identity_.readySequence, identity_.generation, kSdMutexHoldPhase, nowMs);
    }
    emitEvent("gesture_persisted", "nvs_commit_completed_while_sd_blocked", gesture, previousRevision,
              persistedRevision);
}

HilFaultSnapshot StorageBsc14HilFaultModule::controllerSnapshot() const noexcept {
    return owner_.controller().snapshot(HilFaultId::SdMutexHold);
}

StorageBsc14HilSnapshot StorageBsc14HilFaultModule::snapshot() const noexcept {
    StorageBsc14HilSnapshot result{};
    const Stage stage = static_cast<Stage>(stage_.load());
    result.armed = stage == Stage::Armed;
    result.acquiring = stage == Stage::Acquiring;
    result.holding = stage == Stage::Holding;
    result.completionRecorded = completionRecorded_.load() != 0u;
    result.mutexOwned = lifecycle_.ownsMutex();
    result.armSequence = identity_.armSequence;
    result.readySequence = identity_.readySequence;
    result.generation = identity_.generation;
    result.gestureMask = gestureMask_.load();
    return result;
}

StorageBsc14HilFaultModule& storageBsc14HilFaultModule() noexcept {
    static StorageBsc14HilFaultModule module(hilFaultRuntimeOwner());
    return module;
}

#if defined(ARDUINO_ARCH_ESP32)
namespace {
bool deviceTryAcquire(void* const context) noexcept {
    return context != nullptr && xSemaphoreTake(static_cast<SemaphoreHandle_t>(context), 0) == pdTRUE;
}

void deviceRelease(void* const context) noexcept {
    if (context != nullptr) {
        xSemaphoreGive(static_cast<SemaphoreHandle_t>(context));
    }
}

void deviceWriteEvidence(const char* const line, void*) noexcept {
    if (line != nullptr) {
        Serial.print(line);
    }
}

void deviceHolderTask(void*) {
    while (storageBsc14HilFaultModule().holderTaskTick(millis())) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    vTaskDelete(nullptr);
}
} // namespace
#endif

bool configureStorageBsc14HilDeviceRuntime(void* const sdMutex) noexcept {
#if defined(ARDUINO_ARCH_ESP32)
    StorageBsc14HilRuntime runtime{};
    runtime.mutex = {deviceTryAcquire, deviceRelease, sdMutex};
    runtime.writeEvidence = deviceWriteEvidence;
    storageBsc14HilFaultModule().configure(runtime);
    static bool holderTaskStarted = false;
    if (!holderTaskStarted) {
        holderTaskStarted =
            xTaskCreatePinnedToCore(deviceHolderTask, "bsc14_sd_hold", 3072, nullptr, 1, nullptr, 0) == pdPASS;
    }
    return holderTaskStarted;
#else
    (void)sdMutex;
    return false;
#endif
}

#endif // V1SIMPLE_HIL_FAULT_CONTROL

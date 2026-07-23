#if defined(V1SIMPLE_HIL_FAULT_CONTROL)

#include "connection_bsc04_hil_fault_module.h"

#include <cstdio>

#if defined(ARDUINO_ARCH_ESP32)
#include <Arduino.h>
#endif

namespace {
const char* cycleStateName(const uint8_t stateCode) noexcept {
    switch (stateCode) {
    case 0:
        return "SCAN_V1";
    case 1:
        return "V1_SETTLING";
    case 2:
        return "OBD_SCAN";
    case 3:
        return "OBD_CONNECT";
    case 4:
        return "OBD_SETTLED";
    case 5:
        return "PROXY_OPEN";
    case 6:
        return "WIFI_OPEN";
    case 7:
        return "STEADY";
    case 8:
        return "TEARDOWN";
    }
    return "INVALID";
}
} // namespace

ConnectionBsc04HilFaultModule::ConnectionBsc04HilFaultModule(HilFaultRuntimeOwner& owner,
                                                             const ConnectionBsc04HilRuntime& runtime) noexcept
    : owner_(owner), runtime_(runtime) {}

void ConnectionBsc04HilFaultModule::configure(const ConnectionBsc04HilRuntime& runtime) noexcept {
    runtime_ = runtime;
}

uint32_t ConnectionBsc04HilFaultModule::nextGeneration() noexcept {
    ++generationCounter_;
    if (generationCounter_ == 0) {
        ++generationCounter_;
    }
    return generationCounter_;
}

void ConnectionBsc04HilFaultModule::emitEvent(const char* event, const char* reason, const bool forwarded) noexcept {
    if (runtime_.writeEvidence == nullptr || event == nullptr || reason == nullptr) {
        return;
    }
    char response[512]{};
    const int written = std::snprintf(
        response, sizeof(response),
        "{\"hil_event\":\"%s\",\"reason\":\"%s\",\"case_id\":\"BSC-04\","
        "\"fault_id\":\"v1-verify-push-suppress-once\",\"arm_sequence\":%lu,"
        "\"ready_sequence\":%lu,\"generation\":%lu,\"phase\":%u,"
        "\"coordinator_state\":\"%s\",\"v1_connected\":%s,"
        "\"raw_verify_push_edge\":true,\"forwarded_verify_push_edge\":%s}\n",
        event, reason, static_cast<unsigned long>(identity_.armSequence),
        static_cast<unsigned long>(identity_.readySequence), static_cast<unsigned long>(identity_.generation),
        static_cast<unsigned>(kVerifyPushAdmissionPhase), cycleStateName(admission_.coordinatorStateCode),
        admission_.v1GattConnected ? "true" : "false", forwarded ? "true" : "false");
    if (written > 0 && static_cast<size_t>(written) < sizeof(response)) {
        runtime_.writeEvidence(response, runtime_.context);
    }
}

bool ConnectionBsc04HilFaultModule::routeVerifyPushMatchEdge(const ConnectionBsc04VerifyPushAdmission& admission,
                                                             const uint32_t nowMs) noexcept {
    owner_.service(nowMs);
    if (!admission.verifyPushMatchEdge) {
        return false;
    }
    if (admissionAttempted_ || !admission.v1GattConnected || admission.coordinatorStateCode != kV1SettlingStateCode) {
        return true;
    }

    HilArmedFaultIdentity armed{};
    if (!owner_.armedIdentity(HilFaultId::V1VerifyPushSuppressOnce, armed) || armed.caseId != HilCaseId::Bsc04) {
        return true;
    }

    admissionAttempted_ = true;
    admission_ = admission;
    identity_.sessionHash = armed.sessionHash;
    identity_.armSequence = armed.armSequence;
    identity_.generation = nextGeneration();

    const HilReadyResult ready = owner_.controller().publishReady(
        HilCaseId::Bsc04, HilFaultId::V1VerifyPushSuppressOnce, identity_.sessionHash, identity_.armSequence,
        identity_.generation, kVerifyPushAdmissionPhase, nowMs, kAutomaticReleaseMs);
    if (ready.result != HilFaultResult::Ok) {
        return true;
    }
    identity_.readySequence = ready.readySequence;
    emitEvent("ready", "v1_settling_verify_push_admission", true);

    const HilFaultResult fired = owner_.controller().fire(
        HilCaseId::Bsc04, HilFaultId::V1VerifyPushSuppressOnce, identity_.sessionHash, identity_.armSequence,
        identity_.readySequence, identity_.generation, kVerifyPushAdmissionPhase, nowMs);
    if (fired != HilFaultResult::Ok) {
        return true;
    }
    emitEvent("fired", "verify_push_edge_suppressed", false);

    const HilFaultResult observed = owner_.controller().observeCompetingOperation(
        HilCaseId::Bsc04, HilFaultId::V1VerifyPushSuppressOnce, identity_.sessionHash, identity_.armSequence,
        identity_.readySequence, identity_.generation, kVerifyPushAdmissionPhase, nowMs);
    if (observed != HilFaultResult::Ok) {
        return true;
    }
    const HilFaultResult released = owner_.controller().release(
        HilCaseId::Bsc04, HilFaultId::V1VerifyPushSuppressOnce, identity_.sessionHash, identity_.armSequence,
        identity_.readySequence, identity_.generation, kVerifyPushAdmissionPhase, nowMs);
    if (released != HilFaultResult::Ok) {
        return true;
    }
    suppressionApplied_ = true;
    emitEvent("released", "suppressed_edge_committed", false);
    return false;
}

void ConnectionBsc04HilFaultModule::service(const uint32_t nowMs) noexcept {
    owner_.service(nowMs);
}

HilFaultSnapshot ConnectionBsc04HilFaultModule::controllerSnapshot() const noexcept {
    return owner_.controller().snapshot(HilFaultId::V1VerifyPushSuppressOnce);
}

ConnectionBsc04HilSnapshot ConnectionBsc04HilFaultModule::snapshot() const noexcept {
    ConnectionBsc04HilSnapshot result{};
    result.admissionAttempted = admissionAttempted_;
    result.suppressionApplied = suppressionApplied_;
    result.v1GattConnected = admission_.v1GattConnected;
    result.coordinatorStateCode = admission_.coordinatorStateCode;
    result.armSequence = identity_.armSequence;
    result.readySequence = identity_.readySequence;
    result.generation = identity_.generation;
    return result;
}

ConnectionBsc04HilFaultModule& connectionBsc04HilFaultModule() noexcept {
    static ConnectionBsc04HilFaultModule module(hilFaultRuntimeOwner());
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

void configureConnectionBsc04HilDeviceRuntime() noexcept {
#if defined(ARDUINO_ARCH_ESP32)
    ConnectionBsc04HilRuntime runtime{};
    runtime.writeEvidence = deviceWriteEvidence;
    connectionBsc04HilFaultModule().configure(runtime);
#endif
}

#endif // V1SIMPLE_HIL_FAULT_CONTROL

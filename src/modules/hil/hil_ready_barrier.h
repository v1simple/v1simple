#pragma once

#if defined(V1SIMPLE_HIL_FAULT_CONTROL)

#include "hil_fault_controller.h"

struct HilReadyPublication {
    HilCaseId caseId = HilCaseId::Invalid;
    HilFaultId faultId = HilFaultId::Invalid;
    HilSessionTokenHash sessionHash{};
    uint32_t armSequence = 0;
    uint32_t activeGeneration = 0;
    uint16_t exactPhase = 0;
    uint32_t nowMs = 0;
    uint32_t automaticReleaseAfterMs = 0;
};

struct HilReadyIdentity {
    HilCaseId caseId = HilCaseId::Invalid;
    HilFaultId faultId = HilFaultId::Invalid;
    HilSessionTokenHash sessionHash{};
    uint32_t armSequence = 0;
    uint32_t readySequence = 0;
    uint32_t activeGeneration = 0;
    uint16_t exactPhase = 0;
};

class HilReadyBarrier {
  public:
    static HilReadyResult publish(HilFaultController& controller,
                                  const HilReadyPublication& publication) noexcept {
        return controller.publishReady(
            publication.caseId, publication.faultId, publication.sessionHash,
            publication.armSequence, publication.activeGeneration, publication.exactPhase,
            publication.nowMs, publication.automaticReleaseAfterMs);
    }

    static bool shouldPause(const HilFaultController& controller,
                            const HilReadyIdentity& identity,
                            uint32_t nowMs) noexcept {
        return controller.shouldPause(
            identity.caseId, identity.faultId, identity.sessionHash, identity.armSequence,
            identity.readySequence, identity.activeGeneration, identity.exactPhase, nowMs);
    }
};

#endif  // V1SIMPLE_HIL_FAULT_CONTROL

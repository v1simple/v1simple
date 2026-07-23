#pragma once

#include <cstdint>

enum class ObdPhysicalLinkPreownershipOutcome : uint8_t {
    HoldOwnership,
    CancellationObserved,
    PreemptionConfirmed,
    LinkDownWithoutCancellation,
    DeadlineReached,
    InvalidRuntime,
};

struct ObdPhysicalLinkPreownershipRequest {
    uint32_t dispatchEpoch = 0;
    uint32_t activeGeneration = 0;
    uint32_t startedAtMs = 0;
    uint32_t maximumHoldMs = 0;
};

struct ObdPhysicalLinkPreownershipRuntime {
    using ClockMs = uint32_t (*)(void*) noexcept;
    using ReadCancellationEpoch = uint32_t (*)(void*) noexcept;
    using LinkDownConfirmed = bool (*)(uint32_t, void*) noexcept;

    ClockMs clockMs = nullptr;
    ReadCancellationEpoch cancellationEpoch = nullptr;
    LinkDownConfirmed linkDownConfirmed = nullptr;
    void* context = nullptr;
};

struct ObdPhysicalLinkPreownershipObservation {
    ObdPhysicalLinkPreownershipOutcome outcome = ObdPhysicalLinkPreownershipOutcome::InvalidRuntime;
    uint32_t observedAtMs = 0;
    uint32_t cancellationEpoch = 0;
};

// Non-blocking observation seam for the interval after the physical OBD link
// is established and before the runtime adopts it as a state-machine session.
// Callback and transport contexts only publish through the supplied atomic
// providers; the main-loop owner decides whether session adoption may proceed.
class ObdPhysicalLinkPreownershipBarrier {
  public:
    static bool runtimeValid(const ObdPhysicalLinkPreownershipRuntime& runtime) noexcept;
    static ObdPhysicalLinkPreownershipObservation observe(const ObdPhysicalLinkPreownershipRequest& request,
                                                          const ObdPhysicalLinkPreownershipRuntime& runtime) noexcept;
};

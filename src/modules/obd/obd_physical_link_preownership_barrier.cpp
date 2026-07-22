#include "obd_physical_link_preownership_barrier.h"

bool ObdPhysicalLinkPreownershipBarrier::runtimeValid(const ObdPhysicalLinkPreownershipRuntime& runtime) noexcept {
    return runtime.clockMs != nullptr && runtime.cancellationEpoch != nullptr && runtime.linkDownConfirmed != nullptr;
}

ObdPhysicalLinkPreownershipObservation
ObdPhysicalLinkPreownershipBarrier::observe(const ObdPhysicalLinkPreownershipRequest& request,
                                            const ObdPhysicalLinkPreownershipRuntime& runtime) noexcept {
    if (!runtimeValid(runtime) || request.activeGeneration == 0 || request.maximumHoldMs == 0) {
        return {};
    }

    const uint32_t nowMs = runtime.clockMs(runtime.context);
    const uint32_t cancellationEpoch = runtime.cancellationEpoch(runtime.context);
    const bool cancellationObserved = cancellationEpoch != request.dispatchEpoch;
    if (runtime.linkDownConfirmed(request.activeGeneration, runtime.context)) {
        return {cancellationObserved ? ObdPhysicalLinkPreownershipOutcome::PreemptionConfirmed
                                     : ObdPhysicalLinkPreownershipOutcome::LinkDownWithoutCancellation,
                nowMs, cancellationEpoch};
    }
    if (static_cast<uint32_t>(nowMs - request.startedAtMs) >= request.maximumHoldMs) {
        return {ObdPhysicalLinkPreownershipOutcome::DeadlineReached, nowMs, cancellationEpoch};
    }
    return {cancellationObserved ? ObdPhysicalLinkPreownershipOutcome::CancellationObserved
                                 : ObdPhysicalLinkPreownershipOutcome::HoldOwnership,
            nowMs, cancellationEpoch};
}

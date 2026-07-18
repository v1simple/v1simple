#include "wifi_maintenance_recovery_module.h"

void WifiMaintenanceRecoveryModule::reset() {
    downObserved_ = false;
    downSinceMs_ = 0;
    lastAttemptMs_ = 0;
    attempts_ = 0;
}

WifiMaintenanceRecoveryResult WifiMaintenanceRecoveryModule::evaluate(const WifiMaintenanceRecoveryInput& input) {
    WifiMaintenanceRecoveryResult result;
    if (!input.maintenanceBootActive || input.wifiServiceReachable) {
        reset();
        return result;
    }

    if (!downObserved_) {
        // First observation of the service being down: anchor the schedule.
        // Track anchoring separately so millis()==0 and repeated evaluations
        // during that first tick cannot look like an elapsed rollover.
        downObserved_ = true;
        downSinceMs_ = input.nowMs;
        return result;
    }

    // Unsigned subtraction is rollover-safe (repo-wide timer convention).
    const bool firstAttemptDue = (attempts_ == 0) && ((input.nowMs - downSinceMs_) >= kFirstRetryDelayMs);
    const bool repeatAttemptDue = (attempts_ > 0) && ((input.nowMs - lastAttemptMs_) >= kRetryIntervalMs);
    if (firstAttemptDue || repeatAttemptDue) {
        ++attempts_;
        lastAttemptMs_ = input.nowMs;
        result.attemptRestart = true;
        result.attemptNumber = attempts_;
    }
    return result;
}

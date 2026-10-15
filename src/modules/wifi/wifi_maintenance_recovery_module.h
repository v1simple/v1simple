#pragma once

#include <stdint.h>

// Restart failed maintenance Wi-Fi after kFirstRetryDelayMs, then every
// kRetryIntervalMs until reachable. Reachability resets the schedule.
// Maintenance suppresses deliberate AP/no-client/idle stops at their owners,
// so this policy recovers failed starts and emergency low-memory stops.
struct WifiMaintenanceRecoveryInput {
    bool maintenanceBootActive = false;
    // Manager state alone is insufficient: the low-SRAM path can keep the
    // server alive after dropping an AP whose STA connection never completed.
    bool wifiServiceReachable = false;
    unsigned long nowMs = 0;
};

struct WifiMaintenanceRecoveryResult {
    bool attemptRestart = false;
    // 1-based attempt counter, valid when attemptRestart is true.
    uint32_t attemptNumber = 0;
};

class WifiMaintenanceRecoveryModule {
  public:
    static constexpr unsigned long kFirstRetryDelayMs = 3000;
    static constexpr unsigned long kRetryIntervalMs = 30000;

    WifiMaintenanceRecoveryResult evaluate(const WifiMaintenanceRecoveryInput& input);
    void reset();

    uint32_t attemptCount() const { return attempts_; }

  private:
    bool downObserved_ = false;
    unsigned long downSinceMs_ = 0;
    unsigned long lastAttemptMs_ = 0;
    uint32_t attempts_ = 0;
};

#pragma once

#include <stdint.h>

// Decides when the maintenance-boot loop should attempt to restart the WiFi
// setup service. A maintenance session exists solely to serve the web UI, so
// it must never sit WiFi-dead for its (bounded) lifetime: the AP can fail to
// start at maintenance entry, and an emergency stop (sustained low internal
// SRAM) can take the service down mid-session. Neither path has any other
// recovery until the maintenance timeout reboots the device.
//
// Policy: once the service is observed down, allow a first restart attempt
// after `kFirstRetryDelayMs` (so a deliberate in-flight stop/start settles),
// then further attempts every `kRetryIntervalMs`. Observing the service
// reachable resets the schedule. Deliberate idle stops (AP retirement,
// no-client, auto-timeout) are suppressed in maintenance boot at their
// decision sites, so this module only ever fights genuine failures.
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

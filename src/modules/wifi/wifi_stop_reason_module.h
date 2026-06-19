#pragma once

#include <stdint.h>

struct PerfCounters;

enum class WifiStopReason : uint8_t {
    Timeout = 0,
    NoClients = 1,
    NoClientsAuto = 2,
    LowDma = 3,
    Poweroff = 4,
    Other = 5
};

// Classifies and records WiFi stop/ap-drop telemetry without owning WiFi behavior.
class WifiStopReasonModule {
public:
    explicit WifiStopReasonModule(PerfCounters* counters = nullptr);

    void setCounters(PerfCounters* counters);
    WifiStopReason classify(const char* stopReason) const;
    void recordStopRequest(const char* stopReason, bool manual, bool immediate) const;
    void recordApDropLowDma() const;
    void recordApDropIdleSta() const;

private:
    PerfCounters* counters_ = nullptr;
};

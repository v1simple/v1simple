#include "wifi_stop_reason_module.h"

#include <cstring>

#include "../../perf_metrics.h"

WifiStopReasonModule::WifiStopReasonModule(PerfCounters* counters)
    : counters_(counters) {}

void WifiStopReasonModule::setCounters(PerfCounters* counters) {
    counters_ = counters;
}

WifiStopReason WifiStopReasonModule::classify(const char* stopReason) const {
    if (!stopReason || stopReason[0] == '\0') {
        return WifiStopReason::Other;
    }
    if (strcmp(stopReason, "timeout") == 0) {
        return WifiStopReason::Timeout;
    }
    if (strcmp(stopReason, "no_clients") == 0) {
        return WifiStopReason::NoClients;
    }
    if (strcmp(stopReason, "no_clients_auto") == 0) {
        return WifiStopReason::NoClientsAuto;
    }
    if (strcmp(stopReason, "low_dma") == 0) {
        return WifiStopReason::LowDma;
    }
    if (strcmp(stopReason, "poweroff") == 0) {
        return WifiStopReason::Poweroff;
    }
    return WifiStopReason::Other;
}

void WifiStopReasonModule::recordStopRequest(const char* stopReason, bool manual, bool immediate) const {
    if (!counters_) {
        return;
    }

    if (immediate) {
        counters_->wifiStopImmediate++;
    } else {
        counters_->wifiStopGraceful++;
    }
    if (manual) {
        counters_->wifiStopManual++;
    }

    switch (classify(stopReason)) {
        case WifiStopReason::Timeout:
            counters_->wifiStopTimeout++;
            break;
        case WifiStopReason::NoClients:
            counters_->wifiStopNoClients++;
            break;
        case WifiStopReason::NoClientsAuto:
            counters_->wifiStopNoClientsAuto++;
            break;
        case WifiStopReason::LowDma:
            counters_->wifiStopLowDma++;
            break;
        case WifiStopReason::Poweroff:
            counters_->wifiStopPoweroff++;
            break;
        case WifiStopReason::Other:
        default:
            counters_->wifiStopOther++;
            break;
    }
}

void WifiStopReasonModule::recordApDropLowDma() const {
    if (!counters_) {
        return;
    }
    counters_->wifiApDropLowDma++;
}

void WifiStopReasonModule::recordApDropIdleSta() const {
    if (!counters_) {
        return;
    }
    counters_->wifiApDropIdleSta++;
}

/**
 * Perf reason/state name tables.
 * Moved verbatim out of perf_metrics.cpp; no behavior change.
 */

#include "perf_metrics.h"

namespace {
static const char* wifiApTransitionReasonNameInternal(uint32_t reasonCode) {
    switch (static_cast<PerfWifiApTransitionReason>(reasonCode)) {
        case PerfWifiApTransitionReason::Startup:
            return "startup";
        case PerfWifiApTransitionReason::StopManual:
            return "stop_manual";
        case PerfWifiApTransitionReason::StopTimeout:
            return "stop_timeout";
        case PerfWifiApTransitionReason::StopNoClients:
            return "stop_no_clients";
        case PerfWifiApTransitionReason::StopNoClientsAuto:
            return "stop_no_clients_auto";
        case PerfWifiApTransitionReason::DropLowDma:
            return "drop_low_dma";
        case PerfWifiApTransitionReason::DropIdleSta:
            return "drop_idle_sta";
        case PerfWifiApTransitionReason::StopPoweroff:
            return "stop_poweroff";
        case PerfWifiApTransitionReason::StopOther:
            return "stop_other";
        case PerfWifiApTransitionReason::Unknown:
        default:
            return "unknown";
    }
}

static const char* proxyAdvertisingTransitionReasonNameInternal(uint32_t reasonCode) {
    switch (static_cast<PerfProxyAdvertisingTransitionReason>(reasonCode)) {
        case PerfProxyAdvertisingTransitionReason::StartConnected:
            return "start_connected";
        case PerfProxyAdvertisingTransitionReason::StartWifiPriorityResume:
            return "start_wifi_priority_resume";
        case PerfProxyAdvertisingTransitionReason::StartRetryWindow:
            return "start_retry_window";
        case PerfProxyAdvertisingTransitionReason::StartAppDisconnect:
            return "start_app_disconnect";
        case PerfProxyAdvertisingTransitionReason::StartDirect:
            return "start_direct";
        case PerfProxyAdvertisingTransitionReason::StopWifiPriority:
            return "stop_wifi_priority";
        case PerfProxyAdvertisingTransitionReason::StopNoClientTimeout:
            return "stop_no_client_timeout";
        case PerfProxyAdvertisingTransitionReason::StopIdleWindow:
            return "stop_idle_window";
        case PerfProxyAdvertisingTransitionReason::StopBeforeV1Connect:
            return "stop_before_v1_connect";
        case PerfProxyAdvertisingTransitionReason::StopV1Disconnect:
            return "stop_v1_disconnect";
        case PerfProxyAdvertisingTransitionReason::StopAppConnected:
            return "stop_app_connected";
        case PerfProxyAdvertisingTransitionReason::StopOther:
            return "stop_other";
        case PerfProxyAdvertisingTransitionReason::Unknown:
        default:
            return "unknown";
    }
}

static const char* connectionCycleStateNameInternal(uint8_t stateCode) {
    switch (stateCode) {
        case 0:
            return "scan_v1";
        case 1:
            return "v1_settling";
        case 2:
            return "obd_scan";
        case 3:
            return "obd_connect";
        case 4:
            return "obd_settled";
        case 5:
            return "proxy_open";
        case 6:
            return "wifi_open";
        case 7:
            return "steady";
        case 8:
            return "teardown";
        default:
            return "unknown";
    }
}
}  // namespace

const char* perfConnectionCycleStateName(uint8_t stateCode) {
    return connectionCycleStateNameInternal(stateCode);
}

const char* perfWifiApTransitionReasonName(uint32_t reasonCode) {
    return wifiApTransitionReasonNameInternal(reasonCode);
}

const char* perfProxyAdvertisingTransitionReasonName(uint32_t reasonCode) {
    return proxyAdvertisingTransitionReasonNameInternal(reasonCode);
}

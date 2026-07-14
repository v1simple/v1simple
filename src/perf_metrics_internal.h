/**
 * Shared internals for the perf_metrics translation units.
 *
 * perf_metrics.cpp, perf_snapshot.cpp, perf_names.cpp and perf_report.cpp are
 * sections of one subsystem that were split out of a single oversized TU. The
 * session/window state below used to be file-static; it is shared across those
 * sections and is defined once in perf_metrics.cpp.
 *
 * Not a public API. Everything outside src/perf_*.cpp includes perf_metrics.h.
 */

#pragma once

#include "perf_metrics.h"

#include <atomic>
#include <freertos/FreeRTOS.h>

// Session/window state owned by perf_metrics.cpp.
extern uint32_t sDmaFreeCapMin;
extern uint32_t sDmaLargestCapMin;
extern std::atomic<uint32_t> sPrevWindowLoopMaxUs;
extern std::atomic<uint32_t> sPrevWindowWifiMaxUs;
extern std::atomic<uint32_t> sPrevWindowBleProcessMaxUs;
extern std::atomic<uint32_t> sPrevWindowDispPipeMaxUs;
extern std::atomic<uint8_t> sConnectionCycleStateCode;
extern std::atomic<uint32_t> sConnectionCycleTimeInStateMs;
extern std::atomic<uint32_t> sConnectionCycleTransitionsTotal;
extern std::atomic<uint32_t> sConnectionCycleTeardownDurationMs;
extern std::atomic<uint32_t> sConnectionCycleObdRetryAttemptsTotal;
extern std::atomic<uint32_t> sConnectionCycleWifiManualPhoneKicksTotal;
extern std::atomic<uint8_t> sConnectionCycleProxyNoClientLatched;
extern portMUX_TYPE sPerfSnapshotMux;

// Flat SD-snapshot capture; defined in perf_snapshot.cpp, used by perf_report.cpp.
void captureSdSnapshot(PerfSdSnapshot& snapshot);

#pragma once

#include <stdint.h>

namespace WifiHeapThresholds {

inline constexpr uint32_t kStartMinFreeApOnly = 28672;    // 28KB
inline constexpr uint32_t kStartMinBlockApOnly = 12288;   // 12KB
inline constexpr uint32_t kStartMinFreeApSta = 40960;     // 40KB
inline constexpr uint32_t kStartMinBlockApSta = 20480;    // 20KB
inline constexpr uint32_t kRuntimeMinFreeApOnly = 16384;  // 16KB
inline constexpr uint32_t kRuntimeMinBlockApOnly = 12288; // 12KB
inline constexpr uint32_t kRuntimeMinFreeStaOnly = 16384; // 16KB
inline constexpr uint32_t kRuntimeMinBlockStaOnly = 7168; // 7KB
inline constexpr uint32_t kRuntimeMinFreeApSta = 20480;   // 20KB
inline constexpr uint32_t kRuntimeMinBlockApSta = 8192;   // 8KB

// Radio allocator churn can briefly cross a threshold by a tiny amount. These
// tolerances prevent WARN/RECOVER oscillation without weakening the floors.
inline constexpr uint32_t kRuntimeApStaFreeJitterTolerance = 256;
inline constexpr uint32_t kRuntimeStaBlockJitterTolerance = 128;

} // namespace WifiHeapThresholds

struct WifiHeapGuardInput {
    bool dualRadioMode = false;
    bool staRadioOn = false;
    bool staOnlyMode = false;
    uint32_t freeInternal = 0;
    uint32_t largestInternal = 0;
    uint32_t criticalFree = 0;
    uint32_t criticalBlock = 0;
    uint32_t apStaFreeJitterTolerance = 0;
    uint32_t staOnlyBlockJitterTolerance = 0;
};

struct WifiHeapGuardResult {
    bool freeLow = false;
    bool blockLow = false;
    bool lowHeap = false;
    const char* modeLabel = "AP";
};

// Evaluates WiFi runtime heap pressure with mode-aware jitter tolerance.
WifiHeapGuardResult evaluateWifiHeapGuard(const WifiHeapGuardInput& input);

/**
 * Periodic persistence helpers using rate limits and non-blocking SD access.
 */

#include "main_internals.h"
#include "storage_manager.h"
#include "wifi_manager.h"
#include "v1_devices.h"
#include <ArduinoJson.h>
#include <esp_heap_caps.h>

#ifndef MALLOC_CAP_DMA
#define MALLOC_CAP_DMA (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#endif

namespace {
static constexpr uint32_t SAVE_DIAG_REPORT_INTERVAL_MS = 60000; // 60 seconds

// This path is normal-boot only. processV1DeviceStoreSave() has one caller in
// DriveRuntime's typed periodic-maintenance phase. WiFi starts only in maintenance
// boot (MaintenanceRuntime owns every startSetupMode() call), so
// WiFi.getMode() here can only be WIFI_OFF.
//
// That made two thirds of this gate unreachable, and it has now been removed:
//
//   - the AP+STA and STA-only threshold branches (staRadioOn was always false)
//   - BACKGROUND_SAVE_AGED_DMA_FREE / _BLOCK / MAX_DIRTY_AGE_MS and the whole
//     aged-retry path, which was gated on thresholds.allowAgedRetry -- set true
//     only in the dead AP+STA branch, so the && short-circuited every time and
//     hasAgedDmaHeadroomForBackgroundSave() was never once called
//   - withinDeficitTolerance(), which took a tolerance that was always 0:
//     `sample < required && (required - sample) <= 0` is unsatisfiable on
//     unsigned arithmetic, so it could never return true
//   - the freeJitter/blockJitter/agedFree/agedBlock/modeLabel fields those fed
//
// What is left is the AP-only pair that was always in force. The names are kept
// pointing at WiFiManager because that is where the numbers were calibrated,
// even though the radio cannot be on when they are read.
struct SaveDmaThresholds {
    uint32_t minFree = 0;
    uint32_t minBlock = 0;
};

struct SaveDiagStats {
    uint32_t attempts = 0;
    uint32_t success = 0;
    uint32_t fail = 0;
    uint32_t deferLowDma = 0;
    uint32_t deferSdBusy = 0;
    uint32_t minFreeOnSuccess = UINT32_MAX;
    uint32_t minBlockOnSuccess = UINT32_MAX;
    uint32_t minFreeOnFail = UINT32_MAX;
    uint32_t minBlockOnFail = UINT32_MAX;
    uint32_t minFreeOnDeferLow = UINT32_MAX;
    uint32_t minBlockOnDeferLow = UINT32_MAX;
    uint32_t deferRecoveries = 0;
    uint32_t lastDeferToSaveMs = 0;
    uint32_t maxDeferToSaveMs = 0;
    uint32_t lastReportMs = 0;
    uint32_t lastReportedAttempts = 0;
};

SaveDmaThresholds getSaveDmaThresholds() {
    SaveDmaThresholds thresholds{};
    thresholds.minFree = WiFiManager::WIFI_RUNTIME_MIN_FREE_AP_ONLY;
    thresholds.minBlock = WiFiManager::WIFI_RUNTIME_MIN_BLOCK_AP_ONLY;
    return thresholds;
}

// Note the mask: this samples MALLOC_CAP_DMA while the thresholds above were
// sized against MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, the pool the WiFi runtime
// guard reads at wifi_manager_lifecycle.cpp:641-642. The two disagree by a stable
// offset (min 2,296 / median 7,664 / max 14,756 bytes across 42,437 bench rows,
// equal in none of them), which makes the 16,384 B floor effectively ~24,000 B
// here.
//
// Left as-is on purpose. The worst freeDmaCap ever recorded on the bench is
// 77,584 B -- 4.7x the floor, with 61 KB of slack -- and the worst largestDmaCap
// is 45,044 B against a 12,288 B block floor. The offset changes no decision and
// cannot be pushed toward one, because the WiFi that the constants anticipate
// cannot run in this mode. Switching the mask would tighten a gate that has never
// come close to firing.
inline bool hasDmaHeadroomForBackgroundSave(uint32_t& freeDma, uint32_t& largestDma,
                                            const SaveDmaThresholds& thresholds) {
    freeDma = heap_caps_get_free_size(MALLOC_CAP_DMA);
    largestDma = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    return (freeDma >= thresholds.minFree) && (largestDma >= thresholds.minBlock);
}

inline void noteMin(uint32_t& target, uint32_t sample) {
    if (sample < target) {
        target = sample;
    }
}

inline unsigned long sampleOrZero(uint32_t sample) {
    return static_cast<unsigned long>((sample == UINT32_MAX) ? 0 : sample);
}

void maybeLogSaveDiag(const char* tag, SaveDiagStats& stats, uint32_t nowMs) {
    if ((nowMs - stats.lastReportMs) < SAVE_DIAG_REPORT_INTERVAL_MS) {
        return;
    }
    if (stats.attempts == stats.lastReportedAttempts) {
        stats.lastReportMs = nowMs;
        return;
    }
    stats.lastReportMs = nowMs;
    stats.lastReportedAttempts = stats.attempts;
    Serial.printf(
        "[%s] SaveDiag attempts=%lu ok=%lu fail=%lu deferLow=%lu deferBusy=%lu minOk=%lu/%lu "
        "minFail=%lu/%lu minDeferLow=%lu/%lu recoveries=%lu lastDeferMs=%lu maxDeferMs=%lu\n",
        tag, static_cast<unsigned long>(stats.attempts), static_cast<unsigned long>(stats.success),
        static_cast<unsigned long>(stats.fail), static_cast<unsigned long>(stats.deferLowDma),
        static_cast<unsigned long>(stats.deferSdBusy), sampleOrZero(stats.minFreeOnSuccess),
        sampleOrZero(stats.minBlockOnSuccess), sampleOrZero(stats.minFreeOnFail),
        sampleOrZero(stats.minBlockOnFail), sampleOrZero(stats.minFreeOnDeferLow),
        sampleOrZero(stats.minBlockOnDeferLow), static_cast<unsigned long>(stats.deferRecoveries),
        static_cast<unsigned long>(stats.lastDeferToSaveMs), static_cast<unsigned long>(stats.maxDeferToSaveMs));
}
} // namespace

// --- Generic dirty-save state machine ---

struct DirtySaveConfig {
    const char* tag;         // Log prefix, e.g. "V1DeviceStore"
    uint32_t saveIntervalMs; // Minimum interval between successful saves
    uint32_t retryMs;        // Minimum interval between attempts
};

struct DirtySaveState {
    uint32_t lastSaveMs = 0;
    uint32_t lastAttemptMs = 0;
    uint32_t dirtySinceMs = 0;
    uint32_t deferredSinceMs = 0;
    SaveDiagStats diag;
};

static constexpr uint32_t V1_DEVICE_STORE_SAVE_INTERVAL_MS = 5000;
static constexpr uint32_t V1_DEVICE_STORE_SAVE_RETRY_MS = 1000;

static void processDirtySave(const DirtySaveConfig& cfg, DirtySaveState& state, uint32_t nowMs,
                             StorageManager& storage, V1DeviceStore& devices) {
    if (devices.hasPendingSave()) {
        if (state.dirtySinceMs == 0) {
            state.dirtySinceMs = nowMs;
        }
    } else {
        state.dirtySinceMs = 0;
        state.deferredSinceMs = 0;
    }

    if (devices.hasPendingSave() && storage.isReady() && (nowMs - state.lastSaveMs) >= cfg.saveIntervalMs &&
        (nowMs - state.lastAttemptMs) >= cfg.retryMs) {
        state.diag.attempts++;
        state.lastAttemptMs = nowMs;

        fs::FS* fs = storage.getFilesystem();
        bool saveOk = false;
        bool saveDeferred = false;
        bool hadDmaSample = false;
        uint32_t sampledFreeDma = 0;
        uint32_t sampledLargestDma = 0;

        if (fs) {
            if (storage.isSDCard()) {
                const SaveDmaThresholds thresholds = getSaveDmaThresholds();
                uint32_t freeDma = 0;
                uint32_t largestDma = 0;
                const bool normalHeadroom = hasDmaHeadroomForBackgroundSave(freeDma, largestDma, thresholds);
                const uint32_t dirtyAgeMs = (state.dirtySinceMs == 0) ? 0 : (nowMs - state.dirtySinceMs);
                hadDmaSample = true;
                sampledFreeDma = freeDma;
                sampledLargestDma = largestDma;

                if (normalHeadroom) {
                    // checkDmaHeap=false: hasDmaHeadroomForBackgroundSave() just
                    // sampled the same heap against a stricter pair of floors.
                    StorageManager::SDTryLock sdLock(storage.getSDMutex(), /*checkDmaHeap=*/false);
                    if (sdLock) {
                        saveOk = devices.flushPendingSave();
                    } else {
                        saveDeferred = true;
                        state.diag.deferSdBusy++;
                        static uint32_t lastSaveSkipLogMs = 0;
                        if ((nowMs - lastSaveSkipLogMs) >= 10000) {
                            lastSaveSkipLogMs = nowMs;
                            Serial.printf("[%s] Save deferred (SD busy)\n", cfg.tag);
                        }
                    }
                } else {
                    saveDeferred = true;
                    state.diag.deferLowDma++;
                    noteMin(state.diag.minFreeOnDeferLow, freeDma);
                    noteMin(state.diag.minBlockOnDeferLow, largestDma);
                    static uint32_t lastLowDmaLogMs = 0;
                    if ((nowMs - lastLowDmaLogMs) >= 10000) {
                        lastLowDmaLogMs = nowMs;
                        Serial.printf(
                            "[%s] Save deferred (low DMA heap free=%lu block=%lu need>=%lu/%lu dirty=%lus)\n",
                            cfg.tag, static_cast<unsigned long>(freeDma), static_cast<unsigned long>(largestDma),
                            static_cast<unsigned long>(thresholds.minFree),
                            static_cast<unsigned long>(thresholds.minBlock),
                            static_cast<unsigned long>(dirtyAgeMs / 1000));
                    }
                }
            } else {
                saveOk = devices.flushPendingSave();
            }
        }

        if (saveOk) {
            if (state.deferredSinceMs != 0) {
                const uint32_t deferLatencyMs = nowMs - state.deferredSinceMs;
                const uint32_t dirtyAgeMs = (state.dirtySinceMs == 0) ? 0 : (nowMs - state.dirtySinceMs);
                state.diag.deferRecoveries++;
                state.diag.lastDeferToSaveMs = deferLatencyMs;
                if (deferLatencyMs > state.diag.maxDeferToSaveMs) {
                    state.diag.maxDeferToSaveMs = deferLatencyMs;
                }
                Serial.printf("[%s] Save recovered after defer latency=%lus dirty=%lus\n", cfg.tag,
                              static_cast<unsigned long>(deferLatencyMs / 1000),
                              static_cast<unsigned long>(dirtyAgeMs / 1000));
                state.deferredSinceMs = 0;
            }
            state.lastSaveMs = nowMs;
            state.dirtySinceMs = 0;
            state.diag.success++;
            if (hadDmaSample) {
                noteMin(state.diag.minFreeOnSuccess, sampledFreeDma);
                noteMin(state.diag.minBlockOnSuccess, sampledLargestDma);
            }
        } else if (!saveDeferred) {
            state.diag.fail++;
            if (hadDmaSample) {
                noteMin(state.diag.minFreeOnFail, sampledFreeDma);
                noteMin(state.diag.minBlockOnFail, sampledLargestDma);
            }
            Serial.printf("[%s] Save failed\n", cfg.tag);
        } else if (state.deferredSinceMs == 0) {
            state.deferredSinceMs = nowMs;
        }
        maybeLogSaveDiag(cfg.tag, state.diag, nowMs);
    }

}

// --- V1DeviceStore save instance ---

static const DirtySaveConfig deviceStoreSaveConfig = {
    .tag = "V1DeviceStore",
    .saveIntervalMs = V1_DEVICE_STORE_SAVE_INTERVAL_MS,
    .retryMs = V1_DEVICE_STORE_SAVE_RETRY_MS,
};

static DirtySaveState deviceStoreSaveState;

void processV1DeviceStoreSave(uint32_t nowMs, StorageManager& storage, V1DeviceStore& devices) {
    processDirtySave(deviceStoreSaveConfig, deviceStoreSaveState, nowMs, storage, devices);
}

/**
 * main_persist.cpp — Periodic persistence helpers extracted from loop().
 *
 * Self-contained save state machines using rate-limiting, dirty flags, and
 * non-blocking SD try-locks (Tier 7 — best-effort, never block).
 */

#include "main_internals.h"
#include "perf_metrics.h"
#include "storage_manager.h"
#include "wifi_manager.h"
#include "v1_devices.h"
#include <ArduinoJson.h>
#include <esp_heap_caps.h>

#ifndef MALLOC_CAP_DMA
#define MALLOC_CAP_DMA (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#endif

namespace {
// If dirty data remains unsaved for too long, allow a cautious retry using a
// lower free-heap floor tuned to observed AP+STA steady-state with a stricter
// largest-block guard.
static constexpr uint32_t BACKGROUND_SAVE_AGED_DMA_FREE = 16896;
static constexpr uint32_t BACKGROUND_SAVE_AGED_DMA_BLOCK = 10240;
static constexpr uint32_t BACKGROUND_SAVE_MAX_DIRTY_AGE_MS = 90000;  // 90 seconds
static constexpr uint32_t SAVE_DIAG_REPORT_INTERVAL_MS = 60000;    // 60 seconds

struct SaveDmaThresholds {
    uint32_t minFree = 0;
    uint32_t minBlock = 0;
    uint32_t freeJitterTolerance = 0;
    uint32_t blockJitterTolerance = 0;
    uint32_t agedFree = 0;
    uint32_t agedBlock = 0;
    bool allowAgedRetry = false;
    const char* modeLabel = "unknown";
};

struct SaveDiagStats {
    uint32_t attempts = 0;
    uint32_t success = 0;
    uint32_t fail = 0;
    uint32_t deferLowDma = 0;
    uint32_t deferSdBusy = 0;
    uint32_t agedRetryAttempts = 0;
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

inline bool withinDeficitTolerance(uint32_t sample, uint32_t required, uint32_t tolerance) {
    return sample < required && (required - sample) <= tolerance;
}

SaveDmaThresholds getSaveDmaThresholds() {
    SaveDmaThresholds thresholds{};
    const wifi_mode_t mode = WiFi.getMode();
    const bool staRadioOn = (mode == WIFI_AP_STA || mode == WIFI_STA);
    const bool apStaMode = wifiManager.isSetupModeActive() && staRadioOn;
    const bool staOnlyMode = staRadioOn && !apStaMode;

    if (apStaMode) {
        thresholds.minFree = WiFiManager::WIFI_RUNTIME_MIN_FREE_AP_STA;
        thresholds.minBlock = WiFiManager::WIFI_RUNTIME_MIN_BLOCK_AP_STA;
        thresholds.freeJitterTolerance = WiFiManager::WIFI_RUNTIME_AP_STA_FREE_JITTER_TOLERANCE;
        thresholds.blockJitterTolerance = 0;
        thresholds.agedFree = BACKGROUND_SAVE_AGED_DMA_FREE;
        thresholds.agedBlock = BACKGROUND_SAVE_AGED_DMA_BLOCK;
        thresholds.allowAgedRetry = true;
        thresholds.modeLabel = "AP+STA";
        return thresholds;
    }

    if (staOnlyMode) {
        thresholds.minFree = WiFiManager::WIFI_RUNTIME_MIN_FREE_STA_ONLY;
        thresholds.minBlock = WiFiManager::WIFI_RUNTIME_MIN_BLOCK_STA_ONLY;
        thresholds.freeJitterTolerance = 0;
        thresholds.blockJitterTolerance = WiFiManager::WIFI_RUNTIME_STA_BLOCK_JITTER_TOLERANCE;
        thresholds.agedFree = thresholds.minFree;
        thresholds.agedBlock = thresholds.minBlock;
        thresholds.allowAgedRetry = false;
        thresholds.modeLabel = "STA";
        return thresholds;
    }

    thresholds.minFree = WiFiManager::WIFI_RUNTIME_MIN_FREE_AP_ONLY;
    thresholds.minBlock = WiFiManager::WIFI_RUNTIME_MIN_BLOCK_AP_ONLY;
    thresholds.freeJitterTolerance = 0;
    thresholds.blockJitterTolerance = 0;
    thresholds.agedFree = thresholds.minFree;
    thresholds.agedBlock = thresholds.minBlock;
    thresholds.allowAgedRetry = false;
    thresholds.modeLabel = "AP";
    return thresholds;
}

inline bool hasDmaHeadroomForBackgroundSave(uint32_t& freeDma,
                                            uint32_t& largestDma,
                                            const SaveDmaThresholds& thresholds) {
    freeDma = heap_caps_get_free_size(MALLOC_CAP_DMA);
    largestDma = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    const bool freeOk =
        (freeDma >= thresholds.minFree) ||
        withinDeficitTolerance(freeDma,
                               thresholds.minFree,
                               thresholds.freeJitterTolerance);
    const bool blockOk =
        (largestDma >= thresholds.minBlock) ||
        withinDeficitTolerance(largestDma,
                               thresholds.minBlock,
                               thresholds.blockJitterTolerance);
    return freeOk && blockOk;
}

inline bool hasAgedDmaHeadroomForBackgroundSave(uint32_t freeDma,
                                                uint32_t largestDma,
                                                const SaveDmaThresholds& thresholds) {
    return (freeDma >= thresholds.agedFree) && (largestDma >= thresholds.agedBlock);
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
    Serial.printf("[%s] SaveDiag attempts=%lu ok=%lu fail=%lu deferLow=%lu deferBusy=%lu agedTry=%lu minOk=%lu/%lu minFail=%lu/%lu minDeferLow=%lu/%lu recoveries=%lu lastDeferMs=%lu maxDeferMs=%lu\n",
                  tag,
                  static_cast<unsigned long>(stats.attempts),
                  static_cast<unsigned long>(stats.success),
                  static_cast<unsigned long>(stats.fail),
                  static_cast<unsigned long>(stats.deferLowDma),
                  static_cast<unsigned long>(stats.deferSdBusy),
                  static_cast<unsigned long>(stats.agedRetryAttempts),
                  sampleOrZero(stats.minFreeOnSuccess),
                  sampleOrZero(stats.minBlockOnSuccess),
                  sampleOrZero(stats.minFreeOnFail),
                  sampleOrZero(stats.minBlockOnFail),
                  sampleOrZero(stats.minFreeOnDeferLow),
                  sampleOrZero(stats.minBlockOnDeferLow),
                  static_cast<unsigned long>(stats.deferRecoveries),
                  static_cast<unsigned long>(stats.lastDeferToSaveMs),
                  static_cast<unsigned long>(stats.maxDeferToSaveMs));
}
}  // namespace

// --- Generic dirty-save state machine ---

struct DirtySaveConfig {
    const char* tag;           // Log prefix, e.g. "V1DeviceStore"
    const char* filePath;      // Destination file path
    uint32_t saveIntervalMs;   // Minimum interval between successful saves
    uint32_t retryMs;          // Minimum interval between attempts

    // Data source callbacks (no virtual overhead)
    bool (*isDirty)();
    void (*clearDirty)();
    bool (*saveDirect)(fs::FS& fs, const char* path);
    void (*logSuccess)(const char* path);
    void (*recordPerfUs)(uint32_t us);
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

static void processDirtySave(const DirtySaveConfig& cfg, DirtySaveState& state, uint32_t nowMs) {
    if (!cfg.saveDirect) {
        return;
    }
    uint32_t startUs = PERF_TIMESTAMP_US();

    if (cfg.isDirty()) {
        if (state.dirtySinceMs == 0) {
            state.dirtySinceMs = nowMs;
        }
    } else {
        state.dirtySinceMs = 0;
        state.deferredSinceMs = 0;
    }

    if (cfg.isDirty() && storageManager.isReady() &&
        (nowMs - state.lastSaveMs) >= cfg.saveIntervalMs &&
        (nowMs - state.lastAttemptMs) >= cfg.retryMs) {
        state.diag.attempts++;
        state.lastAttemptMs = nowMs;

        fs::FS* fs = storageManager.getFilesystem();
        bool saveOk = false;
        bool saveDeferred = false;
        bool hadDmaSample = false;
        uint32_t sampledFreeDma = 0;
        uint32_t sampledLargestDma = 0;

        if (fs) {
            if (storageManager.isSDCard()) {
                const SaveDmaThresholds thresholds = getSaveDmaThresholds();
                uint32_t freeDma = 0;
                uint32_t largestDma = 0;
                const bool normalHeadroom = hasDmaHeadroomForBackgroundSave(freeDma, largestDma, thresholds);
                const uint32_t dirtyAgeMs = (state.dirtySinceMs == 0) ? 0 : (nowMs - state.dirtySinceMs);
                const bool allowAgedRetry =
                    thresholds.allowAgedRetry &&
                    !normalHeadroom &&
                    (dirtyAgeMs >= BACKGROUND_SAVE_MAX_DIRTY_AGE_MS) &&
                    hasAgedDmaHeadroomForBackgroundSave(freeDma, largestDma, thresholds);
                hadDmaSample = true;
                sampledFreeDma = freeDma;
                sampledLargestDma = largestDma;

                if (normalHeadroom || allowAgedRetry) {
                    if (allowAgedRetry) {
                        state.diag.agedRetryAttempts++;
                        static uint32_t lastAgedRetryLogMs = 0;
                        if ((nowMs - lastAgedRetryLogMs) >= 10000) {
                            lastAgedRetryLogMs = nowMs;
                            Serial.printf("[%s] Save retry (aged dirty=%lus free=%lu block=%lu relaxed>=%lu/%lu)\n",
                                          cfg.tag,
                                          static_cast<unsigned long>(dirtyAgeMs / 1000),
                                          static_cast<unsigned long>(freeDma),
                                          static_cast<unsigned long>(largestDma),
                                          static_cast<unsigned long>(thresholds.agedFree),
                                          static_cast<unsigned long>(thresholds.agedBlock));
                        }
                    }
                    StorageManager::SDTryLock sdLock(storageManager.getSDMutex(), /*checkDmaHeap=*/false);
                    if (sdLock) {
                        saveOk = cfg.saveDirect(*fs, cfg.filePath);
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
                        Serial.printf("[%s] Save deferred (low DMA heap mode=%s free=%lu block=%lu need>=%lu/%lu dirty=%lus)\n",
                                      cfg.tag,
                                      thresholds.modeLabel,
                                      static_cast<unsigned long>(freeDma),
                                      static_cast<unsigned long>(largestDma),
                                      static_cast<unsigned long>(thresholds.minFree),
                                      static_cast<unsigned long>(thresholds.minBlock),
                                      static_cast<unsigned long>(dirtyAgeMs / 1000));
                    }
                }
            } else {
                saveOk = cfg.saveDirect(*fs, cfg.filePath);
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
                Serial.printf("[%s] Save recovered after defer latency=%lus dirty=%lus\n",
                              cfg.tag,
                              static_cast<unsigned long>(deferLatencyMs / 1000),
                              static_cast<unsigned long>(dirtyAgeMs / 1000));
                state.deferredSinceMs = 0;
            }
            state.lastSaveMs = nowMs;
            cfg.clearDirty();
            state.dirtySinceMs = 0;
            state.diag.success++;
            if (hadDmaSample) {
                noteMin(state.diag.minFreeOnSuccess, sampledFreeDma);
                noteMin(state.diag.minBlockOnSuccess, sampledLargestDma);
            }
            cfg.logSuccess(cfg.filePath);
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

    cfg.recordPerfUs(PERF_TIMESTAMP_US() - startUs);
}

// --- V1DeviceStore save instance ---

static const DirtySaveConfig v1DeviceStoreSaveConfig = {
    .tag = "V1DeviceStore",
    .filePath = "/v1devices.json",
    .saveIntervalMs = V1_DEVICE_STORE_SAVE_INTERVAL_MS,
    .retryMs = V1_DEVICE_STORE_SAVE_RETRY_MS,
    .isDirty = []() { return v1DeviceStore.hasPendingSave(); },
    .clearDirty = []() {},
    .saveDirect = [](fs::FS& /*fs*/, const char* /*path*/) { return v1DeviceStore.flushPendingSave(); },
    .logSuccess = [](const char* /*path*/) {},
    .recordPerfUs = [](uint32_t /*us*/) {},
};

static DirtySaveState v1DeviceStoreSaveState;

void processV1DeviceStoreSave(uint32_t nowMs) {
    processDirtySave(v1DeviceStoreSaveConfig, v1DeviceStoreSaveState, nowMs);
}

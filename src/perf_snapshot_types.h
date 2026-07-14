/**
 * Perf snapshot type definitions.
 *
 * Extracted from perf_metrics.h (pure move) so the metric-recording surface
 * and the snapshot payload schema can be read independently. perf_metrics.h
 * re-includes this header, so existing includers need no change and should
 * keep including perf_metrics.h.
 */

#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <atomic>

struct PerfSdSnapshot {
    uint32_t millisTs;
    uint32_t rx;
    uint32_t qDrop;
    uint32_t parseOk;
    uint32_t parseFail;
    uint32_t parseResync;
    uint32_t disc;
    uint32_t reconn;
    uint32_t loopMaxUs;
    uint32_t bleDrainMaxUs;
    uint32_t dispMaxUs;
    uint32_t freeHeap;
    uint32_t freeDma;                                         // Cached internal 8-bit heap (legacy column)
    uint32_t largestDma;                                      // Cached largest internal 8-bit block (legacy column)
    uint32_t freeDmaCap;                                      // True MALLOC_CAP_DMA free bytes
    uint32_t largestDmaCap;                                   // True MALLOC_CAP_DMA largest free block
    uint32_t dmaFreeMin;                                      // Min MALLOC_CAP_DMA free bytes since session start
    uint32_t dmaLargestMin;                                   // Min MALLOC_CAP_DMA largest block since session start
    uint32_t bleProcessMaxUs;                                 // Window max bleClient.process() duration
    uint32_t touchMaxUs;                                      // Window max touchUiModule.process() duration
    uint32_t wifiMaxUs;                                       // Window max wifiManager.process() duration
    uint32_t uiToScanCount;                                   // Screen transitions to scanning
    uint32_t uiToRestCount;                                   // Screen transitions to resting
    uint32_t uiScanToRestCount;                               // Scanning -> resting transitions
    uint32_t uiFastScanExitCount;                             // Scan dwell below threshold before exit
    uint32_t uiLastScanDwellMs;                               // Most recent scan dwell duration
    uint32_t uiMinScanDwellMs;                                // Session minimum scan dwell duration
    uint32_t fadeDownCount;                                   // Fade-down actions emitted
    uint32_t fadeRestoreCount;                                // Restore actions emitted
    uint32_t fadeSkipEqualCount;                              // Restore skipped (current == baseline)
    uint32_t fadeSkipNoBaselineCount;                         // Restore skipped (missing baseline)
    uint32_t fadeSkipNotFadedCount;                           // Restore skipped (session never faded)
    uint8_t fadeLastDecision;                                 // PerfFadeDecision code
    uint8_t fadeLastCurrentVol;                               // Last observed current volume
    uint8_t fadeLastOriginalVol;                              // Last observed baseline/original volume
    uint32_t fadeLastDecisionMs;                              // Last fade decision timestamp
    uint32_t speedVolDropCount;                               // Speed volume drops
    uint32_t speedVolRestoreCount;                            // Speed volume restores issued
    uint32_t speedVolRetryCount;                              // Speed volume retries
    uint32_t bleScanStartMs;                                  // First scan start timestamp
    uint32_t bleTargetFoundMs;                                // First target-found timestamp
    uint32_t bleConnectStartMs;                               // First connect-start timestamp
    uint32_t bleConnectedMs;                                  // First connected timestamp
    uint32_t bleFirstRxMs;                                    // First parsed/received V1 packet timestamp
    uint32_t bleFollowupRequestAlertMaxUs;                    // Window max connect-burst alert request duration
    uint32_t bleFollowupRequestVersionMaxUs;                  // Window max connect-burst version request duration
    uint32_t bleConnectStableCallbackMaxUs;                   // Window max stable-connect callback duration
    uint32_t bleProxyStartMaxUs;                              // Window max proxy advertising start duration
    uint32_t displayGapRecoverMaxUs;                          // Window max display alert-gap recovery duration
    uint32_t displayFullRenderCount;                          // Session full live renders
    uint32_t displayRestingFullRenderCount;                   // Session full resting renders
    uint32_t displayRestingIncrementalRenderCount;            // Session incremental resting renders
    uint32_t displayPersistedRenderCount;                     // Session persisted renders
    uint32_t displayPreviewRenderCount;                       // Session preview renders
    uint32_t displayRestoreRenderCount;                       // Session restore renders
    uint32_t displayLiveScenarioRenderCount;                  // Session live scenario render count
    uint32_t displayRestingScenarioRenderCount;               // Session resting scenario render count
    uint32_t displayPersistedScenarioRenderCount;             // Session persisted scenario render count
    uint32_t displayPreviewScenarioRenderCount;               // Session preview scenario render count
    uint32_t displayRestoreScenarioRenderCount;               // Session restore scenario render count
    uint32_t displayRestingFlushReasonFullRedrawCount;        // Resting DISPLAY_FLUSH: screen/reset
    uint32_t displayRestingFlushReasonPendingExternalCount;   // Resting DISPLAY_FLUSH: pre-frame draw
    uint32_t displayRestingFlushReasonPaintedCount;           // Resting DISPLAY_FLUSH: leaf repaint
    uint32_t displayRestingFlushReasonCacheHitCount;          // Resting no-flush cache hit
    uint32_t displayPersistedFlushReasonFullRedrawCount;      // Persisted DISPLAY_FLUSH: screen/reset
    uint32_t displayPersistedFlushReasonPendingExternalCount; // Persisted DISPLAY_FLUSH: pre-frame draw
    uint32_t displayPersistedFlushReasonPaintedCount;         // Persisted DISPLAY_FLUSH: leaf repaint
    uint32_t displayPersistedFlushReasonCacheHitCount;        // Persisted no-flush cache hit
    uint32_t displayStatusVolumePaintCount;                   // Status-strip volume cell repaints
    uint32_t displayStatusRssiPaintCount;                     // Status-strip RSSI cell repaints
    uint32_t displayStatusProfilePaintCount;                  // Profile label repaints
    uint32_t displayStatusBatteryPaintCount;                  // Battery icon/percent repaints
    uint32_t displayStatusBleProxyPaintCount;                 // BLE proxy icon repaints
    uint32_t displayStatusWifiPaintCount;                     // Wi-Fi icon repaints
    uint32_t displayStatusObdPaintCount;                      // OBD badge repaints
    uint32_t displayStatusGpsPaintCount;                      // GPS badge repaints
    uint32_t displayStatusAlpPaintCount;                      // ALP badge repaints
    uint32_t displayRedrawReasonFirstRunCount;                // Full redraw first-run triggers
    uint32_t displayRedrawReasonEnterLiveCount;               // Full redraw enter-live triggers
    uint32_t displayRedrawReasonLeaveLiveCount;               // Full redraw leave-live triggers
    uint32_t displayRedrawReasonLeavePersistedCount;          // Full redraw leave-persisted triggers
    uint32_t displayRedrawReasonForceRedrawCount;             // Full redraw force-reset triggers
    uint32_t displayRedrawReasonFrequencyChangeCount;         // Full redraw frequency-change triggers
    uint32_t displayRedrawReasonBandSetChangeCount;           // Full redraw band-set-change triggers
    uint32_t displayRedrawReasonArrowChangeCount;             // Full redraw arrow-change triggers
    uint32_t displayRedrawReasonSignalBarChangeCount;         // Full redraw signal-bar-change triggers
    uint32_t displayRedrawReasonVolumeChangeCount;            // Full redraw volume-change triggers
    uint32_t displayRedrawReasonBogeyCounterChangeCount;      // Full redraw bogey-counter-change triggers
    uint32_t displayRedrawReasonRssiRefreshCount;             // Full redraw RSSI-refresh triggers
    uint32_t displayRedrawReasonFlashTickCount;               // Full redraw flash-tick triggers
    uint32_t displayRedrawReasonFullFlushForRedrawCount;      // Live dispatch: needsFullRedraw
    uint32_t displayRedrawReasonCacheHitSkipFlushCount;       // Live dispatch: union empty
    uint32_t displayRedrawReasonUnionExceedsCapCount;         // Live dispatch: union ≥ cap
    uint32_t displayRedrawReasonPartialRegionFlushCount;      // Live dispatch: flushRegion(union)
    uint32_t displayFullFlushCount;                           // Session full-screen flushes
    uint32_t displayPartialFlushCount;                        // Session region flushes
    uint32_t displayPartialFlushAreaPeakPx;                   // Peak region-flush area
    uint32_t displayPartialFlushAreaTotalPx;                  // Total region-flush pixels
    uint32_t displayFlushEquivalentAreaTotalPx;               // Total pixels flushed (full + partial)
    uint32_t displayFlushMaxAreaPx;                           // Area for the flushMaxUs winner
    uint32_t displayPartialFlushLogicalWidthPeakPx;           // Peak logical partial width
    uint32_t displayPartialFlushLogicalHeightPeakPx;          // Peak logical partial height
    uint32_t displayPartialFlushRowCallsPeak;                 // Peak physical row blits per partial flush
    uint32_t displayPartialFlushPixelsPerRowPeakPx;           // Peak pixels per physical row blit
    uint32_t displayPartialFlushUsPeak;                       // Peak partial-only flush duration
    uint32_t displayPartialFlushWorstUsLogicalWidthPx;        // Logical width for partial time peak
    uint32_t displayPartialFlushWorstUsLogicalHeightPx;       // Logical height for partial time peak
    uint32_t displayPartialFlushWorstUsAreaPx;                // Area for partial time peak
    uint32_t displayPartialFlushWouldFullRows64Count;         // Shadow row-cap=64 decisions
    uint32_t displayPartialFlushWouldFullRows128Count;        // Shadow row-cap=128 decisions
    uint32_t displayPartialFlushWouldFullRows256Count;        // Shadow row-cap=256 decisions
    uint32_t displayUnionExceedsCapAreaPeakPx;                // Peak over-cap dirty union
    uint32_t displayUnionExceedsCapRectCountPeak;             // Peak contributing rect count
    uint32_t displayUnionExceedsCapAreaPeakSourceMask;        // Source mask for area peak
    uint32_t displayUnionExceedsCapWithFrequencyCount;        // Over-cap unions touching frequency
    uint32_t displayUnionExceedsCapWithBandsBarsCount;        // Over-cap unions touching bands/bars
    uint32_t displayUnionExceedsCapWithArrowsCount;           // Over-cap unions touching arrows
    uint32_t displayUnionExceedsCapWithStatusCount;           // Over-cap unions touching status strip
    uint32_t displayUnionExceedsCapWithIndicatorsCount;       // Over-cap unions touching OBD/GPS/ALP
    uint32_t displayUnionExceedsCapWithExternalCount;         // Over-cap unions including pending external paint
    uint32_t displayUnionExceedsCapUnclassifiedCount;         // Over-cap unions with unknown source
    uint32_t displayBaseFrameMaxUs;                           // Window max base-frame stage
    uint32_t displayStatusStripMaxUs;                         // Window max status-strip stage
    uint32_t displayFrequencyMaxUs;                           // Window max frequency stage
    uint32_t displayBandsBarsMaxUs;                           // Window max bands+bars stage
    uint32_t displayArrowsIconsMaxUs;                         // Window max arrows+icons stage
    uint32_t displayFlushSubphaseMaxUs;                       // Window max inner render flush stage
    uint32_t displayLiveRenderMaxUs;                          // Window max live render
    uint32_t displayRestingRenderMaxUs;                       // Window max resting render
    uint32_t displayPersistedRenderMaxUs;                     // Window max persisted render
    uint32_t displayPreviewRenderMaxUs;                       // Window max preview render
    uint32_t displayRestoreRenderMaxUs;                       // Window max restore render
    uint32_t displayPreviewFirstRenderMaxUs;                  // Window max first preview render
    uint32_t displayPreviewSteadyRenderMaxUs;                 // Window max later preview render
    uint32_t alertPersistStarts;                              // Persisted-alert sessions started
    uint32_t alertPersistStartsSkippedActive;                 // startPersistence() no-op while window already active
    uint32_t alertPersistStartsSkippedInvalid;                // startPersistence() refused — no valid alert latched
    uint32_t alertPersistExpires;                             // Persisted-alert windows expired naturally
    uint32_t alertPersistClears;                              // Persisted-alert state cleared explicitly
    uint32_t autoPushStarts;                                  // Auto-push runs initiated
    uint32_t autoPushCompletes;                               // Auto-push runs completed
    uint32_t autoPushNoProfile;                               // Auto-push slot had no configured profile
    uint32_t autoPushProfileLoadFail;                         // Auto-push profile load failures
    uint32_t autoPushProfileWriteFail;                        // Auto-push profile write exhausted retries
    uint32_t autoPushBusyRetries;                             // Auto-push write-busy retries
    uint32_t autoPushModeFail;                                // Auto-push mode set failures
    uint32_t autoPushVolumeFail;                              // Auto-push volume set failures
    uint32_t autoPushDisconnectAbort;                         // Auto-push aborted due to disconnect
    uint32_t powerAutoPowerArmed;                             // Auto power-off armed on first V1 data
    uint32_t powerAutoPowerTimerStart;                        // Auto power-off timer started
    uint32_t powerAutoPowerTimerCancel;                       // Auto power-off timer cancelled on reconnect
    uint32_t powerAutoPowerTimerExpire;                       // Auto power-off timer expired
    uint32_t powerCarModeAlpSilenceExpire;                    // Car-mode ALP UART silence shutdown fired
    uint32_t powerCriticalWarn;                               // Critical-battery warning shown
    uint32_t powerCriticalShutdown;                           // Critical-battery shutdown triggered
    uint32_t perfUncleanShutdown; // Boot saw cleanShutdn=false (previous run died uncleanly)
    uint32_t cmdBleBusy;          // BLE command write transient failures/retries

    // CSV schema v6 additions (kept at tail for backwards column stability)
    uint32_t rxBytes;
    uint32_t oversizeDrops;
    uint32_t queueHighWater;
    uint32_t bleMutexSkip;
    uint32_t bleMutexTimeout;
    uint32_t cmdPaceNotYet;
    uint32_t bleDiscTaskCreateFail;
    uint32_t displayUpdates;
    uint32_t displaySkips;
    uint32_t wifiConnectDeferred;
    uint32_t pushNowRetries;
    uint32_t pushNowFailures;
    uint32_t minLargestBlock;
    uint32_t fsMaxUs;
    uint32_t sdMaxUs;
    uint32_t sdWriteCount;
    uint32_t sdWriteLt1msCount;
    uint32_t sdWrite1to5msCount;
    uint32_t sdWrite5to10msCount;
    uint32_t sdWriteGe10msCount;
    uint32_t flushMaxUs;
    uint32_t bleConnectMaxUs;
    uint32_t bleDiscoveryMaxUs;
    uint32_t bleSubscribeMaxUs;
    uint32_t dispPipeMaxUs;
    uint32_t perfReportMaxUs;                     // Window max perfMetricsCheckReport snapshot+enqueue
    uint32_t prioritySelectRowFlag;               // Priority chosen from alert-row isPriority bit
    uint32_t prioritySelectFirstUsable;           // Priority chosen from first usable alert fallback
    uint32_t prioritySelectFirstEntry;            // Priority fell back to entry 0 (last resort)
    uint32_t prioritySelectAmbiguousIndex;        // Alert table complete under both 0-based and 1-based mapping
    uint32_t prioritySelectUnusableIndex;         // Row-priority candidate present but unusable
    uint32_t prioritySelectInvalidChosen;         // Final chosen alert invalid/zero-freq non-laser
    uint32_t alertTablePublishes;                 // Complete alert tables published
    uint32_t alertTablePublishes3Bogey;           // Complete tables published with count=3
    uint32_t alertTableRowReplacements;           // Duplicate row-index replacements
    uint32_t alertTableAssemblyTimeouts;          // Partial table assemblies dropped on timeout
    uint32_t parserRowsBandNone;                  // Alert rows decoded with BAND_NONE
    uint32_t parserRowsKuRaw;                     // Alert rows containing Ku raw bit (0x10)
    uint32_t displayLiveInvalidPrioritySkips;     // Live display invalid-priority early returns
    uint32_t displayLiveFallbackToUsable;         // Live display fallback-to-usable selections
    uint32_t obdMaxUs;                            // Window max obdRuntimeModule.update() duration
    uint32_t obdConnectCallMaxUs;                 // Window max inline OBD BLE connect() duration
    uint32_t obdSecurityStartCallMaxUs;           // Window max inline OBD BLE security-start duration
    uint32_t obdDiscoveryCallMaxUs;               // Window max inline OBD BLE discovery duration
    uint32_t obdSubscribeCallMaxUs;               // Window max inline OBD BLE subscribe duration
    uint32_t obdWriteCallMaxUs;                   // Window max inline OBD BLE command write duration
    uint32_t obdRssiCallMaxUs;                    // Window max inline OBD BLE RSSI read duration
    uint32_t obdPollErrors;                       // OBD poll errors this window
    uint32_t obdStaleCount;                       // OBD stale speed readings this window
    uint32_t perfDrop;                            // Perf snapshot drops since session start
    uint32_t eventBusDrops;                       // System event-bus drops since session start
    uint32_t wifiHandleClientMaxUs;               // Window max HTTP client servicing duration
    uint32_t wifiMaintenanceMaxUs;                // Window max WiFi maintenance duration
    uint32_t wifiStatusCheckMaxUs;                // Window max STA status check duration
    uint32_t wifiTimeoutCheckMaxUs;               // Window max auto-timeout check duration
    uint32_t wifiHeapGuardMaxUs;                  // Window max WiFi heap guard duration
    uint32_t wifiApStaPollMaxUs;                  // Window max AP station polling duration
    uint32_t wifiStopHttpServerMaxUs;             // Window max HTTP stop duration
    uint32_t wifiStopStaDisconnectMaxUs;          // Window max STA disconnect duration
    uint32_t wifiStopApDisableMaxUs;              // Window max AP disable duration
    uint32_t wifiStopModeOffMaxUs;                // Window max radio-off duration
    uint32_t wifiStartPreflightMaxUs;             // Window max WiFi start preflight duration
    uint32_t wifiStartApBringupMaxUs;             // Window max AP bring-up duration
    uint32_t freeDmaMin;                          // Min cached internal 8-bit heap free bytes since session start
    uint32_t largestDmaMin;                       // Min cached internal 8-bit largest block since session start
    uint8_t bleState;                             // BLE runtime state code
    uint8_t subscribeStep;                        // BLE subscribe-step machine code
    uint8_t connectInProgress;                    // BLE connect attempt active
    uint8_t asyncConnectPending;                  // BLE async connect callback pending
    uint8_t pendingDisconnectCleanup;             // BLE deferred disconnect cleanup pending
    uint8_t proxyAdvertising;                     // Proxy advertising currently active
    uint8_t proxyAdvertisingLastTransitionReason; // PerfProxyAdvertisingTransitionReason code
    uint8_t wifiPriorityMode;                     // BLE WiFi-priority suppression active
    uint8_t speedSourceSelected;                  // SpeedSource code for selected speed source
    uint8_t speedSourceValid;                     // Selected speed sample valid/fresh
    uint32_t speedSelectedMph_x10;                // Selected speed mph * 10 when valid
    uint32_t speedSelectedAgeMs;                  // Selected speed age when valid; UINT32_MAX otherwise
    uint32_t speedSourceSwitches;                 // Speed source switches since session start
    uint32_t speedNoSourceSelections;             // No-source selections since session start
    uint32_t speedGpsSelections;                  // GPS selections since session start
    uint8_t cycleState;                           // Connection cycle state code
    uint32_t cycleTransitionsTotal;               // Connection cycle transitions since boot/reset
    uint32_t cycleTimeInStateMs;                  // Current connection cycle dwell time
    uint32_t cycleTeardownDurationMs;             // Last completed teardown duration
    uint32_t cycleObdRetryAttemptsTotal;          // Coordinator-observed OBD retry attempts
    uint32_t cycleWifiManualPhoneKicksTotal;      // Proxy phones disconnected for manual WiFi
    uint8_t cycleProxyNoClientLatched;            // Legacy field; proxy now downshifts cadence

    // GPS observability (schema v37 — appended, never inserted)
    uint64_t utcEpochMs;               // UTC at snapshot time (0 = no GPS fix)
    bool utcValid;                     // true if GPS publisher provided fresh UTC
    uint32_t gpsSentencesOk;           // Sentences parsed successfully
    uint32_t gpsSentencesChecksumFail; // Sentences rejected on checksum
    uint32_t gpsSentencesUnknown;      // Sentences not RMC/GGA (not an error)
    uint32_t gpsBufferOverruns;        // NMEA line buffer overruns
    uint32_t gpsBytesIn;               // Total UART bytes ingested
    uint32_t gpsFirstFixMs;            // millis() at first stable fix (0 = not yet)
    uint32_t gpsLastSentenceAgeMs;     // ms since last NMEA sentence (UINT32_MAX = none)
    uint32_t gpsFixAgeMs;              // ms since last fix (UINT32_MAX = no fix)
    uint32_t gpsStableFixAgeMs;        // ms since stable fix (UINT32_MAX = no fix)
    uint8_t gpsSatellitesInUse;        // Satellites in current fix
    uint16_t gpsHdopX10;               // HDOP * 10 (e.g. 12 = 1.2; UINT16_MAX = NaN)
    bool gpsHasFix;                    // GPS has active fix
    bool gpsStableHasFix;              // GPS has stable fix (with hysteresis)
    uint32_t gpsEnableTransitions;     // Number of enable/disable transitions

    // Notify-to-display end-to-end latency histogram (schema v38 — appended)
    uint32_t notifyToDisplayMaxMs;      // Window max notify→display latency in ms
    uint32_t notifyToDisplayTotalCount; // Window total samples recorded
};

enum class PerfRuntimeSnapshotMode : uint8_t {
    PreserveWindowPeaks = 0,
    CaptureAndResetWindowPeaks = 1,
};

struct PerfRuntimeWifiAutoStartSnapshot {
    const char* gate = "unknown";
    uint8_t gateCode = 0;
    bool enableWifi = false;
    bool bleConnected = false;
    uint32_t v1ConnectedAtMs = 0;
    uint32_t msSinceV1Connect = 0;
    uint32_t settleMs = 0;
    uint32_t bootTimeoutMs = 0;
    bool canStartDma = false;
    bool wifiAutoStartDone = false;
    bool bleSettled = false;
    bool bootTimeoutReached = false;
    bool shouldAutoStart = false;
    bool startTriggered = false;
    bool startSucceeded = false;
};

struct PerfRuntimeSettingsPersistenceSnapshot {
    uint32_t backupRevision = 0;
    bool deferredBackupPending = false;
    bool deferredBackupRetryScheduled = false;
    bool deferredBackupHasNextAttempt = false;
    uint32_t deferredBackupNextAttemptAtMs = 0;
    uint32_t deferredBackupDelayMs = 0;
    bool perfLoggingEnabled = false;
    const char* perfLoggingPath = "";
};

struct PerfRuntimeSpeedSourceSnapshot {
    const char* selected = "none";
    bool selectedValueValid = false;
    float selectedMph = 0.0f;
    uint32_t selectedAgeMs = 0;
    uint32_t sourceSwitches = 0;
    uint32_t gpsSelections = 0;
    uint32_t noSourceSelections = 0;
};

struct PerfRuntimeHeapSnapshot {
    uint32_t heapFree = 0;
    uint32_t heapMinFree = 0;
    uint32_t heapLargest = 0;
    uint32_t heapInternalFree = 0;
    uint32_t heapInternalFreeMin = 0;
    uint32_t heapInternalLargest = 0;
    uint32_t heapInternalLargestMin = 0;
    uint32_t heapDmaFree = 0;
    uint32_t heapDmaFreeMin = 0;
    uint32_t heapDmaLargest = 0;
    uint32_t heapDmaLargestMin = 0;
};

struct PerfRuntimePsramSnapshot {
    uint32_t total = 0;
    uint32_t free = 0;
    uint32_t largest = 0;
};

struct PerfRuntimeSdContentionSnapshot {
    uint32_t tryLockFails = 0;
    uint32_t dmaStarvation = 0;
};

struct PerfRuntimeProxySnapshot {
    uint32_t sendCount = 0;
    uint32_t dropCount = 0;
    uint32_t errorCount = 0;
    uint32_t queueHighWater = 0;
    bool connected = false;
    bool advertising = false;
    uint32_t advertisingOnTransitions = 0;
    uint32_t advertisingOffTransitions = 0;
    uint32_t advertisingLastTransitionMs = 0;
    uint32_t advertisingLastTransitionReasonCode = 0;
    const char* advertisingLastTransitionReason = "unknown";
};

struct PerfRuntimeEventBusSnapshot {
    uint32_t publishCount = 0;
    uint32_t dropCount = 0;
    uint32_t size = 0;
};

struct PerfRuntimeConnectionCycleSnapshot {
    const char* state = "unknown";
    uint8_t stateCode = 0;
    uint32_t transitionsTotal = 0;
    uint32_t timeInStateMs = 0;
    uint32_t teardownDurationMs = 0;
    uint32_t obdRetryAttemptsTotal = 0;
    uint32_t wifiManualPhoneKicksTotal = 0;
    bool proxyNoClientLatched = false;
};

struct PhoneCmdDropMetricsSnapshot {
    uint32_t overflow = 0;
    uint32_t invalid = 0;
    uint32_t bleFail = 0;
    uint32_t lockBusy = 0;
};

struct PerfRuntimeMetricsSnapshot {
    PerfSdSnapshot flat;
    PhoneCmdDropMetricsSnapshot phoneCmdDrops;
    uint32_t uptimeMs = 0;
    uint32_t connectionDispatchRuns = 0;
    uint32_t connectionCadenceDisplayDue = 0;
    uint32_t connectionCadenceHoldScanDwell = 0;
    uint32_t connectionStateProcessRuns = 0;
    uint32_t connectionStateWatchdogForces = 0;
    uint32_t connectionStateProcessGapMaxMs = 0;
    uint32_t bleScanStateEntries = 0;
    uint32_t bleScanStateExits = 0;
    uint32_t bleScanTargetFound = 0;
    uint32_t bleScanNoTargetExits = 0;
    uint32_t bleScanDwellMaxMs = 0;
    uint32_t uuid128FallbackHits = 0;
    uint32_t wifiStopGraceful = 0;
    uint32_t wifiStopImmediate = 0;
    uint32_t wifiStopManual = 0;
    uint32_t wifiStopTimeout = 0;
    uint32_t wifiStopNoClients = 0;
    uint32_t wifiStopNoClientsAuto = 0;
    uint32_t wifiStopLowDma = 0;
    uint32_t wifiStopPoweroff = 0;
    uint32_t wifiStopOther = 0;
    uint32_t wifiApDropLowDma = 0;
    uint32_t wifiApDropIdleSta = 0;
    uint32_t wifiApUpTransitions = 0;
    uint32_t wifiApDownTransitions = 0;
    uint32_t wifiProcessMaxUs = 0;
    const char* bleState = "unknown";
    uint8_t bleStateCode = 0;
    const char* subscribeStep = "unknown";
    uint8_t subscribeStepCode = 0;
    bool connectInProgress = false;
    bool asyncConnectPending = false;
    bool pendingDisconnectCleanup = false;
    bool proxyAdvertising = false;
    uint32_t proxyAdvertisingOnTransitions = 0;
    uint32_t proxyAdvertisingOffTransitions = 0;
    uint32_t proxyAdvertisingLastTransitionMs = 0;
    uint32_t proxyAdvertisingLastTransitionReasonCode = 0;
    const char* proxyAdvertisingLastTransitionReason = "unknown";
    bool wifiPriorityMode = false;
    uint32_t loopMaxPrevWindowUs = 0;
    uint32_t wifiMaxPrevWindowUs = 0;
    uint32_t bleProcessMaxPrevWindowUs = 0;
    uint32_t dispPipeMaxPrevWindowUs = 0;
    uint32_t wifiApActive = 0;
    uint32_t wifiApLastTransitionMs = 0;
    uint32_t wifiApLastTransitionReasonCode = 0;
    const char* wifiApLastTransitionReason = "unknown";
    uint32_t perfSdLockFail = 0;
    uint32_t perfSdDirFail = 0;
    uint32_t perfSdOpenFail = 0;
    uint32_t perfSdHeaderFail = 0;
    uint32_t perfSdMarkerFail = 0;
    uint32_t perfSdWriteFail = 0;
    bool monitoringEnabled = false;
    bool metricsEnabled = true;
    bool debugEnabled = false;
    uint32_t latencyMinUs = 0;
    uint32_t latencyAvgUs = 0;
    uint32_t latencyMaxUs = 0;
    uint32_t latencySamples = 0;
    PerfRuntimeWifiAutoStartSnapshot wifiAutoStart;
    PerfRuntimeSettingsPersistenceSnapshot settingsPersistence;
    PerfRuntimeSpeedSourceSnapshot speedSource;
    PerfRuntimeHeapSnapshot heap;
    PerfRuntimePsramSnapshot psram;
    PerfRuntimeSdContentionSnapshot sdContention;
    PerfRuntimeProxySnapshot proxy;
    PerfRuntimeEventBusSnapshot eventBus;
    PerfRuntimeConnectionCycleSnapshot connectionCycle;
};

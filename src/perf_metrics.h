/**
 * Low-Overhead Performance Metrics (Channel A: flight recorder)
 *
 * TWO-CHANNEL LOGGING ARCHITECTURE:
 * - Channel A (this file): Always-on numeric counters. RED ZONE SAFE.
 *   Counters only, no strings, no heap, no locks, no I/O.
 *   Emitted periodically from safe zone (once per second max).
 *
 * RED ZONE SAFE MACROS (use these everywhere):
 *   PERF_INC(counter)        - Atomic increment, zero overhead
 *   PERF_MAX(counter, value) - Atomic max update, zero overhead
 *
 * Design principles:
 * - No heap allocations
 * - No logging in hot paths
 * - Counters/timestamps stored in RAM (std::atomic)
 * - Sampled timing (1/N packets) to reduce overhead
 * - Compile-time gating via PERF_METRICS for extended stats
 *
 * Usage:
 * - PERF_METRICS=1: Project production env; bench validates this exact image.
 * - PERF_METRICS=0: Local/experimental minimal mode unless CI/release move too.
 */

#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <atomic>

// ============================================================================
// Compile-time gating
// The waveshare-349 production env enables PERF_METRICS so release artifacts
// remain the same firmware image exercised by CI and bench evidence.
// PERF_METRICS=0 is only for local/experimental minimal builds unless the
// production test/release path is moved with it.
// ============================================================================
#ifndef PERF_METRICS
#define PERF_METRICS 0  // Default for ad-hoc builds without platformio.ini flags
#endif

// Compile-time toggles for monitoring and verbose alerts
#ifndef PERF_MONITORING
#define PERF_MONITORING 1  // Disable to keep counters only (no sampling/prints)
#endif

#ifndef PERF_VERBOSE
#define PERF_VERBOSE 0  // Enable to allow immediate alerts and stage timings
#endif

// Sampling rate: measure 1 in N packets to reduce overhead
#ifndef PERF_SAMPLE_RATE
#define PERF_SAMPLE_RATE 8  // Measure every 8th packet
#endif

// Threshold for alert (print immediately if exceeded)
#ifndef PERF_LATENCY_ALERT_MS
#define PERF_LATENCY_ALERT_MS 100  // Alert if latency > 100ms
#endif

// ============================================================================
// Always-on counters (zero overhead when not accessed)
// Uses std::atomic for thread-safe access from main loop and web handlers
// ============================================================================
struct PerfCounters {
    // Packet flow
    std::atomic<uint32_t> rxPackets{0};        // Total BLE notifications received
    std::atomic<uint32_t> rxBytes{0};          // Total bytes received
    std::atomic<uint32_t> queueDrops{0};       // Packets dropped (queue full)
    std::atomic<uint32_t> oversizeDrops{0};    // Packets dropped (too large for buffer)
    std::atomic<uint32_t> queueHighWater{0};   // Max queue depth seen
    std::atomic<uint32_t> proxyQueueHighWater{0}; // Max proxy queue depth
    std::atomic<uint32_t> phoneCmdQueueHighWater{0}; // Max phone→V1 cmd queue depth
    std::atomic<uint32_t> phoneCmdDropsOverflow{0}; // Phone→V1 queue overflow drops
    std::atomic<uint32_t> phoneCmdDropsInvalid{0}; // Phone→V1 malformed packet drops
    std::atomic<uint32_t> phoneCmdDropsBleFail{0}; // Phone→V1 hard BLE send failures
    std::atomic<uint32_t> phoneCmdDropsLockBusy{0}; // Phone→V1 queue lock-busy drops
    std::atomic<uint32_t> parseSuccesses{0};   // Successfully parsed packets
    std::atomic<uint32_t> parseFailures{0};    // Parse failures (resync)
    std::atomic<uint32_t> parseResyncs{0};     // Framing-level resyncs (bad length/size/end marker)
    std::atomic<uint32_t> perfDrop{0};         // Perf SD snapshot drops (queue full)
    std::atomic<uint32_t> perfSdLockFail{0};   // Perf SD writer lock failures
    std::atomic<uint32_t> perfSdDirFail{0};    // Perf SD dir ensure failures
    std::atomic<uint32_t> perfSdOpenFail{0};   // Perf SD file open failures
    std::atomic<uint32_t> perfSdHeaderFail{0}; // Perf SD CSV header write failures
    std::atomic<uint32_t> perfSdMarkerFail{0}; // Perf SD session marker write failures
    std::atomic<uint32_t> perfSdWriteFail{0};  // Perf SD data line write failures

    // Connection
    std::atomic<uint32_t> reconnects{0};       // BLE reconnection count
    std::atomic<uint32_t> disconnects{0};      // BLE disconnection count
    std::atomic<uint32_t> connectionDispatchRuns{0}; // connection-state dispatch passes
    std::atomic<uint32_t> connectionCadenceDisplayDue{0}; // cadence allowed display/update tick
    std::atomic<uint32_t> connectionCadenceHoldScanDwell{0}; // scan dwell hold suppressed process
    std::atomic<uint32_t> connectionStateProcessRuns{0}; // connectionStateModule.process() calls
    std::atomic<uint32_t> connectionStateWatchdogForces{0}; // watchdog-forced process calls
    std::atomic<uint32_t> connectionStateProcessGapMaxMs{0}; // max observed gap between process runs
    std::atomic<uint32_t> bleScanStateEntries{0}; // transitions into SCANNING
    std::atomic<uint32_t> bleScanStateExits{0}; // transitions out of SCANNING
    std::atomic<uint32_t> bleScanTargetFound{0}; // SCANNING->SCAN_STOPPING due to target found
    std::atomic<uint32_t> bleScanNoTargetExits{0}; // SCANNING->DISCONNECTED without target
    std::atomic<uint32_t> bleScanDwellMaxMs{0}; // max SCANNING state dwell duration

    // Display
    std::atomic<uint32_t> displayUpdates{0};   // Frames drawn
    std::atomic<uint32_t> displaySkips{0};     // Updates skipped (throttled)

    // Mutex contention monitoring (should stay low/zero in normal operation)
    std::atomic<uint32_t> bleMutexSkip{0};        // HOT path try-lock skips
    std::atomic<uint32_t> bleMutexTimeout{0};     // COLD path timeout failures
    std::atomic<uint32_t> cmdPaceNotYet{0};       // sendCommand pacing deferrals
    std::atomic<uint32_t> cmdBleBusy{0};          // sendCommand BLE write failed (transient)
    std::atomic<uint32_t> uuid128FallbackHits{0}; // 128-bit custom UUID fast extraction path hits
    std::atomic<uint32_t> bleDiscTaskCreateFail{0}; // Discovery task spawn failures
    std::atomic<uint32_t> wifiConnectDeferred{0}; // WiFi connects staged via non-blocking phase machine
    std::atomic<uint32_t> wifiStopGraceful{0}; // graceful staged WiFi stop requests
    std::atomic<uint32_t> wifiStopImmediate{0}; // immediate WiFi stop requests
    std::atomic<uint32_t> wifiStopManual{0}; // stop requests flagged manual
    std::atomic<uint32_t> wifiStopTimeout{0}; // stop reason timeout
    std::atomic<uint32_t> wifiStopNoClients{0}; // stop reason no_clients
    std::atomic<uint32_t> wifiStopNoClientsAuto{0}; // stop reason no_clients_auto
    std::atomic<uint32_t> wifiStopLowDma{0}; // stop reason low_dma
    std::atomic<uint32_t> wifiStopPoweroff{0}; // stop reason poweroff
    std::atomic<uint32_t> wifiStopOther{0}; // stop reasons not covered above
    std::atomic<uint32_t> wifiApDropLowDma{0}; // AP retired due to sustained low SRAM in AP+STA
    std::atomic<uint32_t> wifiApDropIdleSta{0}; // AP retired while STA remained connected
    std::atomic<uint32_t> wifiApUpTransitions{0}; // AP transition marker: down->up
    std::atomic<uint32_t> wifiApDownTransitions{0}; // AP transition marker: up->down
    std::atomic<uint32_t> wifiApState{0}; // AP state marker (1=up, 0=down)
    std::atomic<uint32_t> wifiApLastTransitionMs{0}; // millis() when AP state last changed
    std::atomic<uint32_t> wifiApLastTransitionReason{0}; // PerfWifiApTransitionReason
    std::atomic<uint32_t> wifiProcessMaxUs{0}; // max WiFiManager::process() duration
    std::atomic<uint32_t> wifiHandleClientMaxUs{0}; // max server.handleClient() duration inside WiFi process
    std::atomic<uint32_t> wifiMaintenanceMaxUs{0}; // max maintenance block duration inside WiFi process
    std::atomic<uint32_t> wifiStatusCheckMaxUs{0}; // max checkWifiClientStatus() duration
    std::atomic<uint32_t> wifiTimeoutCheckMaxUs{0}; // max checkAutoTimeout() duration
    std::atomic<uint32_t> wifiHeapGuardMaxUs{0}; // max heap guard sampling/evaluation duration
    std::atomic<uint32_t> wifiApStaPollMaxUs{0}; // max softAP station poll duration
    std::atomic<uint32_t> wifiStopHttpServerMaxUs{0}; // max staged/immediate HTTP stop duration
    std::atomic<uint32_t> wifiStopStaDisconnectMaxUs{0}; // max staged/immediate STA disconnect duration
    std::atomic<uint32_t> wifiStopApDisableMaxUs{0}; // max staged/immediate AP disable duration
    std::atomic<uint32_t> wifiStopModeOffMaxUs{0}; // max staged/immediate radio-off duration
    std::atomic<uint32_t> wifiStartPreflightMaxUs{0}; // max setup-mode start preflight duration
    std::atomic<uint32_t> wifiStartApBringupMaxUs{0}; // max AP bring-up/server-start duration
    std::atomic<uint32_t> proxyAdvertisingOnTransitions{0}; // proxy advertising transition marker: off->on
    std::atomic<uint32_t> proxyAdvertisingOffTransitions{0}; // proxy advertising transition marker: on->off
    std::atomic<uint32_t> proxyAdvertisingState{0}; // proxy advertising state marker (1=on, 0=off)
    std::atomic<uint32_t> proxyAdvertisingLastTransitionMs{0}; // millis() when proxy advertising state last changed
    std::atomic<uint32_t> proxyAdvertisingLastTransitionReason{0}; // PerfProxyAdvertisingTransitionReason
    std::atomic<uint32_t> pushNowRetries{0};      // Non-blocking Push Now retry attempts
    std::atomic<uint32_t> pushNowFailures{0};     // Non-blocking Push Now exhausted retries
    std::atomic<uint32_t> alertPersistStarts{0};  // Persisted-alert sessions started
    std::atomic<uint32_t> alertPersistStartsSkippedActive{0};  // startPersistence() called while a window was already active (expected idempotent no-op)
    std::atomic<uint32_t> alertPersistStartsSkippedInvalid{0}; // startPersistence() called with no valid alert latched (caller-side invariant violation; should remain 0)
    std::atomic<uint32_t> alertPersistExpires{0}; // Persisted-alert windows expired naturally
    std::atomic<uint32_t> alertPersistClears{0};  // Persisted-alert state cleared explicitly
    std::atomic<uint32_t> autoPushStarts{0};      // Auto-push runs initiated
    std::atomic<uint32_t> autoPushCompletes{0};   // Auto-push runs completed
    std::atomic<uint32_t> autoPushNoProfile{0};   // Auto-push slot had no configured profile
    std::atomic<uint32_t> autoPushProfileLoadFail{0}; // Auto-push profile load failures
    std::atomic<uint32_t> autoPushProfileWriteFail{0}; // Auto-push profile write exhausted retries
    std::atomic<uint32_t> autoPushBusyRetries{0}; // Auto-push write-busy retries
    std::atomic<uint32_t> autoPushModeFail{0};    // Auto-push mode set failures
    std::atomic<uint32_t> autoPushVolumeFail{0};  // Auto-push volume set failures
    std::atomic<uint32_t> autoPushDisconnectAbort{0}; // Auto-push aborted due to disconnect
    std::atomic<uint32_t> prioritySelectRowFlag{0};      // Priority chosen from alert-row isPriority bit
    std::atomic<uint32_t> prioritySelectFirstUsable{0};  // Priority chosen from first usable alert fallback
    std::atomic<uint32_t> prioritySelectFirstEntry{0};   // Priority fell back to entry 0 (last resort)
    std::atomic<uint32_t> prioritySelectAmbiguousIndex{0}; // Alert table completed as both 0-based and 1-based
    std::atomic<uint32_t> prioritySelectUnusableIndex{0};  // Row-priority candidate existed but was unusable
    std::atomic<uint32_t> prioritySelectInvalidChosen{0};  // Chosen alert invalid/zero-freq non-laser
    std::atomic<uint32_t> alertTablePublishes{0};          // Complete alert tables published
    std::atomic<uint32_t> alertTablePublishes3Bogey{0};    // Complete tables published with count=3
    std::atomic<uint32_t> alertTableRowReplacements{0};    // Duplicate row index replacements
    std::atomic<uint32_t> alertTableAssemblyTimeouts{0};   // Partial table assemblies dropped on timeout
    std::atomic<uint32_t> parserRowsBandNone{0};           // Alert rows decoded with BAND_NONE
    std::atomic<uint32_t> parserRowsKuRaw{0};              // Alert rows with Ku raw band bit (0x10)
    std::atomic<uint32_t> displayLiveInvalidPrioritySkips{0}; // Live display update early-returned invalid priority
    std::atomic<uint32_t> displayLiveFallbackToUsable{0};  // Live display used fallback usable alert
    std::atomic<uint32_t> voiceAnnouncePriority{0};        // Voice priority announcements emitted
    std::atomic<uint32_t> voiceAnnounceDirection{0};       // Voice direction/bogey announcements emitted
    std::atomic<uint32_t> voiceAnnounceSecondary{0};       // Voice secondary announcements emitted
    std::atomic<uint32_t> voiceAnnounceEscalation{0};      // Voice escalation announcements emitted
    std::atomic<uint32_t> voiceDirectionThrottled{0};      // Voice direction announcements suppressed by throttle
    std::atomic<uint32_t> powerAutoPowerArmed{0};    // Auto power-off armed on first V1 data
    std::atomic<uint32_t> powerAutoPowerTimerStart{0}; // Auto power-off timer started
    std::atomic<uint32_t> powerAutoPowerTimerCancel{0}; // Auto power-off timer cancelled on reconnect
    std::atomic<uint32_t> powerAutoPowerTimerExpire{0}; // Auto power-off timer expired
    std::atomic<uint32_t> powerCarModeAlpSilenceExpire{0}; // Car-mode ALP UART silence shutdown fired
    std::atomic<uint32_t> powerCriticalWarn{0};      // Critical-battery warning shown
    std::atomic<uint32_t> powerCriticalShutdown{0};  // Critical-battery shutdown triggered
    std::atomic<uint32_t> perfUncleanShutdown{0};    // Boot saw cleanShutdn=false (previous run died uncleanly)
    std::atomic<uint32_t> audioPlayCount{0};          // Audio play tasks successfully started
    std::atomic<uint32_t> audioPlayBusy{0};           // Audio plays rejected because playback is active
    std::atomic<uint32_t> audioTaskFail{0};           // Audio task creation failures

    void reset() {
        rxPackets.store(0, std::memory_order_relaxed);
        rxBytes.store(0, std::memory_order_relaxed);
        queueDrops.store(0, std::memory_order_relaxed);
        oversizeDrops.store(0, std::memory_order_relaxed);
        queueHighWater.store(0, std::memory_order_relaxed);
        proxyQueueHighWater.store(0, std::memory_order_relaxed);
        phoneCmdQueueHighWater.store(0, std::memory_order_relaxed);
        phoneCmdDropsOverflow.store(0, std::memory_order_relaxed);
        phoneCmdDropsInvalid.store(0, std::memory_order_relaxed);
        phoneCmdDropsBleFail.store(0, std::memory_order_relaxed);
        phoneCmdDropsLockBusy.store(0, std::memory_order_relaxed);
        parseSuccesses.store(0, std::memory_order_relaxed);
        parseFailures.store(0, std::memory_order_relaxed);
        parseResyncs.store(0, std::memory_order_relaxed);
        perfDrop.store(0, std::memory_order_relaxed);
        perfSdLockFail.store(0, std::memory_order_relaxed);
        perfSdDirFail.store(0, std::memory_order_relaxed);
        perfSdOpenFail.store(0, std::memory_order_relaxed);
        perfSdHeaderFail.store(0, std::memory_order_relaxed);
        perfSdMarkerFail.store(0, std::memory_order_relaxed);
        perfSdWriteFail.store(0, std::memory_order_relaxed);
        reconnects.store(0, std::memory_order_relaxed);
        disconnects.store(0, std::memory_order_relaxed);
        connectionDispatchRuns.store(0, std::memory_order_relaxed);
        connectionCadenceDisplayDue.store(0, std::memory_order_relaxed);
        connectionCadenceHoldScanDwell.store(0, std::memory_order_relaxed);
        connectionStateProcessRuns.store(0, std::memory_order_relaxed);
        connectionStateWatchdogForces.store(0, std::memory_order_relaxed);
        connectionStateProcessGapMaxMs.store(0, std::memory_order_relaxed);
        bleScanStateEntries.store(0, std::memory_order_relaxed);
        bleScanStateExits.store(0, std::memory_order_relaxed);
        bleScanTargetFound.store(0, std::memory_order_relaxed);
        bleScanNoTargetExits.store(0, std::memory_order_relaxed);
        bleScanDwellMaxMs.store(0, std::memory_order_relaxed);
        displayUpdates.store(0, std::memory_order_relaxed);
        displaySkips.store(0, std::memory_order_relaxed);
        bleMutexSkip.store(0, std::memory_order_relaxed);
        bleMutexTimeout.store(0, std::memory_order_relaxed);
        cmdPaceNotYet.store(0, std::memory_order_relaxed);
        cmdBleBusy.store(0, std::memory_order_relaxed);
        uuid128FallbackHits.store(0, std::memory_order_relaxed);
        bleDiscTaskCreateFail.store(0, std::memory_order_relaxed);
        wifiConnectDeferred.store(0, std::memory_order_relaxed);
        wifiStopGraceful.store(0, std::memory_order_relaxed);
        wifiStopImmediate.store(0, std::memory_order_relaxed);
        wifiStopManual.store(0, std::memory_order_relaxed);
        wifiStopTimeout.store(0, std::memory_order_relaxed);
        wifiStopNoClients.store(0, std::memory_order_relaxed);
        wifiStopNoClientsAuto.store(0, std::memory_order_relaxed);
        wifiStopLowDma.store(0, std::memory_order_relaxed);
        wifiStopPoweroff.store(0, std::memory_order_relaxed);
        wifiStopOther.store(0, std::memory_order_relaxed);
        wifiApDropLowDma.store(0, std::memory_order_relaxed);
        wifiApDropIdleSta.store(0, std::memory_order_relaxed);
        wifiApUpTransitions.store(0, std::memory_order_relaxed);
        wifiApDownTransitions.store(0, std::memory_order_relaxed);
        wifiApState.store(0, std::memory_order_relaxed);
        wifiApLastTransitionMs.store(0, std::memory_order_relaxed);
        wifiApLastTransitionReason.store(0, std::memory_order_relaxed);
        wifiProcessMaxUs.store(0, std::memory_order_relaxed);
        wifiHandleClientMaxUs.store(0, std::memory_order_relaxed);
        wifiMaintenanceMaxUs.store(0, std::memory_order_relaxed);
        wifiStatusCheckMaxUs.store(0, std::memory_order_relaxed);
        wifiTimeoutCheckMaxUs.store(0, std::memory_order_relaxed);
        wifiHeapGuardMaxUs.store(0, std::memory_order_relaxed);
        wifiApStaPollMaxUs.store(0, std::memory_order_relaxed);
        wifiStopHttpServerMaxUs.store(0, std::memory_order_relaxed);
        wifiStopStaDisconnectMaxUs.store(0, std::memory_order_relaxed);
        wifiStopApDisableMaxUs.store(0, std::memory_order_relaxed);
        wifiStopModeOffMaxUs.store(0, std::memory_order_relaxed);
        wifiStartPreflightMaxUs.store(0, std::memory_order_relaxed);
        wifiStartApBringupMaxUs.store(0, std::memory_order_relaxed);
        proxyAdvertisingOnTransitions.store(0, std::memory_order_relaxed);
        proxyAdvertisingOffTransitions.store(0, std::memory_order_relaxed);
        proxyAdvertisingState.store(0, std::memory_order_relaxed);
        proxyAdvertisingLastTransitionMs.store(0, std::memory_order_relaxed);
        proxyAdvertisingLastTransitionReason.store(0, std::memory_order_relaxed);
        pushNowRetries.store(0, std::memory_order_relaxed);
        pushNowFailures.store(0, std::memory_order_relaxed);
        alertPersistStarts.store(0, std::memory_order_relaxed);
        alertPersistStartsSkippedActive.store(0, std::memory_order_relaxed);
        alertPersistStartsSkippedInvalid.store(0, std::memory_order_relaxed);
        alertPersistExpires.store(0, std::memory_order_relaxed);
        alertPersistClears.store(0, std::memory_order_relaxed);
        autoPushStarts.store(0, std::memory_order_relaxed);
        autoPushCompletes.store(0, std::memory_order_relaxed);
        autoPushNoProfile.store(0, std::memory_order_relaxed);
        autoPushProfileLoadFail.store(0, std::memory_order_relaxed);
        autoPushProfileWriteFail.store(0, std::memory_order_relaxed);
        autoPushBusyRetries.store(0, std::memory_order_relaxed);
        autoPushModeFail.store(0, std::memory_order_relaxed);
        autoPushVolumeFail.store(0, std::memory_order_relaxed);
        autoPushDisconnectAbort.store(0, std::memory_order_relaxed);
        prioritySelectRowFlag.store(0, std::memory_order_relaxed);
        prioritySelectFirstUsable.store(0, std::memory_order_relaxed);
        prioritySelectFirstEntry.store(0, std::memory_order_relaxed);
        prioritySelectAmbiguousIndex.store(0, std::memory_order_relaxed);
        prioritySelectUnusableIndex.store(0, std::memory_order_relaxed);
        prioritySelectInvalidChosen.store(0, std::memory_order_relaxed);
        alertTablePublishes.store(0, std::memory_order_relaxed);
        alertTablePublishes3Bogey.store(0, std::memory_order_relaxed);
        alertTableRowReplacements.store(0, std::memory_order_relaxed);
        alertTableAssemblyTimeouts.store(0, std::memory_order_relaxed);
        parserRowsBandNone.store(0, std::memory_order_relaxed);
        parserRowsKuRaw.store(0, std::memory_order_relaxed);
        displayLiveInvalidPrioritySkips.store(0, std::memory_order_relaxed);
        displayLiveFallbackToUsable.store(0, std::memory_order_relaxed);
        voiceAnnouncePriority.store(0, std::memory_order_relaxed);
        voiceAnnounceDirection.store(0, std::memory_order_relaxed);
        voiceAnnounceSecondary.store(0, std::memory_order_relaxed);
        voiceAnnounceEscalation.store(0, std::memory_order_relaxed);
        voiceDirectionThrottled.store(0, std::memory_order_relaxed);
        powerAutoPowerArmed.store(0, std::memory_order_relaxed);
        powerAutoPowerTimerStart.store(0, std::memory_order_relaxed);
        powerAutoPowerTimerCancel.store(0, std::memory_order_relaxed);
        powerAutoPowerTimerExpire.store(0, std::memory_order_relaxed);
        powerCarModeAlpSilenceExpire.store(0, std::memory_order_relaxed);
        powerCriticalWarn.store(0, std::memory_order_relaxed);
        powerCriticalShutdown.store(0, std::memory_order_relaxed);
        perfUncleanShutdown.store(0, std::memory_order_relaxed);
        audioPlayCount.store(0, std::memory_order_relaxed);
        audioPlayBusy.store(0, std::memory_order_relaxed);
        audioTaskFail.store(0, std::memory_order_relaxed);
    }
};

// ============================================================================
// Extended metrics for p95/max latency, loop jitter, heap stats
// ============================================================================
struct PerfHistogramMs {
    static constexpr size_t kBucketCount = 10;
    uint32_t buckets[kBucketCount] = {0};
    uint32_t total = 0;
    uint32_t maxMs = 0;
    uint32_t overflow = 0;  // Samples exceeding max bucket (>1000ms)

    void reset() {
        for (size_t i = 0; i < kBucketCount; ++i) {
            buckets[i] = 0;
        }
        total = 0;
        maxMs = 0;
        overflow = 0;
    }
};

// Display-screen perf vocabulary. Current producers emit only Unknown,
// Resting, Scanning, Live, and Persisted. Historical numeric slots 3
// (retired Disconnected) and 6 (retired Camera) remain intentionally unused
// so surviving values stay stable.
enum class PerfDisplayScreen : uint8_t {
    Unknown = 0,
    Resting = 1,
    Scanning = 2,
    Live = 4,
    Persisted = 5,
};

enum class PerfFadeDecision : uint8_t {
    None = 0,
    FadeDown = 1,
    RestoreApplied = 2,
    RestoreSkippedEqual = 3,
    RestoreSkippedNoBaseline = 4,
    RestoreSkippedNotFaded = 5
};

enum class PerfBleTimelineEvent : uint8_t {
    ScanStart = 1,
    TargetFound = 2,
    ConnectStart = 3,
    Connected = 4,
    FirstRx = 5
};

enum class PerfWifiApTransitionReason : uint8_t {
    Unknown = 0,
    Startup = 1,
    StopManual = 2,
    StopTimeout = 3,
    StopNoClients = 4,
    StopNoClientsAuto = 5,
    DropLowDma = 6,
    DropIdleSta = 7,
    StopPoweroff = 8,
    StopOther = 9
};

enum class PerfProxyAdvertisingTransitionReason : uint8_t {
    Unknown = 0,
    StartConnected = 1,
    StartWifiPriorityResume = 2,
    StartRetryWindow = 3,
    StartAppDisconnect = 4,
    StartDirect = 5,
    StopWifiPriority = 6,
    StopNoClientTimeout = 7,
    StopIdleWindow = 8,
    StopBeforeV1Connect = 9,
    StopV1Disconnect = 10,
    StopAppConnected = 11,
    StopOther = 12
};

enum class PerfDisplayRenderPath : uint8_t {
    Full = 0,
    RestingFull = 2,
    RestingIncremental = 3,
    Persisted = 4,
    Preview = 5,
    Restore = 6,
};

enum class PerfDisplayRedrawReason : uint8_t {
    FirstRun = 0,
    EnterLive = 1,
    LeaveLive = 2,
    LeavePersisted = 3,
    ForceRedraw = 4,
    FrequencyChange = 5,
    BandSetChange = 6,
    ArrowChange = 7,
    SignalBarChange = 8,
    VolumeChange = 9,
    BogeyCounterChange = 10,
    RssiRefresh = 11,
    FlashTick = 12,
    FullFlushForRedraw = 14,     // live-update dispatch: needsFullRedraw path
    CacheHitSkipFlush = 15,      // live-update dispatch: DrawnRegion empty,
                                 // canvas byte-identical to panel, no SPI push
    UnionExceedsCap = 16,        // live-update dispatch: DrawnRegion union area
                                 // ≥ 50% canvas, fell back to full flush
    PartialRegionFlush = 17,     // live-update dispatch: flushRegion(union)
};

enum class PerfDisplayRenderScenario : uint8_t {
    None = 0,
    Live = 1,
    Resting = 2,
    Persisted = 3,
    PreviewFirstFrame = 4,
    PreviewSteadyFrame = 5,
    Restore = 6,
};

enum class PerfDisplayRenderSubphase : uint8_t {
    BaseFrame = 0,
    StatusStrip = 1,
    Frequency = 2,
    BandsBars = 3,
    ArrowsIcons = 4,
    Cards = 5,
    Flush = 6,
};

enum class PerfDisplayFlushDecisionPath : uint8_t {
    Resting = 0,
    Persisted = 1,
};

enum class PerfDisplayFlushDecisionReason : uint8_t {
    FullRedraw = 0,
    PendingExternal = 1,
    Painted = 2,
    CacheHit = 3,
};

enum class PerfDisplayStatusPaint : uint8_t {
    Volume = 0,
    Rssi = 1,
    Profile = 2,
    Battery = 3,
    BleProxy = 4,
    Wifi = 5,
    Obd = 6,
    Gps = 7,
    Alp = 8,
};

struct PerfExtendedMetrics {
    PerfHistogramMs notifyToDisplayMs;
    uint32_t loopMaxUs = 0;
    // V1 firmware version reported via RESP_VERSION
    // (PacketParser populates DisplayState.v1FirmwareVersion; we mirror it
    // here so SD perf logs can surface it for diagnostics).  Stored as a
    // 6-digit decimal: e.g. v3.8.943 -> 380943.  Persistent (NOT reset on
    // window rollover — it identifies the connected V1, not a stat).
    uint32_t v1FirmwareVersion = 0;
    uint32_t minFreeHeap = UINT32_MAX;
    uint32_t minLargestBlock = UINT32_MAX;
    uint32_t minFreeDma = UINT32_MAX;         // DMA-capable internal SRAM (WiFi/SD contention)
    uint32_t minLargestDma = UINT32_MAX;      // Largest DMA block (fragmentation detection)
    uint32_t wifiMaxUs = 0;
    uint32_t wifiHandleClientMaxUs = 0;
    uint32_t wifiMaintenanceMaxUs = 0;
    uint32_t wifiStatusCheckMaxUs = 0;
    uint32_t wifiTimeoutCheckMaxUs = 0;
    uint32_t wifiHeapGuardMaxUs = 0;
    uint32_t wifiApStaPollMaxUs = 0;
    uint32_t wifiStopHttpServerMaxUs = 0;
    uint32_t wifiStopStaDisconnectMaxUs = 0;
    uint32_t wifiStopApDisableMaxUs = 0;
    uint32_t wifiStopModeOffMaxUs = 0;
    uint32_t wifiStartPreflightMaxUs = 0;
    uint32_t wifiStartApBringupMaxUs = 0;
    uint32_t fsMaxUs = 0;
    uint32_t sdMaxUs = 0;
    // SD write latency histogram (window totals — reset each reporting window)
    uint32_t sdWriteCount = 0;         // Total SD data writes recorded
    uint32_t sdWriteLt1msCount = 0;    // < 1 ms (< 1000 us)
    uint32_t sdWrite1to5msCount = 0;   // 1–4 ms (1000–4999 us)
    uint32_t sdWrite5to10msCount = 0;  // 5–9 ms (5000–9999 us)
    uint32_t sdWriteGe10msCount = 0;   // ≥ 10 ms (≥ 10000 us)
    uint32_t flushMaxUs = 0;
    uint32_t displayRenderMaxUs = 0;  // Full display render time (draw + flush)
    uint32_t bleDrainMaxUs = 0;
    // BLE connection path timing (for diagnosing reconnect stalls)
    uint32_t bleConnectMaxUs = 0;     // pClient->connect() duration
    uint32_t bleDiscoveryMaxUs = 0;   // discoverAttributes() duration
    uint32_t bleSubscribeMaxUs = 0;   // Max executeSubscribeStep() duration
    uint32_t bleProcessMaxUs = 0;     // bleClient.process() total duration
    uint32_t dispPipeMaxUs = 0;       // displayPipelineModule.handleParsed() duration
    uint32_t touchMaxUs = 0;          // touchUiModule.process() duration
    uint32_t obdMaxUs = 0;              // obdRuntimeModule.update() duration
    uint32_t obdConnectCallMaxUs = 0;
    uint32_t obdSecurityStartCallMaxUs = 0;
    uint32_t obdDiscoveryCallMaxUs = 0;
    uint32_t obdSubscribeCallMaxUs = 0;
    uint32_t obdWriteCallMaxUs = 0;
    uint32_t obdRssiCallMaxUs = 0;
    uint32_t perfReportMaxUs = 0;      // perfMetricsCheckReport snapshot + enqueue
    uint32_t uiToScanCount = 0;       // Screen transitions -> Scanning
    uint32_t uiToRestCount = 0;       // Screen transitions -> Resting
    uint32_t uiScanToRestCount = 0;   // Scanning -> Resting transitions
    uint32_t uiFastScanExitCount = 0; // Scanning dwell < threshold before leaving
    uint32_t uiLastScanDwellMs = 0;   // Last measured scanning dwell
    uint32_t uiMinScanDwellMs = UINT32_MAX; // Session minimum scanning dwell
    uint32_t uiLastScanEnteredMs = 0; // Internal marker for scanning dwell
    uint32_t fadeDownCount = 0;       // Fade-down commands generated
    uint32_t fadeRestoreCount = 0;    // Restore commands generated
    uint32_t fadeSkipEqualCount = 0;  // Restore skipped because current == original
    uint32_t fadeSkipNoBaselineCount = 0; // Restore skipped (baseline missing)
    uint32_t fadeSkipNotFadedCount = 0;   // Restore skipped (session not faded)
    uint8_t fadeLastDecision = 0;     // PerfFadeDecision
    uint8_t fadeLastCurrentVol = 0xFF;
    uint8_t fadeLastOriginalVol = 0xFF;
    uint32_t fadeLastDecisionMs = 0;
    uint32_t speedVolDropCount = 0;         // Speed volume drops
    uint32_t speedVolRestoreCount = 0;      // Speed volume restores issued
    uint32_t speedVolRetryCount = 0;        // Speed volume retries
    uint32_t bleScanStartMs = 0;      // First transition to SCANNING
    uint32_t bleTargetFoundMs = 0;    // First "V1 found" scan-stop transition
    uint32_t bleConnectStartMs = 0;   // First transition to CONNECTING
    uint32_t bleConnectedMs = 0;      // First transition to CONNECTED
    uint32_t bleFirstRxMs = 0;        // First packet observed in BLE drain path
    uint32_t bleFollowupRequestAlertMaxUs = 0;   // Max REQUEST_ALERT_DATA followup duration
    uint32_t bleFollowupRequestVersionMaxUs = 0; // Max REQUEST_VERSION followup duration
    uint32_t bleConnectStableCallbackMaxUs = 0;  // Max stable-connect callback duration
    uint32_t bleProxyStartMaxUs = 0;             // Max proxy advertising start duration
    uint32_t displayVoiceMaxUs = 0;              // Max voice-decision branch duration
    uint32_t displayGapRecoverMaxUs = 0;         // Max alert-gap recovery request duration
    uint32_t displayFullRenderCount = 0;         // Full live render count
    uint32_t displayRestingFullRenderCount = 0;  // Full resting render count
    uint32_t displayRestingIncrementalRenderCount = 0; // Incremental resting render count
    uint32_t displayPersistedRenderCount = 0;    // Persisted render count
    uint32_t displayPreviewRenderCount = 0;      // Preview render count
    uint32_t displayRestoreRenderCount = 0;      // Restore render count
    uint32_t displayLiveScenarioRenderCount = 0;      // Scenario-tagged live render count
    uint32_t displayRestingScenarioRenderCount = 0;   // Scenario-tagged resting render count
    uint32_t displayPersistedScenarioRenderCount = 0; // Scenario-tagged persisted render count
    uint32_t displayPreviewScenarioRenderCount = 0;   // Scenario-tagged preview render count
    uint32_t displayRestoreScenarioRenderCount = 0;   // Scenario-tagged restore render count
    uint32_t displayRestingFlushReasonFullRedrawCount = 0;      // Resting DISPLAY_FLUSH: screen/reset
    uint32_t displayRestingFlushReasonPendingExternalCount = 0; // Resting DISPLAY_FLUSH: pre-frame draw
    uint32_t displayRestingFlushReasonPaintedCount = 0;         // Resting DISPLAY_FLUSH: leaf repaint
    uint32_t displayRestingFlushReasonCacheHitCount = 0;        // Resting no-flush cache hit
    uint32_t displayPersistedFlushReasonFullRedrawCount = 0;      // Persisted DISPLAY_FLUSH: screen/reset
    uint32_t displayPersistedFlushReasonPendingExternalCount = 0; // Persisted DISPLAY_FLUSH: pre-frame draw
    uint32_t displayPersistedFlushReasonPaintedCount = 0;         // Persisted DISPLAY_FLUSH: leaf repaint
    uint32_t displayPersistedFlushReasonCacheHitCount = 0;        // Persisted no-flush cache hit
    uint32_t displayStatusVolumePaintCount = 0;   // Status-strip volume cell repaints
    uint32_t displayStatusRssiPaintCount = 0;     // Status-strip RSSI cell repaints
    uint32_t displayStatusProfilePaintCount = 0;  // Profile label repaints
    uint32_t displayStatusBatteryPaintCount = 0;  // Battery icon/percent repaints
    uint32_t displayStatusBleProxyPaintCount = 0; // BLE proxy icon repaints
    uint32_t displayStatusWifiPaintCount = 0;     // Wi-Fi icon repaints
    uint32_t displayStatusObdPaintCount = 0;      // OBD badge repaints
    uint32_t displayStatusGpsPaintCount = 0;      // GPS badge repaints
    uint32_t displayStatusAlpPaintCount = 0;      // ALP badge repaints
    uint32_t displayRedrawReasonFirstRunCount = 0;
    uint32_t displayRedrawReasonEnterLiveCount = 0;
    uint32_t displayRedrawReasonLeaveLiveCount = 0;
    uint32_t displayRedrawReasonLeavePersistedCount = 0;
    uint32_t displayRedrawReasonForceRedrawCount = 0;
    uint32_t displayRedrawReasonFrequencyChangeCount = 0;
    uint32_t displayRedrawReasonBandSetChangeCount = 0;
    uint32_t displayRedrawReasonArrowChangeCount = 0;
    uint32_t displayRedrawReasonSignalBarChangeCount = 0;
    uint32_t displayRedrawReasonVolumeChangeCount = 0;
    uint32_t displayRedrawReasonBogeyCounterChangeCount = 0;
    uint32_t displayRedrawReasonRssiRefreshCount = 0;
    uint32_t displayRedrawReasonFlashTickCount = 0;
    // Live-update dispatch histogram — counters for the four exit paths of
    // update(priority,...)'s flush decision. See
    // docs/plans/PARTIAL_FLUSH_REGION_UNION_20260422.md acceptance criteria.
    uint32_t displayRedrawReasonFullFlushForRedrawCount = 0; // needsFullRedraw → DISPLAY_FLUSH
    uint32_t displayRedrawReasonCacheHitSkipFlushCount = 0;  // union empty → no push
    uint32_t displayRedrawReasonUnionExceedsCapCount = 0;    // union ≥ 50% canvas → DISPLAY_FLUSH
    uint32_t displayRedrawReasonPartialRegionFlushCount = 0; // flushRegion(union)
    uint32_t displayFullFlushCount = 0;          // Full-screen flush count
    uint32_t displayPartialFlushCount = 0;       // Region flush count
    uint32_t displayPartialFlushAreaPeakPx = 0;  // Peak partial flush area
    uint32_t displayPartialFlushAreaTotalPx = 0; // Session total partial-flush pixels
    uint32_t displayFlushEquivalentAreaTotalPx = 0; // Full + partial flush pixels
    uint32_t displayFlushMaxAreaPx = 0;          // Area of the flush that set flushMaxUs
    uint32_t displayPartialFlushLogicalWidthPeakPx = 0;  // Peak logical partial width
    uint32_t displayPartialFlushLogicalHeightPeakPx = 0; // Peak logical partial height
    uint32_t displayPartialFlushRowCallsPeak = 0;      // Peak physical row blits per partial flush
    uint32_t displayPartialFlushPixelsPerRowPeakPx = 0;  // Peak pixels per physical row blit
    uint32_t displayPartialFlushUsPeak = 0;            // Peak partial-only flush duration
    uint32_t displayPartialFlushWorstUsLogicalWidthPx = 0;  // Logical width for partial time peak
    uint32_t displayPartialFlushWorstUsLogicalHeightPx = 0; // Logical height for partial time peak
    uint32_t displayPartialFlushWorstUsAreaPx = 0;        // Area for partial time peak
    uint32_t displayPartialFlushWouldFullRows64Count = 0;  // Shadow row-cap=64 decisions
    uint32_t displayPartialFlushWouldFullRows128Count = 0; // Shadow row-cap=128 decisions
    uint32_t displayPartialFlushWouldFullRows256Count = 0; // Shadow row-cap=256 decisions
    uint32_t displayUnionExceedsCapAreaPeakPx = 0;          // Peak over-cap dirty union
    uint32_t displayUnionExceedsCapRectCountPeak = 0;       // Peak contributing rect count
    uint32_t displayUnionExceedsCapAreaPeakSourceMask = 0;  // Source mask for area peak
    uint32_t displayUnionExceedsCapWithFrequencyCount = 0;  // Over-cap unions touching frequency
    uint32_t displayUnionExceedsCapWithBandsBarsCount = 0;  // Over-cap unions touching bands/bars
    uint32_t displayUnionExceedsCapWithArrowsCount = 0;     // Over-cap unions touching arrows
    uint32_t displayUnionExceedsCapWithStatusCount = 0;     // Over-cap unions touching status strip
    uint32_t displayUnionExceedsCapWithIndicatorsCount = 0; // Over-cap unions touching OBD/GPS/ALP
    uint32_t displayUnionExceedsCapWithExternalCount = 0;   // Over-cap unions including pending external paint
    uint32_t displayUnionExceedsCapUnclassifiedCount = 0;   // Over-cap unions with unknown source
    uint32_t displayBaseFrameMaxUs = 0;          // drawBaseFrame() stage max
    uint32_t displayStatusStripMaxUs = 0;        // Status strip stage max
    uint32_t displayFrequencyMaxUs = 0;          // Frequency stage max
    uint32_t displayBandsBarsMaxUs = 0;          // Bands + bars stage max
    uint32_t displayArrowsIconsMaxUs = 0;        // Arrows + icons stage max
    uint32_t displayCardsMaxUs = 0;              // Card-row stage max
    uint32_t displayFlushSubphaseMaxUs = 0;      // Inner render flush stage max
    uint32_t displayLiveRenderMaxUs = 0;         // Scenario-tagged live render max
    uint32_t displayRestingRenderMaxUs = 0;      // Scenario-tagged resting render max
    uint32_t displayPersistedRenderMaxUs = 0;    // Scenario-tagged persisted render max
    uint32_t displayPreviewRenderMaxUs = 0;      // Scenario-tagged preview render max
    uint32_t displayRestoreRenderMaxUs = 0;      // Scenario-tagged restore render max
    uint32_t displayPreviewFirstRenderMaxUs = 0; // First preview-frame render max
    uint32_t displayPreviewSteadyRenderMaxUs = 0; // Later preview-frame render max

    void reset() {
        notifyToDisplayMs.reset();
        loopMaxUs = 0;
        minFreeHeap = UINT32_MAX;
        minLargestBlock = UINT32_MAX;
        minFreeDma = UINT32_MAX;
        minLargestDma = UINT32_MAX;
        wifiMaxUs = 0;
        wifiHandleClientMaxUs = 0;
        wifiMaintenanceMaxUs = 0;
        wifiStatusCheckMaxUs = 0;
        wifiTimeoutCheckMaxUs = 0;
        wifiHeapGuardMaxUs = 0;
        wifiApStaPollMaxUs = 0;
        wifiStopHttpServerMaxUs = 0;
        wifiStopStaDisconnectMaxUs = 0;
        wifiStopApDisableMaxUs = 0;
        wifiStopModeOffMaxUs = 0;
        wifiStartPreflightMaxUs = 0;
        wifiStartApBringupMaxUs = 0;
        fsMaxUs = 0;
        sdMaxUs = 0;
        sdWriteCount = 0;
        sdWriteLt1msCount = 0;
        sdWrite1to5msCount = 0;
        sdWrite5to10msCount = 0;
        sdWriteGe10msCount = 0;
        flushMaxUs = 0;
        displayRenderMaxUs = 0;
        bleDrainMaxUs = 0;
        bleConnectMaxUs = 0;
        bleDiscoveryMaxUs = 0;
        bleSubscribeMaxUs = 0;
        bleProcessMaxUs = 0;
        dispPipeMaxUs = 0;
        touchMaxUs = 0;
        obdMaxUs = 0;
        obdConnectCallMaxUs = 0;
        obdSecurityStartCallMaxUs = 0;
        obdDiscoveryCallMaxUs = 0;
        obdSubscribeCallMaxUs = 0;
        obdWriteCallMaxUs = 0;
        obdRssiCallMaxUs = 0;
        perfReportMaxUs = 0;
        uiToScanCount = 0;
        uiToRestCount = 0;
        uiScanToRestCount = 0;
        uiFastScanExitCount = 0;
        uiLastScanDwellMs = 0;
        uiMinScanDwellMs = UINT32_MAX;
        uiLastScanEnteredMs = 0;
        fadeDownCount = 0;
        fadeRestoreCount = 0;
        fadeSkipEqualCount = 0;
        fadeSkipNoBaselineCount = 0;
        fadeSkipNotFadedCount = 0;
        fadeLastDecision = static_cast<uint8_t>(PerfFadeDecision::None);
        fadeLastCurrentVol = 0xFF;
        fadeLastOriginalVol = 0xFF;
        fadeLastDecisionMs = 0;
        speedVolDropCount = 0;
        speedVolRestoreCount = 0;
        speedVolRetryCount = 0;
        bleScanStartMs = 0;
        bleTargetFoundMs = 0;
        bleConnectStartMs = 0;
        bleConnectedMs = 0;
        bleFirstRxMs = 0;
        bleFollowupRequestAlertMaxUs = 0;
        bleFollowupRequestVersionMaxUs = 0;
        bleConnectStableCallbackMaxUs = 0;
        bleProxyStartMaxUs = 0;
        displayVoiceMaxUs = 0;
        displayGapRecoverMaxUs = 0;
        displayFullRenderCount = 0;
        displayRestingFullRenderCount = 0;
        displayRestingIncrementalRenderCount = 0;
        displayPersistedRenderCount = 0;
        displayPreviewRenderCount = 0;
        displayRestoreRenderCount = 0;
        displayLiveScenarioRenderCount = 0;
        displayRestingScenarioRenderCount = 0;
        displayPersistedScenarioRenderCount = 0;
        displayPreviewScenarioRenderCount = 0;
        displayRestoreScenarioRenderCount = 0;
        displayRestingFlushReasonFullRedrawCount = 0;
        displayRestingFlushReasonPendingExternalCount = 0;
        displayRestingFlushReasonPaintedCount = 0;
        displayRestingFlushReasonCacheHitCount = 0;
        displayPersistedFlushReasonFullRedrawCount = 0;
        displayPersistedFlushReasonPendingExternalCount = 0;
        displayPersistedFlushReasonPaintedCount = 0;
        displayPersistedFlushReasonCacheHitCount = 0;
        displayStatusVolumePaintCount = 0;
        displayStatusRssiPaintCount = 0;
        displayStatusProfilePaintCount = 0;
        displayStatusBatteryPaintCount = 0;
        displayStatusBleProxyPaintCount = 0;
        displayStatusWifiPaintCount = 0;
        displayStatusObdPaintCount = 0;
        displayStatusGpsPaintCount = 0;
        displayStatusAlpPaintCount = 0;
        displayRedrawReasonFirstRunCount = 0;
        displayRedrawReasonEnterLiveCount = 0;
        displayRedrawReasonLeaveLiveCount = 0;
        displayRedrawReasonLeavePersistedCount = 0;
        displayRedrawReasonForceRedrawCount = 0;
        displayRedrawReasonFrequencyChangeCount = 0;
        displayRedrawReasonBandSetChangeCount = 0;
        displayRedrawReasonArrowChangeCount = 0;
        displayRedrawReasonSignalBarChangeCount = 0;
        displayRedrawReasonVolumeChangeCount = 0;
        displayRedrawReasonBogeyCounterChangeCount = 0;
        displayRedrawReasonRssiRefreshCount = 0;
        displayRedrawReasonFlashTickCount = 0;
        displayRedrawReasonFullFlushForRedrawCount = 0;
        displayRedrawReasonCacheHitSkipFlushCount = 0;
        displayRedrawReasonUnionExceedsCapCount = 0;
        displayRedrawReasonPartialRegionFlushCount = 0;
        displayFullFlushCount = 0;
        displayPartialFlushCount = 0;
        displayPartialFlushAreaPeakPx = 0;
        displayPartialFlushAreaTotalPx = 0;
        displayFlushEquivalentAreaTotalPx = 0;
        displayFlushMaxAreaPx = 0;
        displayPartialFlushLogicalWidthPeakPx = 0;
        displayPartialFlushLogicalHeightPeakPx = 0;
        displayPartialFlushRowCallsPeak = 0;
        displayPartialFlushPixelsPerRowPeakPx = 0;
        displayPartialFlushUsPeak = 0;
        displayPartialFlushWorstUsLogicalWidthPx = 0;
        displayPartialFlushWorstUsLogicalHeightPx = 0;
        displayPartialFlushWorstUsAreaPx = 0;
        displayPartialFlushWouldFullRows64Count = 0;
        displayPartialFlushWouldFullRows128Count = 0;
        displayPartialFlushWouldFullRows256Count = 0;
        displayUnionExceedsCapAreaPeakPx = 0;
        displayUnionExceedsCapRectCountPeak = 0;
        displayUnionExceedsCapAreaPeakSourceMask = 0;
        displayUnionExceedsCapWithFrequencyCount = 0;
        displayUnionExceedsCapWithBandsBarsCount = 0;
        displayUnionExceedsCapWithArrowsCount = 0;
        displayUnionExceedsCapWithStatusCount = 0;
        displayUnionExceedsCapWithIndicatorsCount = 0;
        displayUnionExceedsCapWithExternalCount = 0;
        displayUnionExceedsCapUnclassifiedCount = 0;
        displayBaseFrameMaxUs = 0;
        displayStatusStripMaxUs = 0;
        displayFrequencyMaxUs = 0;
        displayBandsBarsMaxUs = 0;
        displayArrowsIconsMaxUs = 0;
        displayCardsMaxUs = 0;
        displayFlushSubphaseMaxUs = 0;
        displayLiveRenderMaxUs = 0;
        displayRestingRenderMaxUs = 0;
        displayPersistedRenderMaxUs = 0;
        displayPreviewRenderMaxUs = 0;
        displayRestoreRenderMaxUs = 0;
        displayPreviewFirstRenderMaxUs = 0;
        displayPreviewSteadyRenderMaxUs = 0;
    }
};

extern PerfExtendedMetrics perfExtended;

void perfRecordNotifyToDisplayMs(uint32_t ms);
void perfRecordLoopJitterUs(uint32_t us);
void perfRecordHeapStats(uint32_t freeHeap, uint32_t largestBlock, uint32_t freeDma, uint32_t largestDma);
void perfRecordWifiProcessUs(uint32_t us);
void perfRecordWifiHandleClientUs(uint32_t us);
void perfRecordWifiMaintenanceUs(uint32_t us);
void perfRecordWifiStatusCheckUs(uint32_t us);
void perfRecordWifiTimeoutCheckUs(uint32_t us);
void perfRecordWifiHeapGuardUs(uint32_t us);
void perfRecordWifiApStaPollUs(uint32_t us);
void perfRecordWifiStopHttpServerUs(uint32_t us);
void perfRecordWifiStopStaDisconnectUs(uint32_t us);
void perfRecordWifiStopApDisableUs(uint32_t us);
void perfRecordWifiStopModeOffUs(uint32_t us);
void perfRecordWifiStartPreflightUs(uint32_t us);
void perfRecordWifiStartApBringupUs(uint32_t us);
void perfRecordFsServeUs(uint32_t us);
void perfRecordSdFlushUs(uint32_t us);
void perfRecordFlushUs(uint32_t us, uint32_t areaPx, bool fullFlush);
void perfRecordPartialFlushShape(uint32_t us, uint32_t areaPx, uint16_t logicalW, uint16_t logicalH);
void perfRecordDisplayUnionExceedsCap(uint32_t areaPx, uint8_t rectCount, uint8_t sourceMask);
void perfRecordDisplayRenderUs(uint32_t us);
void perfRecordDisplayScenarioRenderUs(uint32_t us);
void perfRecordDisplayRenderPath(PerfDisplayRenderPath path);
void perfRecordDisplayRedrawReason(PerfDisplayRedrawReason reason);
void perfRecordDisplayRenderSubphaseUs(PerfDisplayRenderSubphase subphase, uint32_t us);
void perfRecordDisplayFlushDecision(PerfDisplayFlushDecisionPath path,
                                    PerfDisplayFlushDecisionReason reason);
void perfRecordDisplayStatusPaint(PerfDisplayStatusPaint element);
void perfSetDisplayRenderScenario(PerfDisplayRenderScenario scenario);
PerfDisplayRenderScenario perfGetDisplayRenderScenario();
void perfClearDisplayRenderScenario();
void perfRecordBleDrainUs(uint32_t us);
void perfRecordBleConnectUs(uint32_t us);
void perfRecordBleDiscoveryUs(uint32_t us);
void perfRecordBleSubscribeUs(uint32_t us);
void perfRecordBleFollowupRequestAlertUs(uint32_t us);
void perfRecordBleFollowupRequestVersionUs(uint32_t us);
// Capture V1 firmware version (decoded from RESP_VERSION).
// Idempotent — only the first non-zero value sticks unless the V1 changes ID.
void perfRecordV1FirmwareVersion(uint32_t version);
void perfRecordBleConnectStableCallbackUs(uint32_t us);
void perfRecordBleProxyStartUs(uint32_t us);
void perfRecordBleProcessUs(uint32_t us);
void perfRecordDispPipeUs(uint32_t us);
void perfRecordDisplayVoiceUs(uint32_t us);
void perfRecordDisplayGapRecoverUs(uint32_t us);
void perfRecordTouchUs(uint32_t us);
void perfRecordPerfReportUs(uint32_t us);
void perfRecordObdConnectCallUs(uint32_t us);
void perfRecordObdSecurityStartCallUs(uint32_t us);
void perfRecordObdDiscoveryCallUs(uint32_t us);
void perfRecordObdSubscribeCallUs(uint32_t us);
void perfRecordObdWriteCallUs(uint32_t us);
void perfRecordObdRssiCallUs(uint32_t us);
// Records transitions among the actively emitted display states
// (Unknown/Scanning/Resting/Live/Persisted). Reserved compatibility values
// remain valid enum members but should not be emitted by current production code.
void perfRecordDisplayScreenTransition(PerfDisplayScreen from, PerfDisplayScreen to, uint32_t nowMs);
void perfRecordVolumeFadeDecision(PerfFadeDecision decision, uint8_t currentVolume, uint8_t originalVolume, uint32_t nowMs);
void perfRecordSpeedVolDrop();
void perfRecordSpeedVolRestore();
void perfRecordSpeedVolRetry();
void perfRecordBleTimelineEvent(PerfBleTimelineEvent event, uint32_t nowMs);
void perfRecordWifiApTransition(bool apActive, uint8_t reasonCode, uint32_t nowMs);
void perfRecordProxyAdvertisingTransition(bool advertising, uint8_t reasonCode, uint32_t nowMs);

uint32_t perfGetMinFreeHeap();
uint32_t perfGetMinFreeDma();
void perfRecordObdUs(uint32_t us);
uint32_t perfGetPrevWindowLoopMaxUs();
uint32_t perfGetPrevWindowWifiMaxUs();
uint32_t perfGetPrevWindowBleProcessMaxUs();
uint32_t perfGetPrevWindowDispPipeMaxUs();
uint32_t perfGetWifiApState();
uint32_t perfGetWifiApLastTransitionMs();
uint32_t perfGetWifiApLastTransitionReason();
const char* perfWifiApTransitionReasonName(uint32_t reasonCode);
uint32_t perfGetProxyAdvertisingState();
uint32_t perfGetProxyAdvertisingLastTransitionMs();
uint32_t perfGetProxyAdvertisingLastTransitionReason();
const char* perfProxyAdvertisingTransitionReasonName(uint32_t reasonCode);

// ============================================================================
// Sampled latency tracking (only when PERF_METRICS=1)
// Uses std::atomic for thread-safe access
// ============================================================================
struct PerfLatency {
    // BLE→Flush latency (microseconds)
    std::atomic<uint32_t> minUs{UINT32_MAX};
    std::atomic<uint32_t> maxUs{0};
    std::atomic<uint64_t> totalUs{0};
    std::atomic<uint32_t> sampleCount{0};

    // Per-stage breakdown (for debugging bottlenecks)
    std::atomic<uint32_t> notifyToQueueUs{0};    // notify callback → queue send
    std::atomic<uint32_t> queueToParseUs{0};     // queue receive → parse done
    std::atomic<uint32_t> parseToFlushUs{0};     // parse done → display flush

    void reset() {
        minUs.store(UINT32_MAX, std::memory_order_relaxed);
        maxUs.store(0, std::memory_order_relaxed);
        totalUs.store(0, std::memory_order_relaxed);
        sampleCount.store(0, std::memory_order_relaxed);
        notifyToQueueUs.store(0, std::memory_order_relaxed);
        queueToParseUs.store(0, std::memory_order_relaxed);
        parseToFlushUs.store(0, std::memory_order_relaxed);
    }

    uint32_t avgUs() const {
        uint32_t count = sampleCount.load(std::memory_order_relaxed);
        return count > 0 ? static_cast<uint32_t>(totalUs.load(std::memory_order_relaxed) / count) : 0;
    }
};

// ============================================================================
// Global instances
// ============================================================================
extern PerfCounters perfCounters;

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
    uint32_t freeDma;         // Cached internal 8-bit heap (legacy column)
    uint32_t largestDma;      // Cached largest internal 8-bit block (legacy column)
    uint32_t freeDmaCap;      // True MALLOC_CAP_DMA free bytes
    uint32_t largestDmaCap;   // True MALLOC_CAP_DMA largest free block
    uint32_t dmaFreeMin;      // Min MALLOC_CAP_DMA free bytes since session start
    uint32_t dmaLargestMin;   // Min MALLOC_CAP_DMA largest block since session start
    uint32_t bleProcessMaxUs; // Window max bleClient.process() duration
    uint32_t touchMaxUs;      // Window max touchUiModule.process() duration
    uint32_t wifiMaxUs;        // Window max wifiManager.process() duration
    uint32_t uiToScanCount;   // Screen transitions to scanning
    uint32_t uiToRestCount;   // Screen transitions to resting
    uint32_t uiScanToRestCount;   // Scanning -> resting transitions
    uint32_t uiFastScanExitCount; // Scan dwell below threshold before exit
    uint32_t uiLastScanDwellMs;   // Most recent scan dwell duration
    uint32_t uiMinScanDwellMs;    // Session minimum scan dwell duration
    uint32_t fadeDownCount;       // Fade-down actions emitted
    uint32_t fadeRestoreCount;    // Restore actions emitted
    uint32_t fadeSkipEqualCount;  // Restore skipped (current == baseline)
    uint32_t fadeSkipNoBaselineCount; // Restore skipped (missing baseline)
    uint32_t fadeSkipNotFadedCount;   // Restore skipped (session never faded)
    uint8_t fadeLastDecision;     // PerfFadeDecision code
    uint8_t fadeLastCurrentVol;   // Last observed current volume
    uint8_t fadeLastOriginalVol;  // Last observed baseline/original volume
    uint32_t fadeLastDecisionMs;  // Last fade decision timestamp
    uint32_t speedVolDropCount;         // Speed volume drops
    uint32_t speedVolRestoreCount;      // Speed volume restores issued
    uint32_t speedVolRetryCount;        // Speed volume retries
    uint32_t bleScanStartMs;      // First scan start timestamp
    uint32_t bleTargetFoundMs;    // First target-found timestamp
    uint32_t bleConnectStartMs;   // First connect-start timestamp
    uint32_t bleConnectedMs;      // First connected timestamp
    uint32_t bleFirstRxMs;        // First parsed/received V1 packet timestamp
    uint32_t bleFollowupRequestAlertMaxUs;   // Window max connect-burst alert request duration
    uint32_t bleFollowupRequestVersionMaxUs; // Window max connect-burst version request duration
    uint32_t bleConnectStableCallbackMaxUs;  // Window max stable-connect callback duration
    uint32_t bleProxyStartMaxUs;             // Window max proxy advertising start duration
    uint32_t displayGapRecoverMaxUs;         // Window max display alert-gap recovery duration
    uint32_t displayFullRenderCount;         // Session full live renders
    uint32_t displayRestingFullRenderCount;  // Session full resting renders
    uint32_t displayRestingIncrementalRenderCount; // Session incremental resting renders
    uint32_t displayPersistedRenderCount;    // Session persisted renders
    uint32_t displayPreviewRenderCount;      // Session preview renders
    uint32_t displayRestoreRenderCount;      // Session restore renders
    uint32_t displayLiveScenarioRenderCount;      // Session live scenario render count
    uint32_t displayRestingScenarioRenderCount;   // Session resting scenario render count
    uint32_t displayPersistedScenarioRenderCount; // Session persisted scenario render count
    uint32_t displayPreviewScenarioRenderCount;   // Session preview scenario render count
    uint32_t displayRestoreScenarioRenderCount;   // Session restore scenario render count
    uint32_t displayRestingFlushReasonFullRedrawCount;      // Resting DISPLAY_FLUSH: screen/reset
    uint32_t displayRestingFlushReasonPendingExternalCount; // Resting DISPLAY_FLUSH: pre-frame draw
    uint32_t displayRestingFlushReasonPaintedCount;         // Resting DISPLAY_FLUSH: leaf repaint
    uint32_t displayRestingFlushReasonCacheHitCount;        // Resting no-flush cache hit
    uint32_t displayPersistedFlushReasonFullRedrawCount;      // Persisted DISPLAY_FLUSH: screen/reset
    uint32_t displayPersistedFlushReasonPendingExternalCount; // Persisted DISPLAY_FLUSH: pre-frame draw
    uint32_t displayPersistedFlushReasonPaintedCount;         // Persisted DISPLAY_FLUSH: leaf repaint
    uint32_t displayPersistedFlushReasonCacheHitCount;        // Persisted no-flush cache hit
    uint32_t displayStatusVolumePaintCount;   // Status-strip volume cell repaints
    uint32_t displayStatusRssiPaintCount;     // Status-strip RSSI cell repaints
    uint32_t displayStatusProfilePaintCount;  // Profile label repaints
    uint32_t displayStatusBatteryPaintCount;  // Battery icon/percent repaints
    uint32_t displayStatusBleProxyPaintCount; // BLE proxy icon repaints
    uint32_t displayStatusWifiPaintCount;     // Wi-Fi icon repaints
    uint32_t displayStatusObdPaintCount;      // OBD badge repaints
    uint32_t displayStatusGpsPaintCount;      // GPS badge repaints
    uint32_t displayStatusAlpPaintCount;      // ALP badge repaints
    uint32_t displayRedrawReasonFirstRunCount;    // Full redraw first-run triggers
    uint32_t displayRedrawReasonEnterLiveCount;   // Full redraw enter-live triggers
    uint32_t displayRedrawReasonLeaveLiveCount;   // Full redraw leave-live triggers
    uint32_t displayRedrawReasonLeavePersistedCount; // Full redraw leave-persisted triggers
    uint32_t displayRedrawReasonForceRedrawCount; // Full redraw force-reset triggers
    uint32_t displayRedrawReasonFrequencyChangeCount; // Full redraw frequency-change triggers
    uint32_t displayRedrawReasonBandSetChangeCount; // Full redraw band-set-change triggers
    uint32_t displayRedrawReasonArrowChangeCount; // Full redraw arrow-change triggers
    uint32_t displayRedrawReasonSignalBarChangeCount; // Full redraw signal-bar-change triggers
    uint32_t displayRedrawReasonVolumeChangeCount; // Full redraw volume-change triggers
    uint32_t displayRedrawReasonBogeyCounterChangeCount; // Full redraw bogey-counter-change triggers
    uint32_t displayRedrawReasonRssiRefreshCount; // Full redraw RSSI-refresh triggers
    uint32_t displayRedrawReasonFlashTickCount;   // Full redraw flash-tick triggers
    uint32_t displayRedrawReasonFullFlushForRedrawCount; // Live dispatch: needsFullRedraw
    uint32_t displayRedrawReasonCacheHitSkipFlushCount;  // Live dispatch: union empty
    uint32_t displayRedrawReasonUnionExceedsCapCount;    // Live dispatch: union ≥ cap
    uint32_t displayRedrawReasonPartialRegionFlushCount; // Live dispatch: flushRegion(union)
    uint32_t displayFullFlushCount;         // Session full-screen flushes
    uint32_t displayPartialFlushCount;      // Session region flushes
    uint32_t displayPartialFlushAreaPeakPx; // Peak region-flush area
    uint32_t displayPartialFlushAreaTotalPx; // Total region-flush pixels
    uint32_t displayFlushEquivalentAreaTotalPx; // Total pixels flushed (full + partial)
    uint32_t displayFlushMaxAreaPx;         // Area for the flushMaxUs winner
    uint32_t displayPartialFlushLogicalWidthPeakPx;  // Peak logical partial width
    uint32_t displayPartialFlushLogicalHeightPeakPx; // Peak logical partial height
    uint32_t displayPartialFlushRowCallsPeak;      // Peak physical row blits per partial flush
    uint32_t displayPartialFlushPixelsPerRowPeakPx;  // Peak pixels per physical row blit
    uint32_t displayPartialFlushUsPeak;            // Peak partial-only flush duration
    uint32_t displayPartialFlushWorstUsLogicalWidthPx;  // Logical width for partial time peak
    uint32_t displayPartialFlushWorstUsLogicalHeightPx; // Logical height for partial time peak
    uint32_t displayPartialFlushWorstUsAreaPx;        // Area for partial time peak
    uint32_t displayPartialFlushWouldFullRows64Count;  // Shadow row-cap=64 decisions
    uint32_t displayPartialFlushWouldFullRows128Count; // Shadow row-cap=128 decisions
    uint32_t displayPartialFlushWouldFullRows256Count; // Shadow row-cap=256 decisions
    uint32_t displayUnionExceedsCapAreaPeakPx;         // Peak over-cap dirty union
    uint32_t displayUnionExceedsCapRectCountPeak;      // Peak contributing rect count
    uint32_t displayUnionExceedsCapAreaPeakSourceMask; // Source mask for area peak
    uint32_t displayUnionExceedsCapWithFrequencyCount; // Over-cap unions touching frequency
    uint32_t displayUnionExceedsCapWithBandsBarsCount; // Over-cap unions touching bands/bars
    uint32_t displayUnionExceedsCapWithArrowsCount;    // Over-cap unions touching arrows
    uint32_t displayUnionExceedsCapWithStatusCount;    // Over-cap unions touching status strip
    uint32_t displayUnionExceedsCapWithIndicatorsCount; // Over-cap unions touching OBD/GPS/ALP
    uint32_t displayUnionExceedsCapWithExternalCount;  // Over-cap unions including pending external paint
    uint32_t displayUnionExceedsCapUnclassifiedCount;  // Over-cap unions with unknown source
    uint32_t displayBaseFrameMaxUs;         // Window max base-frame stage
    uint32_t displayStatusStripMaxUs;       // Window max status-strip stage
    uint32_t displayFrequencyMaxUs;         // Window max frequency stage
    uint32_t displayBandsBarsMaxUs;         // Window max bands+bars stage
    uint32_t displayArrowsIconsMaxUs;       // Window max arrows+icons stage
    uint32_t displayFlushSubphaseMaxUs;     // Window max inner render flush stage
    uint32_t displayLiveRenderMaxUs;        // Window max live render
    uint32_t displayRestingRenderMaxUs;     // Window max resting render
    uint32_t displayPersistedRenderMaxUs;   // Window max persisted render
    uint32_t displayPreviewRenderMaxUs;     // Window max preview render
    uint32_t displayRestoreRenderMaxUs;     // Window max restore render
    uint32_t displayPreviewFirstRenderMaxUs; // Window max first preview render
    uint32_t displayPreviewSteadyRenderMaxUs; // Window max later preview render
    uint32_t alertPersistStarts;  // Persisted-alert sessions started
    uint32_t alertPersistStartsSkippedActive;  // startPersistence() no-op while window already active
    uint32_t alertPersistStartsSkippedInvalid; // startPersistence() refused — no valid alert latched
    uint32_t alertPersistExpires; // Persisted-alert windows expired naturally
    uint32_t alertPersistClears;  // Persisted-alert state cleared explicitly
    uint32_t autoPushStarts;      // Auto-push runs initiated
    uint32_t autoPushCompletes;   // Auto-push runs completed
    uint32_t autoPushNoProfile;   // Auto-push slot had no configured profile
    uint32_t autoPushProfileLoadFail;  // Auto-push profile load failures
    uint32_t autoPushProfileWriteFail; // Auto-push profile write exhausted retries
    uint32_t autoPushBusyRetries; // Auto-push write-busy retries
    uint32_t autoPushModeFail;    // Auto-push mode set failures
    uint32_t autoPushVolumeFail;  // Auto-push volume set failures
    uint32_t autoPushDisconnectAbort; // Auto-push aborted due to disconnect
    uint32_t powerAutoPowerArmed;     // Auto power-off armed on first V1 data
    uint32_t powerAutoPowerTimerStart; // Auto power-off timer started
    uint32_t powerAutoPowerTimerCancel; // Auto power-off timer cancelled on reconnect
    uint32_t powerAutoPowerTimerExpire; // Auto power-off timer expired
    uint32_t powerCarModeAlpSilenceExpire; // Car-mode ALP UART silence shutdown fired
    uint32_t powerCriticalWarn;       // Critical-battery warning shown
    uint32_t powerCriticalShutdown;   // Critical-battery shutdown triggered
    uint32_t perfUncleanShutdown;     // Boot saw cleanShutdn=false (previous run died uncleanly)
    uint32_t cmdBleBusy;              // BLE command write transient failures/retries

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
    uint32_t perfReportMaxUs;    // Window max perfMetricsCheckReport snapshot+enqueue
    uint32_t prioritySelectRowFlag;      // Priority chosen from alert-row isPriority bit
    uint32_t prioritySelectFirstUsable;  // Priority chosen from first usable alert fallback
    uint32_t prioritySelectFirstEntry;   // Priority fell back to entry 0 (last resort)
    uint32_t prioritySelectAmbiguousIndex; // Alert table complete under both 0-based and 1-based mapping
    uint32_t prioritySelectUnusableIndex;  // Row-priority candidate present but unusable
    uint32_t prioritySelectInvalidChosen;  // Final chosen alert invalid/zero-freq non-laser
    uint32_t alertTablePublishes;          // Complete alert tables published
    uint32_t alertTablePublishes3Bogey;    // Complete tables published with count=3
    uint32_t alertTableRowReplacements;    // Duplicate row-index replacements
    uint32_t alertTableAssemblyTimeouts;   // Partial table assemblies dropped on timeout
    uint32_t parserRowsBandNone;           // Alert rows decoded with BAND_NONE
    uint32_t parserRowsKuRaw;              // Alert rows containing Ku raw bit (0x10)
    uint32_t displayLiveInvalidPrioritySkips; // Live display invalid-priority early returns
    uint32_t displayLiveFallbackToUsable;  // Live display fallback-to-usable selections
    uint32_t obdMaxUs;                     // Window max obdRuntimeModule.update() duration
    uint32_t obdConnectCallMaxUs;          // Window max inline OBD BLE connect() duration
    uint32_t obdSecurityStartCallMaxUs;    // Window max inline OBD BLE security-start duration
    uint32_t obdDiscoveryCallMaxUs;        // Window max inline OBD BLE discovery duration
    uint32_t obdSubscribeCallMaxUs;        // Window max inline OBD BLE subscribe duration
    uint32_t obdWriteCallMaxUs;            // Window max inline OBD BLE command write duration
    uint32_t obdRssiCallMaxUs;             // Window max inline OBD BLE RSSI read duration
    uint32_t obdPollErrors;                // OBD poll errors this window
    uint32_t obdStaleCount;                // OBD stale speed readings this window
    uint32_t perfDrop;                     // Perf snapshot drops since session start
    uint32_t eventBusDrops;                // System event-bus drops since session start
    uint32_t wifiHandleClientMaxUs;        // Window max HTTP client servicing duration
    uint32_t wifiMaintenanceMaxUs;         // Window max WiFi maintenance duration
    uint32_t wifiStatusCheckMaxUs;         // Window max STA status check duration
    uint32_t wifiTimeoutCheckMaxUs;        // Window max auto-timeout check duration
    uint32_t wifiHeapGuardMaxUs;           // Window max WiFi heap guard duration
    uint32_t wifiApStaPollMaxUs;           // Window max AP station polling duration
    uint32_t wifiStopHttpServerMaxUs;      // Window max HTTP stop duration
    uint32_t wifiStopStaDisconnectMaxUs;   // Window max STA disconnect duration
    uint32_t wifiStopApDisableMaxUs;       // Window max AP disable duration
    uint32_t wifiStopModeOffMaxUs;         // Window max radio-off duration
    uint32_t wifiStartPreflightMaxUs;      // Window max WiFi start preflight duration
    uint32_t wifiStartApBringupMaxUs;      // Window max AP bring-up duration
    uint32_t freeDmaMin;                   // Min cached internal 8-bit heap free bytes since session start
    uint32_t largestDmaMin;                // Min cached internal 8-bit largest block since session start
    uint8_t bleState;                      // BLE runtime state code
    uint8_t subscribeStep;                 // BLE subscribe-step machine code
    uint8_t connectInProgress;             // BLE connect attempt active
    uint8_t asyncConnectPending;           // BLE async connect callback pending
    uint8_t pendingDisconnectCleanup;      // BLE deferred disconnect cleanup pending
    uint8_t proxyAdvertising;              // Proxy advertising currently active
    uint8_t proxyAdvertisingLastTransitionReason; // PerfProxyAdvertisingTransitionReason code
    uint8_t wifiPriorityMode;              // BLE WiFi-priority suppression active
    uint8_t speedSourceSelected;           // SpeedSource code for selected speed source
    uint8_t speedSourceValid;              // Selected speed sample valid/fresh
    uint32_t speedSelectedMph_x10;         // Selected speed mph * 10 when valid
    uint32_t speedSelectedAgeMs;           // Selected speed age when valid; UINT32_MAX otherwise
    uint32_t speedSourceSwitches;          // Speed source switches since session start
    uint32_t speedNoSourceSelections;      // No-source selections since session start
    uint32_t speedGpsSelections;           // GPS selections since session start
    uint8_t cycleState;                    // Connection cycle state code
    uint32_t cycleTransitionsTotal;        // Connection cycle transitions since boot/reset
    uint32_t cycleTimeInStateMs;           // Current connection cycle dwell time
    uint32_t cycleTeardownDurationMs;      // Last completed teardown duration
    uint32_t cycleObdRetryAttemptsTotal;   // Coordinator-observed OBD retry attempts
    uint32_t cycleWifiManualPhoneKicksTotal; // Proxy phones disconnected for manual WiFi
    uint8_t cycleProxyNoClientLatched;     // Legacy field; proxy now downshifts cadence

    // GPS observability (schema v37 — appended, never inserted)
    uint64_t utcEpochMs;               // UTC at snapshot time (0 = no GPS fix)
    bool     utcValid;                 // true if GPS publisher provided fresh UTC
    uint32_t gpsSentencesOk;           // Sentences parsed successfully
    uint32_t gpsSentencesChecksumFail; // Sentences rejected on checksum
    uint32_t gpsSentencesUnknown;      // Sentences not RMC/GGA (not an error)
    uint32_t gpsBufferOverruns;        // NMEA line buffer overruns
    uint32_t gpsBytesIn;               // Total UART bytes ingested
    uint32_t gpsFirstFixMs;            // millis() at first stable fix (0 = not yet)
    uint32_t gpsLastSentenceAgeMs;     // ms since last NMEA sentence (UINT32_MAX = none)
    uint32_t gpsFixAgeMs;              // ms since last fix (UINT32_MAX = no fix)
    uint32_t gpsStableFixAgeMs;        // ms since stable fix (UINT32_MAX = no fix)
    uint8_t  gpsSatellitesInUse;       // Satellites in current fix
    uint16_t gpsHdopX10;               // HDOP * 10 (e.g. 12 = 1.2; UINT16_MAX = NaN)
    bool     gpsHasFix;                // GPS has active fix
    bool     gpsStableHasFix;          // GPS has stable fix (with hysteresis)
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

void perfSetConnectionCycleSnapshot(uint8_t stateCode,
                                    uint32_t timeInStateMs,
                                    uint32_t transitionsTotal,
                                    uint32_t teardownDurationMs,
                                    uint32_t obdRetryAttemptsTotal,
                                    uint32_t wifiManualPhoneKicksTotal,
                                    bool proxyNoClientLatched);

const char* perfConnectionCycleStateName(uint8_t stateCode);

void perfCaptureRuntimeMetricsSnapshot(
    PerfRuntimeMetricsSnapshot& snapshot,
    PerfRuntimeSnapshotMode mode = PerfRuntimeSnapshotMode::PreserveWindowPeaks);

inline PhoneCmdDropMetricsSnapshot perfPhoneCmdDropMetricsSnapshot() {
    PhoneCmdDropMetricsSnapshot snapshot;
    snapshot.overflow = perfCounters.phoneCmdDropsOverflow.load(std::memory_order_relaxed);
    snapshot.invalid = perfCounters.phoneCmdDropsInvalid.load(std::memory_order_relaxed);
    snapshot.bleFail = perfCounters.phoneCmdDropsBleFail.load(std::memory_order_relaxed);
    snapshot.lockBusy = perfCounters.phoneCmdDropsLockBusy.load(std::memory_order_relaxed);
    return snapshot;
}

inline void perfAppendPhoneCmdDropMetrics(JsonDocument& doc,
                                          const PhoneCmdDropMetricsSnapshot& snapshot) {
    doc["phoneCmdDropsOverflow"] = snapshot.overflow;
    doc["phoneCmdDropsInvalid"] = snapshot.invalid;
    doc["phoneCmdDropsBleFail"] = snapshot.bleFail;
    doc["phoneCmdDropsLockBusy"] = snapshot.lockBusy;
}

#if PERF_METRICS
extern PerfLatency perfLatency;
#endif

#if PERF_METRICS && PERF_MONITORING
extern bool perfDebugEnabled;        // Runtime debug print enable
extern uint32_t perfLastReportMs;    // Last report timestamp
#endif

// ============================================================================
// Inline instrumentation macros (zero cost when disabled)
// ============================================================================

// Always-on counter increments
#define PERF_INC(counter) (perfCounters.counter++)
#define PERF_ADD(counter, value) (perfCounters.counter += (value))
#define PERF_SET(counter, value) (perfCounters.counter = (value))
#define PERF_MAX(counter, value) do { \
    const uint32_t _perfMaxValue = static_cast<uint32_t>(value); \
    uint32_t _perfMaxCurrent = perfCounters.counter.load(std::memory_order_relaxed); \
    while (_perfMaxValue > _perfMaxCurrent && \
           !perfCounters.counter.compare_exchange_weak(_perfMaxCurrent, _perfMaxValue, \
                                                       std::memory_order_relaxed, \
                                                       std::memory_order_relaxed)) {} \
} while(0)

// Timestamp capture (always on, but cheap)
#define PERF_TIMESTAMP_US() ((uint32_t)esp_timer_get_time())

#if PERF_METRICS && PERF_MONITORING

// Sampled latency recording
#define PERF_SAMPLE_LATENCY(startUs, endUs) do { \
    static uint32_t _sampleCounter = 0; \
    if ((++_sampleCounter & (PERF_SAMPLE_RATE - 1)) == 0) { \
        uint32_t _lat = (endUs) - (startUs); \
        if (_lat < perfLatency.minUs) perfLatency.minUs = _lat; \
        if (_lat > perfLatency.maxUs) perfLatency.maxUs = _lat; \
        perfLatency.totalUs += _lat; \
        perfLatency.sampleCount++; \
    } \
} while(0)

// Stage timing (for debugging)
#if PERF_VERBOSE
#define PERF_STAGE_TIME(stage, value) (perfLatency.stage = (value))
#else
#define PERF_STAGE_TIME(stage, value) ((void)0)
#endif

// Threshold alert (immediate print if exceeded)
#if PERF_VERBOSE
#define PERF_ALERT_IF_SLOW(latencyUs) do { \
    if (perfDebugEnabled && (latencyUs) > (PERF_LATENCY_ALERT_MS * 1000)) { \
        Serial.printf("[PERF ALERT] latency=%luus\n", (unsigned long)(latencyUs)); \
    } \
} while(0)
#else
#define PERF_ALERT_IF_SLOW(latencyUs) ((void)0)
#endif

#else  // PERF_METRICS == 0 or PERF_MONITORING == 0

#define PERF_SAMPLE_LATENCY(startUs, endUs) ((void)0)
#define PERF_STAGE_TIME(stage, value) ((void)0)
#define PERF_ALERT_IF_SLOW(latencyUs) ((void)0)

#endif  // PERF_METRICS && PERF_MONITORING

// ============================================================================
// API functions
// ============================================================================

// Initialize metrics system
void perfMetricsInit();

// Reset all metrics
void perfMetricsReset();

// Reset only windowed runtime peaks before starting a new logical perf CSV
// session. Cumulative counters remain intact so delta-based importers can still
// compare first/last rows inside the session.
void perfMetricsResetSessionWindow();

// Check if periodic report is due (call from loop)
// Returns true if report was printed
bool perfMetricsCheckReport();

// Best-effort immediate SD snapshot enqueue (non-blocking); returns false on skip/drop.
bool perfMetricsEnqueueSnapshotNow();

// Pause/resume SD CSV snapshot emission without disabling in-RAM counters.
void perfMetricsSetSdCapturePaused(bool paused);
bool perfMetricsIsSdCapturePaused();

// Enable/disable debug prints at runtime
void perfMetricsSetDebug(bool enabled);

/**
 * WiFi manager lifecycle/process implementation and core WiFiManager methods.
 */

#include "wifi_manager_internals.h"
#include "settings.h"
#include "modules/wifi/wifi_static_path_guard.h"
#include "modules/wifi/wifi_auto_timeout_module.h"
#include "modules/wifi/wifi_heap_guard_module.h"
#include "modules/wifi/wifi_stop_lifecycle_policy.h"
#include "esp_wifi.h"

// Optional AP auto-timeout (milliseconds). Set to 0 to keep always-on behavior.
static constexpr unsigned long WIFI_AP_AUTO_TIMEOUT_MS = 0; // e.g., 10 * 60 * 1000 for 10 minutes
static constexpr unsigned long WIFI_AP_INACTIVITY_GRACE_MS =
    60 * 1000; // Require no UI activity/clients for this long before stopping

// --- Static helpers (used only in this TU) ---

static bool shouldUseApSta(const V1Settings& settings) {
    return settings.wifiClientEnabled && settings.hasConfiguredWifiStaSlot();
}

static void getWifiStartThresholds(bool apStaMode, uint32_t& minFree, uint32_t& minBlock) {
    minFree = apStaMode ? WiFiManager::WIFI_START_MIN_FREE_AP_STA : WiFiManager::WIFI_START_MIN_FREE_AP_ONLY;
    minBlock = apStaMode ? WiFiManager::WIFI_START_MIN_BLOCK_AP_STA : WiFiManager::WIFI_START_MIN_BLOCK_AP_ONLY;
}

static void getWifiRuntimeThresholds(bool apStaMode, bool staOnlyMode, uint32_t& minFree, uint32_t& minBlock) {
    if (apStaMode) {
        minFree = WiFiManager::WIFI_RUNTIME_MIN_FREE_AP_STA;
        minBlock = WiFiManager::WIFI_RUNTIME_MIN_BLOCK_AP_STA;
        return;
    }
    if (staOnlyMode) {
        minFree = WiFiManager::WIFI_RUNTIME_MIN_FREE_STA_ONLY;
        minBlock = WiFiManager::WIFI_RUNTIME_MIN_BLOCK_STA_ONLY;
        return;
    }
    minFree = WiFiManager::WIFI_RUNTIME_MIN_FREE_AP_ONLY;
    minBlock = WiFiManager::WIFI_RUNTIME_MIN_BLOCK_AP_ONLY;
}

static WifiHeapGuardModule sWifiHeapGuardModule;
static WifiAutoTimeoutModule sWifiAutoTimeoutModule;

unsigned long WiFiManager::lowDmaCooldownRemainingMs() const {
    if (lowDmaCooldownUntilMs_ == 0) {
        return 0;
    }

    unsigned long now = millis();
    long remaining = static_cast<long>(lowDmaCooldownUntilMs_ - now);
    return (remaining > 0) ? static_cast<unsigned long>(remaining) : 0;
}

bool WiFiManager::canStartSetupMode(uint32_t* freeInternal, uint32_t* largestInternal) const {
    const uint32_t freeNow = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const uint32_t largestNow = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (freeInternal) {
        *freeInternal = freeNow;
    }
    if (largestInternal) {
        *largestInternal = largestNow;
    }

    if (lowDmaCooldownRemainingMs() > 0) {
        return false;
    }

    const V1Settings& settings = settingsManager.get();
    uint32_t minFree = 0;
    uint32_t minBlock = 0;
    getWifiStartThresholds(shouldUseApSta(settings), minFree, minBlock);
    return freeNow >= minFree && largestNow >= minBlock;
}

// Ensure last client seen timestamp advances when UI is accessed
// (called on every HTTP request via checkRateLimit/markUiActivity)

bool WiFiManager::startSetupMode(const bool autoStarted) {
    const V1Settings& settings = settingsManager.get();
    const bool apStaMode = shouldUseApSta(settings);
    const bool restartingFromStopping = (setupModeState_ == SETUP_MODE_STOPPING);
    const auto cancelDeferredStopForRestart = [this]() {
        setupModeState_ = SETUP_MODE_OFF;
        apInterfaceEnabled_ = false;
        wifiStopPhase_ = WifiStopPhase::IDLE;
        wifiStopPhaseStartMs_ = 0;
        wifiStopStartMs_ = 0;
        wifiStopReason_ = "";
        wifiStopManual_ = false;
        wifiStopHadSta_ = false;
        wifiStopHadAp_ = false;
    };

    if (setupModeState_ == SETUP_MODE_AP_ON) {
        if (apInterfaceEnabled_) {
            return true;
        }

        uint32_t freeInternal = 0;
        uint32_t largestInternal = 0;
        if (!canStartSetupMode(&freeInternal, &largestInternal)) {
            Serial.printf("[SetupMode] AP re-enable deferred: free=%lu largest=%lu cooldownMs=%lu\n",
                          static_cast<unsigned long>(freeInternal), static_cast<unsigned long>(largestInternal),
                          static_cast<unsigned long>(lowDmaCooldownRemainingMs()));
            return false;
        }
        if (apStaMode) {
            if (WiFi.getMode() != WIFI_AP_STA) {
                WiFi.mode(WIFI_AP_STA);
            }
        } else if (WiFi.getMode() != WIFI_AP) {
            WiFi.mode(WIFI_AP);
            wifiClientState_ = WIFI_CLIENT_DISABLED;
            currentConnectedSlotIndex_ = -1;
        }

        resetReconnectFailures();
        WiFi.setTxPower(WIFI_POWER_5dBm);
        if (!setupAP()) {
            apInterfaceEnabled_ = false;
            Serial.println("[SetupMode] ABORT: AP re-enable failed");
            return false;
        }
        apInterfaceEnabled_ = true;
        lastClientSeenMs_ = millis();
        lastAnyClientSeenMs_ = lastClientSeenMs_;
        lastApStaCountPollMs_ = 0;
        cachedApStaCount_ = 0;
        wasAutoStarted_ = autoStarted;
        return true;
    }

    if (!apStaMode) {
        Serial.printf("[SetupMode] STA unavailable for this session (wifiClientEnabled=%s ssidLen=%u)\n",
                      settings.wifiClientEnabled ? "true" : "false",
                      static_cast<unsigned>(settings.wifiClientSSID.length()));
    }

    // Check internal SRAM before WiFi init. AP+STA requires more headroom than AP-only.
    const uint32_t freeInternal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const uint32_t largestInternal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint32_t minFree = 0;
    uint32_t minBlock = 0;
    getWifiStartThresholds(apStaMode, minFree, minBlock);
    const unsigned long cooldownMs = lowDmaCooldownRemainingMs();

    Serial.printf("[SetupMode] Start preflight: mode=%s freeInternal=%lu largestInternal=%lu needFree>=%lu "
                  "needLargest>=%lu cooldownMs=%lu\n",
                  apStaMode ? "AP+STA" : "AP", (unsigned long)freeInternal, (unsigned long)largestInternal,
                  (unsigned long)minFree, (unsigned long)minBlock, (unsigned long)cooldownMs);

    if (cooldownMs > 0) {
        Serial.printf("[SetupMode] ABORT: low_dma cooldown active (%lu ms remaining)\n", (unsigned long)cooldownMs);
        return false;
    }

    if (freeInternal < minFree || largestInternal < minBlock) {
        Serial.printf(
            "[SetupMode] ABORT: Insufficient internal SRAM (need free>=%lu largest>=%lu, have free=%lu largest=%lu)\n",
            (unsigned long)minFree, (unsigned long)minBlock, (unsigned long)freeInternal,
            (unsigned long)largestInternal);
        return false; // Graceful fail instead of crash
    }
    if (restartingFromStopping) {
        cancelDeferredStopForRestart();
    }

    setupModeStartTime_ = millis();
    lastClientSeenMs_ = setupModeStartTime_;
    lastAnyClientSeenMs_ = setupModeStartTime_;
    lastApStaCountPollMs_ = 0;
    cachedApStaCount_ = 0;
    lastMaintenanceFastMs_ = 0;
    lastStatusCheckMs_ = 0;
    lastTimeoutCheckMs_ = 0;
    lowDmaSinceMs_ = 0;
    lowDmaCooldownUntilMs_ = 0;
    resetReconnectFailures();

    // Check if WiFi client is enabled - use AP+STA mode
    if (apStaMode) {
        WiFi.mode(WIFI_AP_STA);
        wifiClientState_ = WIFI_CLIENT_DISCONNECTED;
        currentConnectedSlotIndex_ = -1;
    } else {
        WiFi.mode(WIFI_AP);
        wifiClientState_ = WIFI_CLIENT_DISABLED;
        currentConnectedSlotIndex_ = -1;
    }
    WiFi.setTxPower(WIFI_POWER_5dBm);
    Serial.println("[WiFi] TX power 5dBm (low RF for BLE coex)");

    if (!setupAP()) {
        Serial.println("[SetupMode] ABORT: AP bring-up failed");
        WiFi.mode(WIFI_OFF);
        apInterfaceEnabled_ = WifiApLifecyclePolicy::afterBringupAbort(apInterfaceEnabled_);
        wifiClientState_ = WIFI_CLIENT_DISABLED;
        currentConnectedSlotIndex_ = -1;
        return false;
    }
    if (!setupWebServer()) {
        WiFi.mode(WIFI_OFF);
        return false;
    }

    // Collect headers for static caching and guarded maintenance writes.
    const char* headerKeys[] = {
        "Accept-Encoding",
        "If-None-Match",
        maintenanceApiWriteHeader(),
    };
    server_.collectHeaders(headerKeys, 3);

    server_.begin();
    setupModeState_ = SETUP_MODE_AP_ON;
    apInterfaceEnabled_ = true;
    wasAutoStarted_ = autoStarted;

    // Route saved-network rejoin through the same staged STA connect path used
    // everywhere else so AP/STA transitions have one owner.
    if (apStaMode) {
        if (maintenanceBootMode_) {
            (void)beginMaintenanceAutoConnectScan(false);
        } else {
            Serial.println("[SetupMode] STA connect queued");
            (void)connectToNetwork(settings.wifiClientSSID, settingsManager.getWifiClientPassword(), false);
        }
    }

    return true;
}

bool WiFiManager::stopSetupModeImmediate(bool emergencyLowDma) {
    if (emergencyLowDma) {
        // Emergency path: prioritize low-latency return to BLE/display loop.
        lowDmaCooldownUntilMs_ = millis() + WIFI_LOW_DMA_RETRY_COOLDOWN_MS;
        server_.stop();
        WiFi.mode(WIFI_OFF);
        finalizeStopSetupMode();
        return true;
    }

    const wifi_mode_t currentMode = WiFi.getMode();
    const bool modeHasSta = (currentMode == WIFI_AP_STA || currentMode == WIFI_STA);
    const bool modeHasAp = (currentMode == WIFI_AP_STA || currentMode == WIFI_AP);

    // Stop server_ first, then release STA/AP without erasing configured credentials.
    server_.stop();
    if (modeHasSta && (wifiClientState_ == WIFI_CLIENT_CONNECTED || wifiClientState_ == WIFI_CLIENT_CONNECTING ||
                       WiFi.status() == WL_CONNECTED)) {
        WiFi.disconnect(false, false);
        currentConnectedSlotIndex_ = -1;
    }
    if (WifiApLifecyclePolicy::shouldDisableInterfaceOnStop(modeHasAp, apInterfaceEnabled_)) {
        if (!WiFi.enableAP(false)) {
            WiFi.softAPdisconnect(true);
        }
    }

    WiFi.mode(WIFI_OFF);
    finalizeStopSetupMode();
    return true;
}

void WiFiManager::finalizeStopSetupMode() {
    const String stopReason = wifiStopReason_;
    const bool stopManual = wifiStopManual_;
    const uint32_t stopDurMs = millis() - wifiStopStartMs_;

    // ============================================================================
    // RESET ALL STATE
    // ============================================================================
    setupModeState_ = SETUP_MODE_OFF;
    apInterfaceEnabled_ = false;
    wifiClientState_ = WIFI_CLIENT_DISABLED;
    currentConnectedSlotIndex_ = -1;
    resetWifiScanState();
    wifiConnectStartMs_ = 0;
    wifiConnectPhase_ = WifiConnectPhase::IDLE;
    wifiConnectPhaseStartMs_ = 0;
    pendingConnectSSID_ = "";
    pendingConnectPassword_ = "";
    pendingConnectPersistCredentials_ = true;
    pendingConnectSlotIndex_ = -1;
    maintenanceAutoConnectStaDropGate_.clear();
    lastUiActivityMs_ = 0;
    lastClientSeenMs_ = 0;
    lastAnyClientSeenMs_ = 0;
    lastApStaCountPollMs_ = 0;
    cachedApStaCount_ = 0;
    lastMaintenanceFastMs_ = 0;
    lastStatusCheckMs_ = 0;
    lastTimeoutCheckMs_ = 0;
    lastReconnectAttemptMs_ = 0;
    wifiReconnectDeferredLogged_ = false;
    wasAutoStarted_ = false;
    lowDmaSinceMs_ = 0;
    wifiStopPhase_ = WifiStopPhase::IDLE;
    wifiStopPhaseStartMs_ = 0;
    wifiStopStartMs_ = 0;
    wifiStopReason_ = "";
    wifiStopManual_ = false;
    wifiStopHadSta_ = false;
    wifiStopHadAp_ = false;
    BackupApiService::releaseBackupSnapshotCache(cachedBackupSnapshot_);
    WifiStatusApiService::releaseStatusJsonCache(cachedStatusJson_, lastStatusJsonTime_);

    // ============================================================================
    // OBSERVABILITY
    // ============================================================================
    uint32_t freeInternalAfter = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint32_t largestInternalAfter = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    Serial.printf("[SetupMode] WiFi OFF: reason=%s manual=%d radio=%d http=%d freeDma=%lu largestDma=%lu durMs=%lu\n",
                  stopReason.length() ? stopReason.c_str() : "unknown", stopManual ? 1 : 0, 0, 0,
                  (unsigned long)freeInternalAfter, (unsigned long)largestInternalAfter, (unsigned long)stopDurMs);
}

void WiFiManager::processStopSetupModePhase() {
    const unsigned long now = millis();
    const WifiStopLifecyclePolicy::PhaseInput phaseInput{
        wifiStopPhase_ == WifiStopPhase::IDLE,
        wifiStopPhase_ == WifiStopPhase::STOP_HTTP_SERVER,
        now,
        wifiStopPhaseStartMs_,
        WIFI_STOP_PHASE_SETTLE_MS,
    };
    if (!WifiStopLifecyclePolicy::shouldExecutePhase(phaseInput)) {
        return;
    }

    switch (wifiStopPhase_) {
    case WifiStopPhase::STOP_HTTP_SERVER: {
        server_.stop();
        wifiStopPhase_ = WifiStopPhase::DISCONNECT_STA;
        wifiStopPhaseStartMs_ = now;
        break;
    }

    case WifiStopPhase::DISCONNECT_STA: {
        if (wifiStopHadSta_ && (wifiClientState_ == WIFI_CLIENT_CONNECTED ||
                                wifiClientState_ == WIFI_CLIENT_CONNECTING || WiFi.status() == WL_CONNECTED)) {
            WiFi.disconnect(false, false);
            currentConnectedSlotIndex_ = -1;
        }
        wifiStopPhase_ = WifiStopPhase::DISABLE_AP;
        wifiStopPhaseStartMs_ = now;
        break;
    }

    case WifiStopPhase::DISABLE_AP: {
        if (WifiApLifecyclePolicy::shouldDisableInterfaceOnStop(wifiStopHadAp_, apInterfaceEnabled_)) {
            if (!WiFi.enableAP(false)) {
                WiFi.softAPdisconnect(true);
            }
            apInterfaceEnabled_ = false;
            cachedApStaCount_ = 0;
            lastApStaCountPollMs_ = 0;
        }
        wifiStopPhase_ = WifiStopPhase::MODE_OFF;
        wifiStopPhaseStartMs_ = now;
        break;
    }

    case WifiStopPhase::MODE_OFF: {
        WiFi.mode(WIFI_OFF);
        wifiStopPhase_ = WifiStopPhase::FINALIZE;
        wifiStopPhaseStartMs_ = now;
        break;
    }

    case WifiStopPhase::FINALIZE:
        finalizeStopSetupMode();
        break;

    case WifiStopPhase::IDLE:
    default:
        break;
    }
}

bool WiFiManager::stopSetupMode(bool manual, const char* reason) {
    const WifiStopLifecyclePolicy::RequestDecision stopDecision = WifiStopLifecyclePolicy::evaluateRequest({
        setupModeState_ == SETUP_MODE_AP_ON,
        setupModeState_ == SETUP_MODE_STOPPING,
        wifiStopPhase_ != WifiStopPhase::IDLE,
        manual,
        reason,
    });
    if (stopDecision.action == WifiStopLifecyclePolicy::RequestAction::REJECT) {
        return false;
    }

    const char* stopReason = stopDecision.reason;
    const bool forceImmediate = stopDecision.action == WifiStopLifecyclePolicy::RequestAction::STOP_IMMEDIATE;
    const uint32_t stopStartMs = millis();
    uint32_t freeInternalBefore = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint32_t largestInternalBefore = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    Serial.printf("[SetupMode] Stopping WiFi: reason=%s manual=%d freeDma=%lu largestDma=%lu\n", stopReason,
                  manual ? 1 : 0, (unsigned long)freeInternalBefore, (unsigned long)largestInternalBefore);

    if (stopDecision.escalatesPendingStop) {
        wifiStopReason_ = stopReason;
        wifiStopManual_ = manual;
        wifiStopStartMs_ = stopStartMs;
        return stopSetupModeImmediate(stopDecision.emergencyLowDma);
    }
    if (stopDecision.action == WifiStopLifecyclePolicy::RequestAction::ACKNOWLEDGE_PENDING) {
        return true;
    }

    lowDmaSinceMs_ = 0;
    wifiStopReason_ = stopReason;
    wifiStopManual_ = manual;
    wifiStopStartMs_ = stopStartMs;

    if (forceImmediate) {
        return stopSetupModeImmediate(stopDecision.emergencyLowDma);
    }

    setupModeState_ = SETUP_MODE_STOPPING;
    lastUiActivityMs_ = 0;
    wifiConnectStartMs_ = 0;
    wifiConnectPhase_ = WifiConnectPhase::IDLE;
    wifiConnectPhaseStartMs_ = 0;
    pendingConnectSSID_ = "";
    pendingConnectPassword_ = "";
    pendingConnectPersistCredentials_ = true;
    pendingConnectSlotIndex_ = -1;

    const wifi_mode_t currentMode = WiFi.getMode();
    wifiStopHadSta_ = (currentMode == WIFI_AP_STA || currentMode == WIFI_STA);
    wifiStopHadAp_ = (currentMode == WIFI_AP_STA || currentMode == WIFI_AP || apInterfaceEnabled_);
    wifiStopPhase_ = WifiStopPhase::STOP_HTTP_SERVER;
    wifiStopPhaseStartMs_ = stopStartMs;
    return true;
}

bool WiFiManager::setupAP() {
    // Use saved SSID/password when available; fall back to defaults if missing/too short
    const V1Settings& settings = settingsManager.get();
    String apSSID = settings.apSSID.length() ? settings.apSSID : "V1-Simple";
    String apPass = (settings.apPassword.length() >= 8) ? settings.apPassword : "setupv1simple"; // WPA2 requires 8+

    // Configure AP IP
    IPAddress apIP(192, 168, 35, 5);
    IPAddress gateway(192, 168, 35, 5);
    IPAddress subnet(255, 255, 255, 0);

    if (!WiFi.softAPConfig(apIP, gateway, subnet)) {
        Serial.println("[SetupMode] ERROR: softAPConfig failed");
        return false;
    }

    if (!WiFi.softAP(apSSID.c_str(), apPass.c_str())) {
        Serial.println("[SetupMode] ERROR: softAP start failed");
        return false;
    }
    return true;
}

void WiFiManager::checkAutoTimeout() {
    uint8_t timeoutMins = settingsManager.getApTimeoutMinutes();
    if (timeoutMins == 0)
        return; // Disabled (always on)
    if (!isSetupModeActive())
        return;

    unsigned long now = millis();
    int staCount = cachedApStaCount_;
    if (lastApStaCountPollMs_ == 0 || (now - lastApStaCountPollMs_) >= AP_STA_COUNT_POLL_MS) {
        // softAPgetStationNum() can be slow on some loops; cache short-term to
        // reduce loop jitter without changing timeout semantics materially.
        staCount = WiFi.softAPgetStationNum();
        cachedApStaCount_ = staCount;
        lastApStaCountPollMs_ = now;
    }
    if (staCount > 0) {
        lastClientSeenMs_ = now;
    }

    WifiAutoTimeoutInput timeoutInput;
    timeoutInput.timeoutMins = timeoutMins;
    timeoutInput.setupModeActive = isSetupModeActive();
    timeoutInput.maintenanceBootMode = maintenanceBootMode_;
    timeoutInput.nowMs = now;
    timeoutInput.setupModeStartMs = setupModeStartTime_;
    timeoutInput.lastClientSeenMs = lastClientSeenMs_;
    timeoutInput.lastUiActivityMs = lastUiActivityMs_;
    timeoutInput.staCount = staCount;
    timeoutInput.inactivityGraceMs = WIFI_AP_INACTIVITY_GRACE_MS;
    const WifiAutoTimeoutResult timeoutResult = sWifiAutoTimeoutModule.evaluate(timeoutInput);

    if (timeoutResult.shouldStop) {
        Serial.println("[SetupMode] Auto-timeout reached - stopping AP");
        stopSetupMode(false, "timeout");
    }
}

void WiFiManager::process() {
    if (setupModeState_ != SETUP_MODE_AP_ON && setupModeState_ != SETUP_MODE_STOPPING) {
        lowDmaSinceMs_ = 0;
        return; // No WiFi processing when Setup Mode is off
    }

    // Graceful shutdown runs as a staged sequence to avoid long stop-time stalls.
    if (wifiStopPhase_ != WifiStopPhase::IDLE) {
        processStopSetupModePhase();
        return;
    }
    if (setupModeState_ == SETUP_MODE_STOPPING) {
        lowDmaSinceMs_ = 0;
        return;
    }

    // Runtime SRAM guard with persistence + mode-aware thresholds:
    // AP+STA needs more memory than AP-only, and short dips should not force shutdown.
    const wifi_mode_t mode = WiFi.getMode();
    const bool staRadioOn = (mode == WIFI_AP_STA || mode == WIFI_STA);
    const bool dualRadioMode = isSetupModeActive() && staRadioOn;
    const bool staOnlyMode = staRadioOn && !dualRadioMode;
    uint32_t criticalFree = 0;
    uint32_t criticalBlock = 0;
    getWifiRuntimeThresholds(dualRadioMode, staOnlyMode, criticalFree, criticalBlock);

    const uint32_t freeInternal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const uint32_t largestInternal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    WifiHeapGuardInput heapGuardInput;
    heapGuardInput.dualRadioMode = dualRadioMode;
    heapGuardInput.staRadioOn = staRadioOn;
    heapGuardInput.staOnlyMode = staOnlyMode;
    heapGuardInput.freeInternal = freeInternal;
    heapGuardInput.largestInternal = largestInternal;
    heapGuardInput.criticalFree = criticalFree;
    heapGuardInput.criticalBlock = criticalBlock;
    heapGuardInput.apStaFreeJitterTolerance = WIFI_RUNTIME_AP_STA_FREE_JITTER_TOLERANCE;
    heapGuardInput.staOnlyBlockJitterTolerance = WIFI_RUNTIME_STA_BLOCK_JITTER_TOLERANCE;
    const WifiHeapGuardResult heapGuard = sWifiHeapGuardModule.evaluate(heapGuardInput);
    const bool lowHeap = heapGuard.lowHeap;

    if (lowHeap) {
        const unsigned long now = millis();
        if (lowDmaSinceMs_ == 0) {
            lowDmaSinceMs_ = now;
            Serial.printf("[WiFi] WARN: Internal SRAM low (mode=%s free=%lu block=%lu need>=%lu/%lu) - grace %lu ms\n",
                          heapGuard.modeLabel, (unsigned long)freeInternal, (unsigned long)largestInternal,
                          (unsigned long)criticalFree, (unsigned long)criticalBlock,
                          (unsigned long)WIFI_LOW_DMA_PERSIST_MS);
        } else if ((now - lowDmaSinceMs_) >= WIFI_LOW_DMA_PERSIST_MS) {
            Serial.printf("[WiFi] CRITICAL: Internal SRAM low for %lu ms (free=%lu block=%lu) - stopping WiFi\n",
                          (unsigned long)(now - lowDmaSinceMs_), (unsigned long)freeInternal,
                          (unsigned long)largestInternal);

            // In AP+STA mode, drop AP first to preserve STA utility under pressure.
            if (dualRadioMode) {
                Serial.println("[WiFi] ACTION: dropping AP due to sustained low SRAM (keeping STA online)");
                if (!WiFi.enableAP(false)) {
                    Serial.println("[WiFi] WARN: enableAP(false) failed during low-SRAM AP drop; falling back to "
                                   "softAPdisconnect");
                    WiFi.softAPdisconnect(true);
                }
                apInterfaceEnabled_ = false;

                const wl_status_t staStatus = WiFi.status();
                if (staStatus == WL_CONNECTED) {
                    wifiClientState_ = WIFI_CLIENT_CONNECTED;
                    wifiReconnectFailures_ = 0;
                } else if (wifiClientState_ == WIFI_CLIENT_CONNECTING) {
                    // Keep connect workflow active and let status polling settle.
                    wifiClientState_ = WIFI_CLIENT_CONNECTING;
                } else {
                    wifiClientState_ = WIFI_CLIENT_DISCONNECTED;
                    currentConnectedSlotIndex_ = -1;
                }

                // Cancel staged mode-switch workflow; reconnect logic can resume normally.
                wifiConnectPhase_ = WifiConnectPhase::IDLE;
                wifiConnectPhaseStartMs_ = 0;
                lowDmaCooldownUntilMs_ = now + WIFI_LOW_DMA_RETRY_COOLDOWN_MS;
                lowDmaSinceMs_ = 0;
                Serial.printf("[WiFi] AP dropped; STA status=%s\n", wifiClientStateApiName(wifiClientState_));
                return;
            }

            stopSetupMode(false, "low_dma"); // Graceful shutdown to free memory
            return;
        }
    } else if (lowDmaSinceMs_ != 0) {
        const unsigned long lowDuration = millis() - lowDmaSinceMs_;
        Serial.printf("[WiFi] RECOVERED: Internal SRAM back above threshold after %lu ms\n",
                      (unsigned long)lowDuration);
        lowDmaSinceMs_ = 0;
    }

    const unsigned long now = millis();
    int apClientCount = 0;
    const bool apInterfaceActive = isSetupModeActive();
    if (apInterfaceActive) {
        if (lastApStaCountPollMs_ == 0 || (now - lastApStaCountPollMs_) >= AP_STA_COUNT_POLL_MS) {
            cachedApStaCount_ = WiFi.softAPgetStationNum();
            lastApStaCountPollMs_ = now;
        }
        apClientCount = cachedApStaCount_;
    } else {
        cachedApStaCount_ = 0;
    }

    const bool staConnectedNow = (wifiClientState_ == WIFI_CLIENT_CONNECTED) || (WiFi.status() == WL_CONNECTED);
    if (apInterfaceActive && apClientCount > 0) {
        lastClientSeenMs_ = now;
    }

    if (!maintenanceBootMode_ && apInterfaceActive && staConnectedNow && apClientCount == 0 && lastClientSeenMs_ != 0 &&
        (now - lastClientSeenMs_) >= WIFI_AP_IDLE_DROP_AFTER_STA_MS) {
        Serial.printf("[WiFi] STA connected and AP idle for %lu ms - dropping AP\n",
                      static_cast<unsigned long>(WIFI_AP_IDLE_DROP_AFTER_STA_MS));
        const bool staWasConnected = (WiFi.status() == WL_CONNECTED);
        if (!WiFi.enableAP(false)) {
            Serial.println(
                "[WiFi] WARN: enableAP(false) failed during idle AP retire; falling back to softAPdisconnect");
            WiFi.softAPdisconnect(true);
        }
        apInterfaceEnabled_ = false;
        cachedApStaCount_ = 0;
        lastApStaCountPollMs_ = 0;
        if (staWasConnected && WiFi.status() != WL_CONNECTED) {
            Serial.println("[WiFi] WARN: STA dropped during AP retire; reconnecting");
            wifiClientState_ = WIFI_CLIENT_DISCONNECTED;
            currentConnectedSlotIndex_ = -1;
            const V1Settings& settings = settingsManager.get();
            if (settings.wifiClientEnabled && settings.wifiClientSSID.length() > 0) {
                String savedPassword = settingsManager.getWifiClientPassword();
                connectToNetwork(settings.wifiClientSSID, savedPassword, false);
            }
        }
        return;
    }

    WifiNoClientTimeoutInput noClientInput;
    noClientInput.maintenanceBootMode = maintenanceBootMode_;
    noClientInput.clientPresent = staConnectedNow || apClientCount > 0;
    noClientInput.staConnectInProgress = wifiClientState_ == WIFI_CLIENT_CONNECTING;
    noClientInput.autoStarted = wasAutoStarted_;
    noClientInput.nowMs = now;
    noClientInput.lastAnyClientSeenMs = lastAnyClientSeenMs_;
    noClientInput.manualTimeoutMs = WIFI_NO_CLIENT_SHUTDOWN_MS;
    noClientInput.autoTimeoutMs = WIFI_NO_CLIENT_SHUTDOWN_AUTO_MS;
    const WifiNoClientTimeoutResult noClientResult = sWifiAutoTimeoutModule.evaluateNoClient(noClientInput);

    if (noClientResult.refreshLastSeen) {
        lastAnyClientSeenMs_ = now;
    } else if (noClientResult.shouldStop) {
        Serial.printf("[WiFi] No AP/STA clients for %lu ms (%s) - stopping WiFi\n",
                      static_cast<unsigned long>(noClientResult.timeoutMs), wasAutoStarted_ ? "auto-start" : "manual");
        stopSetupMode(false, wasAutoStarted_ ? "no_clients_auto" : "no_clients");
        return;
    }

    // Continue serving HTTP while STA remains online even after AP is retired.
    if (isWifiServiceActive()) {
        if (maintenanceBootMode_) {
            processMaintenanceAutoConnect();
        }

        server_.handleClient();
    }

    if (lastMaintenanceFastMs_ == 0 || (now - lastMaintenanceFastMs_) >= WIFI_MAINTENANCE_FAST_MS) {
        processMaintenanceAutoConnect();
        processWifiClientConnectPhase();
        lastMaintenanceFastMs_ = now;
    }
    if (lastTimeoutCheckMs_ == 0 || (now - lastTimeoutCheckMs_) >= WIFI_TIMEOUT_CHECK_MS) {
        checkAutoTimeout();
        lastTimeoutCheckMs_ = now;
    }

    // Check WiFi client (STA) status at a moderate cadence to avoid tight-loop
    // status polling jitter while preserving reconnect responsiveness.
    if (lastStatusCheckMs_ == 0 || (now - lastStatusCheckMs_) >= WIFI_STATUS_CHECK_MS) {
        checkWifiClientStatus();
        lastStatusCheckMs_ = now;
    }
}
// ============================================================================
// API Endpoints
// ============================================================================

void WiFiManager::handleNotFound() {
    String uri = server_.uri();

    if (!WifiStaticPathGuard::isAllowedServedPath(uri.c_str())) {
        if (!WifiStaticPathGuard::isSafe(uri.c_str())) {
            Serial.printf("[HTTP] REJECT unsafe path %s\n", uri.c_str());
        } else {
            Serial.printf("[HTTP] REJECT unlisted static path %s\n", uri.c_str());
        }
        server_.send(404, "text/plain", "Not found");
        return;
    }

    if (WifiStaticPathGuard::isHtmlPagePath(uri.c_str())) {
        String path = uri;
        if (uri == "/") {
            path = "/index.html";
        } else if (uri.indexOf('.') == -1) {
            path = uri + ".html";
        }
        if (serveLittleFSFile(path.c_str(), "text/html")) {
            return;
        }
    }

    // Try to serve static files (js, css, json, etc.)
    String contentType = "application/octet-stream";
    if (uri.endsWith(".js"))
        contentType = "application/javascript";
    else if (uri.endsWith(".css"))
        contentType = "text/css";
    else if (uri.endsWith(".json"))
        contentType = "application/json";
    else if (uri.endsWith(".html"))
        contentType = "text/html";
    else if (uri.endsWith(".svg"))
        contentType = "image/svg+xml";
    else if (uri.endsWith(".png"))
        contentType = "image/png";
    else if (uri.endsWith(".ico"))
        contentType = "image/x-icon";

    if (serveLittleFSFile(uri.c_str(), contentType.c_str())) {
        return;
    }

    server_.send(404, "text/plain", "Not found");
}

bool WiFiManager::serveLittleFSFile(const char* path, const char* contentType) {
    return serveLittleFSFileHelper(server_, path, contentType);
}

// ============================================================================
// Auto-Push Handlers
// ============================================================================

// ============================================================================
// Display Colors Handlers
// ============================================================================

// ============================================================================
// Core WiFiManager methods
// ============================================================================

WiFiManager::WiFiManager()
    : server_(80), setupModeState_(SETUP_MODE_OFF), apInterfaceEnabled_(false), setupModeStartTime_(0) {}

bool WiFiManager::isStopping() const {
    return setupModeState_ == SETUP_MODE_STOPPING;
}

// Mutation rate limiting: returns true if the write is allowed, false if rate limited.
// Read-only status polls call markUiActivity() directly and never enter this window.
bool WiFiManager::checkRateLimit() {
    const uint32_t now = millis();

    // Admitted and rejected mutations both prove that the UI is active.
    markUiActivity();

    const SlidingWindowRateLimitDecision decision = rateLimiter_.evaluate(now);
    if (!decision.allowed) {
        const unsigned long roundedRetryAfter = static_cast<unsigned long>((decision.retryAfterMs + 999u) / 1000u);
        const unsigned long retryAfterSec = (roundedRetryAfter == 0) ? 1 : roundedRetryAfter;
        server_.sendHeader("Retry-After", String(retryAfterSec));
        server_.send(429, "application/json", "{\"success\":false,\"message\":\"Too many requests\"}");
        return false;
    }

    return true;
}

// Web activity tracking for maintenance-session idle deadlines
void WiFiManager::markUiActivity() {
    lastUiActivityMs_ = millis();
}

bool WiFiManager::isUiActive(unsigned long timeoutMs) const {
    return wifiUiActiveSince(lastUiActivityMs_, millis(), timeoutMs);
}

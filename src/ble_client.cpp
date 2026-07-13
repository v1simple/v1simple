/**
 * BLE Client for Valentine1 Gen2
 * With BLE Server proxy support for companion app
 *
 * Architecture:
 * - NimBLE 2.3.7 tuned for stable dual-role operation
 * - Client connects to V1 (V1G* device names)
 * - Server advertises with a V1-compatible name for companion apps
 * - FreeRTOS task manages advertising timing
 * - Thread-safe with mutexes for BLE operations
 *
 * Key Features:
 * - Automatic V1 discovery and reconnection
 * - Bidirectional proxy (V1 ↔ app)
 * - Profile settings push
 * - Mode control (All Bogeys/Logic/Advanced Logic)
 * - Mute toggle
 */

#include "ble_client.h"
#include "ble_bond_backup_store.h"
#include "ble_bond_backup_writer.h"
#include "ble_fresh_flash_policy.h"
#include "settings.h"
#include "perf_metrics.h"
#include "storage_manager.h"
#include "config.h"
#include <Arduino.h>
#include <WiFi.h>  // For WiFi coexistence during BLE connect
#include <Preferences.h>  // For fresh-flash detection
#include <set>
#include <string>
#include <cstdlib>
#include <cstring>
#include "ble_internals.h"

// NimBLE low-level store API for bond backup/restore
extern "C" {
#include "nimble/nimble/host/include/host/ble_store.h"
#include "nimble/nimble/host/include/host/ble_sm.h"
}

// ============================================================================
// BLE Bond Backup/Restore (SD card)
// ============================================================================
// NimBLE stores bonds in NVS ("nimble_bond" namespace). NVS is volatile —
// brownouts, partition changes, and flash erases lose all bonds. This backs
// up bond key material to SD so it can be restored automatically.
//
// File format: /v1simple_ble_bonds.bin
//   [4 bytes]  magic "BLB\x01" (BLE Bonds v1)
//   [4 bytes]  uint32_t  ourSecCount
//   [4 bytes]  uint32_t  peerSecCount
//   [N * sizeof(ble_store_value_sec)]  our_sec entries
//   [M * sizeof(ble_store_value_sec)]  peer_sec entries
// ============================================================================

static constexpr const char* BLE_BOND_BACKUP_PATH = "/v1simple_ble_bonds.bin";
// Max TX power (21 dBm) to maximize BLE range.
// ESP32-S3 valid steps: -12, -9, -6, -3, 0, 3, 6, 9, 12, 15, 18, 21.
static constexpr int8_t BLE_TX_POWER_DBM = 21;

int refreshBleBondBackup() {
    return enqueueCurrentBleBondBackupSnapshot();
}

int V1BLEClient::enqueueCurrentBondBackupSnapshot() {
    return enqueueCurrentBleBondBackupSnapshot();
}

// Restore bond keys from SD card. Must be called after NimBLEDevice::init()
// but before scanning/connecting. Returns number of bonds restored, or -1 on error.
static int restoreBondsFromSD() {
    if (!storageManager.isReady() || !storageManager.isSDCard()) {
        return -1;
    }

    StorageManager::SDLockBlocking sdLock(storageManager.getSDMutex());
    if (!sdLock) {
        return -1;
    }

    fs::FS* sdFs = storageManager.getFilesystem();
    if (!sdFs) {
        return -1;
    }
    const int restored = restoreBleBondBackup(*sdFs, BLE_BOND_BACKUP_PATH);
    if (restored < 0) {
        return -1;
    }

    if (restored > 0) {
        Serial.printf("[BLE] Restored %d bond(s) from SD backup\n", restored);
    }
    return restored;
}
// Spinlock for deferring settings writes from BLE scan callbacks
portMUX_TYPE pendingAddrMux = portMUX_INITIALIZER_UNLOCKED;

// Instance pointer for callbacks (extern in ble_internals.h)
V1BLEClient* instancePtr = nullptr;

V1BLEClient::V1BLEClient()
    : pClient_(nullptr)
    , pRemoteService_(nullptr)
    , pDisplayDataChar_(nullptr)
    , pCommandChar_(nullptr)
    , pCommandCharLong_(nullptr)
    , pServer_(nullptr)
    , pProxyService_(nullptr)
    , pProxyNotifyChar_(nullptr)
    , pProxyNotifyLongChar_(nullptr)
    , pProxyWriteChar_(nullptr)
    , proxyEnabled_(false)
    , proxyServerInitialized_(false)
    , proxyServerInitAttempted_(false)
    // proxyClientConnected_ - uses default member initializer (atomic)
    , proxyName_("V1-Proxy")
    , proxyQueue_(nullptr)
    , phone2v1Queue_(nullptr)
    , proxyQueuesInPsram_(false)
    , dataCallback_(nullptr)
    , connectImmediateCallback_(nullptr)
    , connectStableCallback_(nullptr)
    // connected_, shouldConnect_ - use default member initializers (atomic)
    , hasTargetDevice_(false)
    , targetAddress_()
    , lastScanStart_(0)
    , freshFlashBoot_(false)
    , pScanCallbacks_(nullptr)
    , pClientCallbacks_(nullptr)
    , pProxyServerCallbacks_(nullptr)
    , pProxyWriteCallbacks_(nullptr) {
    instancePtr = this;
}

V1BLEClient::~V1BLEClient() {
    releaseProxyQueues();
    if (instancePtr == this) {
        instancePtr = nullptr;
    }
}

const char* V1BLEClient::getSubscribeStepName() const {
    switch (subscribeStep_) {
        case SubscribeStep::GET_SERVICE:
            return "GET_SERVICE";
        case SubscribeStep::GET_DISPLAY_CHAR:
            return "GET_DISPLAY_CHAR";
        case SubscribeStep::GET_COMMAND_CHAR:
            return "GET_COMMAND_CHAR";
        case SubscribeStep::GET_COMMAND_LONG:
            return "GET_COMMAND_LONG";
        case SubscribeStep::SUBSCRIBE_DISPLAY:
            return "SUBSCRIBE_DISPLAY";
        case SubscribeStep::GET_DISPLAY_LONG:
            return "GET_DISPLAY_LONG";
        case SubscribeStep::SUBSCRIBE_LONG:
            return "SUBSCRIBE_LONG";
        case SubscribeStep::REQUEST_ALERT_DATA:
            return "REQUEST_ALERT_DATA";
        case SubscribeStep::REQUEST_VERSION:
            return "REQUEST_VERSION";
        case SubscribeStep::COMPLETE:
            return "COMPLETE";
        default:
            return "UNKNOWN";
    }
}

// ============================================================================
// BLE State Machine
// ============================================================================

void V1BLEClient::setBLEState(BLEState newState, const char* reason) {
    BLEState oldState = bleState_;
    if (oldState == newState) return;  // No change

    const uint32_t now = static_cast<uint32_t>(millis());
    const uint32_t stateTime =
        (oldState != BLEState::DISCONNECTED && stateEnteredMs_ > 0) ? (now - stateEnteredMs_) : 0;

    bleState_ = newState;
    stateEnteredMs_ = now;
    if (newState == BLEState::SCAN_STOPPING || oldState == BLEState::SCAN_STOPPING) {
        scanStopResultsCleared_ = false;
    }

    if (newState == BLEState::SCANNING) {
        PERF_INC(bleScanStateEntries);
    }
    if (oldState == BLEState::SCANNING && newState != BLEState::SCANNING) {
        PERF_INC(bleScanStateExits);
        PERF_MAX(bleScanDwellMaxMs, stateTime);
    }

    if (newState == BLEState::SCANNING) {
        perfRecordBleTimelineEvent(PerfBleTimelineEvent::ScanStart, now);
    } else if (newState == BLEState::SCAN_STOPPING && reason && strstr(reason, "V1 found")) {
        PERF_INC(bleScanTargetFound);
        perfRecordBleTimelineEvent(PerfBleTimelineEvent::TargetFound, now);
    } else if (newState == BLEState::CONNECTING) {
        perfRecordBleTimelineEvent(PerfBleTimelineEvent::ConnectStart, now);
    } else if (newState == BLEState::CONNECTED) {
        perfRecordBleTimelineEvent(PerfBleTimelineEvent::Connected, now);
    }
    if (oldState == BLEState::SCANNING &&
        newState == BLEState::DISCONNECTED &&
        reason &&
        strstr(reason, "scan ended without finding V1")) {
        PERF_INC(bleScanNoTargetExits);
    }

}

// Full cleanup of BLE connection state - call before retry or after failures
void V1BLEClient::cleanupConnection() {
    // processClientQuiesce() is the sole caller. It reaches this point only
    // after discovery has exited and an active link has delivered its
    // disconnect callback. Remote attribute pointers are main-loop-owned, so
    // no callback can clear one between a check and dereference.
    pDisplayDataChar_ = nullptr;
    pCommandChar_ = nullptr;
    pCommandCharLong_ = nullptr;
    pRemoteService_ = nullptr;
    scanStopResultsCleared_ = false;
    // Publish null callback mappings before a future connect can delete/rebuild
    // NimBLE's cached service objects.
    notifyShortCharId_.store(0, std::memory_order_release);
    notifyShortChar_.store(nullptr, std::memory_order_release);
    notifyLongCharId_.store(0, std::memory_order_release);
    notifyLongChar_.store(nullptr, std::memory_order_release);
    connected_.store(false, std::memory_order_release);
    {
        SemaphoreGuard lock(bleMutex_, pdMS_TO_TICKS(20));  // COLD: disconnect cleanup
        if (lock.locked()) {
            shouldConnect_ = false;
            hasTargetDevice_ = false;
            targetDevice_ = NimBLEAdvertisedDevice();
        }
    }

    // 5. Clear stale phone command state (prevents sending commands from previous session)
    phoneCmdPendingClear_ = true;

    connectInProgress_ = false;
    connectedFollowupStep_ = ConnectedFollowupStep::NONE;
}

// Request a hard reset after repeated failures. Teardown is intentionally
// non-blocking: processClientQuiesce() completes it only after discovery and
// disconnect callbacks have released the shared NimBLE client.
void V1BLEClient::hardResetBLEClient() {
    Serial.println("[BLE] Hard reset...");
    beginClientQuiesce("hard reset requested", true);
}

// Reapply reusable-client configuration after quiescence. NimBLE has a fixed
// client-slot array, so destroying/recreating a healthy slot is deliberately
// avoided.
void V1BLEClient::completeHardResetBLEClient() {

    // Stop any active scanning
    NimBLEScan* pScan = NimBLEDevice::getScan();
    if (pScan && pScan->isScanning()) {
        pScan->stop();
    }

    // Reuse existing client (don't destroy - NimBLE has fixed 3-slot array,
    // nulling without deleteClient leaks a slot permanently)
    if (!pClient_) {
        pClient_ = NimBLEDevice::createClient();
    }
    if (pClient_) {
        if (!pClientCallbacks_) {
            pClientCallbacks_.reset(new ClientCallbacks());
        }
        pClient_->setClientCallbacks(pClientCallbacks_.get());
        // Connection parameters: 12-24 (15-30ms interval), balanced for stability
        pClient_->setConnectionParams(NIMBLE_CONN_INTERVAL_MIN,
                                     NIMBLE_CONN_INTERVAL_MAX,
                                     NIMBLE_CONN_LATENCY,
                                     NIMBLE_CONN_SUPERVISION_TIMEOUT);
        pClient_->setConnectTimeout(NIMBLE_CONNECT_TIMEOUT_INIT_MS);
        pClient_->setConnectRetries(0);  // Project manages retries via MAX_CONNECT_ATTEMPTS
    } else {
        Serial.println("[BLE] ERROR: Failed to create client!");
    }

    // Reset failure counter after hard reset
    consecutiveConnectFailures_ = 0;
    nextConnectAllowedMs_ = 0;

}

// Initialize BLE stack without starting scan
bool V1BLEClient::initBLE(bool enableProxy, const char* proxyName) {
    static bool initialized = false;
    if (initialized) {
        return true;  // Already initialized
    }

    Serial.print("[BLE] Init...");

    proxyEnabled_ = enableProxy;
    proxyName_ = proxyName ? proxyName : "V1C-LE-S3";
    bool needsFreshFlashBondReset = false;

    // Create mutexes for thread-safe BLE operations (only once)
    if (!bleMutex_) {
        bleMutex_ = xSemaphoreCreateMutex();
    }
    if (!bleNotifyMutex_) {
        bleNotifyMutex_ = xSemaphoreCreateMutex();
    }
    if (!phoneCmdMutex_) {
        phoneCmdMutex_ = xSemaphoreCreateMutex();
    }

    if (!bleMutex_ || !bleNotifyMutex_ || !phoneCmdMutex_) {
        Serial.println("FAIL");
        return false;
    }

    // Fresh-flash detection: stage BLE bond reset if firmware version changed.
    // The actual delete happens only after the normal NimBLE init path so the
    // stack is brought up once per boot.
    {
        Preferences blePrefs;
        if (blePrefs.begin(BleFreshFlashPolicy::kNamespace, false)) {  // Read-write mode
            needsFreshFlashBondReset =
                BleFreshFlashPolicy::hasFirmwareVersionMismatch(blePrefs, FIRMWARE_VERSION);
            blePrefs.end();
        }
    }

    // BLE initialization pattern for NimBLE dual-role stability:
    // 1. init() with a generic name
    // 2. setDeviceName() with the actual advertised name
    // 3. setPower() and setMTU for better throughput
    // 4. Create the proxy GATT server BEFORE scanning, even when the saved
    //    runtime mode has proxy disabled. NimBLE 2.5 resets GATT when a server
    //    is first advertised; doing that after scan/connect has begun can trip
    //    ble_svc_gap_init() assertions.
    // 5. Start advertising then stop (initializes BLE stack)
    // 6. Runtime proxy toggles only allocate/release queues and start/stop
    //    advertising; they must not create the server after boot.
    NimBLEDevice::init(proxyEnabled_ ? "V1 Proxy" : "V1Display");
    NimBLEDevice::setDeviceName(proxyName_.c_str());
    // NimBLE-Arduino expects dBm here, not esp_power_level_t enum indices.
    // 9 dBm is a supported ESP32-S3 step.
    NimBLEDevice::setPower(BLE_TX_POWER_DBM);
    NimBLEDevice::setMTU(517);  // Max MTU for BLE 5.x

    const bool requestedProxyEnabled = proxyEnabled_;
    proxyServerInitAttempted_ = true;
    proxyServerInitialized_ = initProxyServer(proxyName_.c_str());
    if (!proxyServerInitialized_) {
        Serial.println("[BLE] Proxy server unavailable during init");
        proxyEnabled_ = false;
    } else if (!requestedProxyEnabled) {
        releaseProxyQueues();
        proxyEnabled_ = false;
    }

    // Force 1M PHY only. NimBLE 2.x defaults to all-PHY (1M|2M|CODED) which
    // allows the controller to auto-negotiate 2M PHY after connection.  2M PHY
    // degrades RSSI readings significantly on the ESP32-S3.
    NimBLEDevice::setDefaultPhy(BLE_GAP_LE_PHY_1M_MASK, BLE_GAP_LE_PHY_1M_MASK);

    // OBDLink CX requires encrypted communication and benefits from bond
    // restore on reconnect. Keep pairing compatibility high by using
    // no-input/no-output legacy-capable bonding with ENC+ID key exchange.
    NimBLEDevice::setSecurityAuth(true, false, false);
    NimBLEDevice::setSecurityIOCap(BLE_SM_IO_CAP_NO_IO);
    NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
    NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);

    if (needsFreshFlashBondReset) {
        Preferences blePrefs;
        if (blePrefs.begin(BleFreshFlashPolicy::kNamespace, false)) {
            Serial.printf(" fresh-flash detected...");
            const BleFreshFlashPolicy::BondResetResult resetResult =
                BleFreshFlashPolicy::resetBondsForFirmwareVersion(
                    blePrefs,
                    FIRMWARE_VERSION,
                    backupCurrentBleBondsViaCore0AtBoot,
                    []() { NimBLEDevice::deleteAllBonds(); });
            if (resetResult.backedUpBondCount > 0) {
                Serial.printf(" backed up %d bond(s)...", resetResult.backedUpBondCount);
            }
            freshFlashBoot_ = true;
            blePrefs.end();
        }
    }

    // Restore bonds from SD backup if NVS was cleared (fresh-flash or NVS corruption)
    if (NimBLEDevice::getNumBonds() == 0) {
        const int restored = restoreBondsFromSD();
        if (restored > 0) {
            Serial.printf("[BLE] Restored %d bond(s) from SD\n", restored);
        }
    } else {
        // NVS has bonds — hand a snapshot to the Core-0 writer. Normal boot
        // must not perform SD I/O on the Arduino loop task.
        enqueueCurrentBleBondBackupSnapshot();
    }
    lastBondBackupCount_ = static_cast<uint8_t>(NimBLEDevice::getNumBonds());

    // Create client once during init - reuse for all connection attempts
    // Don't delete/recreate on failures - causes callback pointer corruption
    if (!pClient_) {
        pClient_ = NimBLEDevice::createClient();
        if (!pClient_) {
            Serial.println("[BLE] ERROR: Failed to create BLE client");
            return false;
        }

        // Create callbacks once and keep them for the lifetime of the client
        if (!pClientCallbacks_) {
            pClientCallbacks_.reset(new ClientCallbacks());
        }
        pClient_->setClientCallbacks(pClientCallbacks_.get());

        // Connection parameters: 12-24 (15-30ms interval), balanced for stability
        pClient_->setConnectionParams(NIMBLE_CONN_INTERVAL_MIN,
                                     NIMBLE_CONN_INTERVAL_MAX,
                                     NIMBLE_CONN_LATENCY,
                                     NIMBLE_CONN_SUPERVISION_TIMEOUT);
        pClient_->setConnectTimeout(NIMBLE_CONNECT_TIMEOUT_INIT_MS);
        pClient_->setConnectRetries(0);  // Project manages retries via MAX_CONNECT_ATTEMPTS
    }

    initialized = true;
    Serial.printf(" OK proxy=%s\n", proxyEnabled_ ? "on" : "off");
    return true;
}

bool V1BLEClient::begin(bool enableProxy, const char* proxyName) {
    // Initialize BLE stack first (idempotent)
    if (!initBLE(enableProxy, proxyName)) {
        return false;
    }

    // Start scanning for V1 - optimized for reliable discovery
    NimBLEScan* pScan = NimBLEDevice::getScan();

    // Replace scan callbacks atomically; previous handler is released automatically.
    pScanCallbacks_.reset(new ScanCallbacks(this));
    pScan->setScanCallbacks(pScanCallbacks_.get());
    pScan->setActiveScan(true);  // Request scan response to get device names
    // ESP32-S3 WiFi coexistence: use 75% duty cycle for reliable V1 discovery
    // Higher duty = more BLE radio time = faster discovery, but less WiFi throughput
    pScan->setInterval(160);  // 100ms interval
    pScan->setWindow(120);    // 75ms window - 75% duty cycle (was 50%)
    pScan->setMaxResults(0);  // Unlimited results
    // Reliability first: allow duplicate reports so we don't miss a late name/scan-response
    // update under WiFi coexistence stress.
    pScan->setDuplicateFilter(false);

    lastScanStart_ = static_cast<uint32_t>(millis());
    bool started = pScan->start(SCAN_DURATION, false, false);  // duration, isContinuous, restart

    if (started) {
        setBLEState(BLEState::SCANNING, "begin()");
    }

    return started;
}

bool V1BLEClient::isConnected() {
    // Acquire ordering ensures that when we observe connected_==false, we also
    // see the null characteristic pointers written before it (release) in cleanup.
    if (!connected_.load(std::memory_order_acquire) || !pClient_) {
        return false;
    }
    return pClient_->isConnected();
}

// RSSI caching - only query BLE stack every 2 seconds to reduce overhead.
// Atomic for portability: these are written/read from getConnectionRssi()
// which is a public method with no explicit threading constraint.
static std::atomic<int> s_cachedV1Rssi{0};
static uint32_t s_lastV1RssiQueryMs = 0;
static constexpr uint32_t RSSI_QUERY_INTERVAL_MS = 2000;

int V1BLEClient::getConnectionRssi() {
    // Return RSSI of connected_ V1 device, or 0 if not connected_
    if (!connected_.load(std::memory_order_relaxed) || !pClient_ || !pClient_->isConnected()) {
        s_cachedV1Rssi.store(0, std::memory_order_relaxed);
        return 0;
    }

    // Only query BLE stack every 2 seconds - return cached value otherwise
    const uint32_t now = static_cast<uint32_t>(millis());
    if (now - s_lastV1RssiQueryMs >= RSSI_QUERY_INTERVAL_MS) {
        s_cachedV1Rssi.store(pClient_->getRssi(), std::memory_order_relaxed);
        s_lastV1RssiQueryMs = now;
    }
    return s_cachedV1Rssi.load(std::memory_order_relaxed);
}

// Proxy client RSSI caching.
// Atomic for same portability reason as s_cachedV1Rssi above.
static std::atomic<int> s_cachedProxyRssi{0};
static uint32_t s_lastProxyRssiQueryMs = 0;

int V1BLEClient::getProxyClientRssi() {
    // Return RSSI of connected_ proxy client (app), or 0 if not connected_
    if (!proxyClientConnected_ || !pServer_ || pServer_->getConnectedCount() == 0) {
        s_cachedProxyRssi.store(0, std::memory_order_relaxed);
        return 0;
    }

    // Only query BLE stack every 2 seconds
    const uint32_t now = static_cast<uint32_t>(millis());
    if (now - s_lastProxyRssiQueryMs >= RSSI_QUERY_INTERVAL_MS) {
        // Use getPeerDevices() to get a valid handle safely.
        // getPeerInfo(0) can return conn_handle=0 (V1's handle) if the
        // phone disconnects between the count check and the lookup.
        std::vector<uint16_t> peers = pServer_->getPeerDevices();
        if (!peers.empty()) {
            int8_t rssi = 0;
            if (ble_gap_conn_rssi(peers[0], &rssi) == 0) {
                s_cachedProxyRssi.store(rssi, std::memory_order_relaxed);
            }
        }
        s_lastProxyRssiQueryMs = now;
    }
    return s_cachedProxyRssi.load(std::memory_order_relaxed);
}

bool V1BLEClient::isProxyClientConnected() {
    return proxyClientConnected_;
}

void V1BLEClient::setConnectionCycleProxyPolicy(const bool advertisingAllowed,
                                                const bool keepConnectionAllowed) {
    proxyAdvertisingAllowed_ = advertisingAllowed;
    proxyKeepConnectionAllowed_ = keepConnectionAllowed;
}

void V1BLEClient::setConnectionCycleState(const uint8_t stateCode,
                                          const uint32_t timeInStateMs) {
    connectionCycleStateCode_ = stateCode;
    connectionCycleTimeInStateMs_ = timeInStateMs;
}

void V1BLEClient::setObdBleArbitrationRequest(ObdBleArbitrationRequest request) {
    if (obdBleArbitrationRequest_ == request) {
        return;
    }

    const bool releasingAutoHold =
        obdBleArbitrationRequest_ == ObdBleArbitrationRequest::HOLD_PROXY_FOR_AUTO_OBD &&
        request != ObdBleArbitrationRequest::HOLD_PROXY_FOR_AUTO_OBD;
    const bool releasingManualPreempt =
        obdBleArbitrationRequest_ == ObdBleArbitrationRequest::PREEMPT_PROXY_FOR_MANUAL_SCAN &&
        request == ObdBleArbitrationRequest::NONE;

    if (releasingAutoHold || releasingManualPreempt) {
        proxySuppressedForObdHold_ = true;
        if (proxySuppressedResumeReasonCode_ ==
            static_cast<uint8_t>(PerfProxyAdvertisingTransitionReason::Unknown)) {
            proxySuppressedResumeReasonCode_ =
                static_cast<uint8_t>(PerfProxyAdvertisingTransitionReason::StartRetryWindow);
        }
    }

    obdBleArbitrationRequest_ = request;
}

void V1BLEClient::setProxyClientConnected(bool connected_) {
    proxyClientConnected_ = connected_;
    if (connected_) {
        proxyClientConnectedOnceThisBoot_ = true;
        proxyFastAdvertisingUntilMs_ = 0;
    }
}

void V1BLEClient::onDataReceived(DataCallback callback) {
    SemaphoreGuard lock(bleMutex_, pdMS_TO_TICKS(20));
    dataCallback_ = callback;
}

void V1BLEClient::onV1ConnectImmediate(ConnectionCallback callback) {
    SemaphoreGuard lock(bleMutex_, pdMS_TO_TICKS(20));
    connectImmediateCallback_ = callback;
}

void V1BLEClient::onV1Connected(ConnectionCallback callback) {
    SemaphoreGuard lock(bleMutex_, pdMS_TO_TICKS(20));
    connectStableCallback_ = callback;
}

void V1BLEClient::noteBleProcessDuration(uint32_t us) {
    lastBleProcessDurationUs_.store(us, std::memory_order_relaxed);
}

void V1BLEClient::noteDisplayPipelineDuration(uint32_t us) {
    lastDisplayPipelineDurationUs_.store(us, std::memory_order_relaxed);
}

bool V1BLEClient::isConnectBurstSettling() const {
    return connectedFollowupStep_ != ConnectedFollowupStep::NONE;
}

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

// Callback context for ble_store_iterate
struct BondCollector {
    struct ble_store_value_sec entries[kMaxBleBondEntries];
    size_t count;
};

static int bondCollectCallback(int obj_type, union ble_store_value* val, void* cookie) {
    (void)obj_type;
    auto* collector = static_cast<BondCollector*>(cookie);
    if (collector->count < kMaxBleBondEntries) {
        memcpy(&collector->entries[collector->count], &val->sec, sizeof(struct ble_store_value_sec));
        collector->count++;
    }
    return 0;  // 0 = continue iterating
}

static int collectBondEntries(BondCollector& ourSecs, BondCollector& peerSecs) {
    ble_store_iterate(BLE_STORE_OBJ_TYPE_OUR_SEC, bondCollectCallback, &ourSecs);
    ble_store_iterate(BLE_STORE_OBJ_TYPE_PEER_SEC, bondCollectCallback, &peerSecs);
    return static_cast<int>(ourSecs.count + peerSecs.count);
}

static int writeBondBackupSnapshot(fs::FS& sdFs,
                                   const BondCollector& ourSecs,
                                   const BondCollector& peerSecs) {
    const String tmpPath = String(BLE_BOND_BACKUP_PATH) + ".tmp";
    File f = sdFs.open(tmpPath.c_str(), "w");
    if (!f) {
        Serial.println("[BLE] WARN: Failed to open bond backup tmp file");
        return -1;
    }

    BondBackupHeader hdr = {};
    memcpy(hdr.magic, kBleBondMagic, 4);
    hdr.ourSecCount = ourSecs.count;
    hdr.peerSecCount = peerSecs.count;

    bool ok = true;
    ok = ok && (f.write((const uint8_t*)&hdr, sizeof(hdr)) == sizeof(hdr));
    if (ourSecs.count > 0) {
        const size_t sz = ourSecs.count * sizeof(struct ble_store_value_sec);
        ok = ok && (f.write((const uint8_t*)ourSecs.entries, sz) == sz);
    }
    if (peerSecs.count > 0) {
        const size_t sz = peerSecs.count * sizeof(struct ble_store_value_sec);
        ok = ok && (f.write((const uint8_t*)peerSecs.entries, sz) == sz);
    }
    f.flush();
    f.close();

    if (!ok) {
        sdFs.remove(tmpPath.c_str());
        Serial.println("[BLE] WARN: Bond backup write incomplete");
        return -1;
    }

    if (!StorageManager::promoteTempFileWithRollback(sdFs, tmpPath.c_str(), BLE_BOND_BACKUP_PATH)) {
        Serial.println("[BLE] WARN: Bond backup rename failed");
        return -1;
    }

    const int total = static_cast<int>(ourSecs.count + peerSecs.count);
    Serial.printf("[BLE] Backed up %d bond(s) to SD (%u our, %u peer)\n",
                  total, (unsigned)ourSecs.count, (unsigned)peerSecs.count);
    return total;
}

// Backup all bond keys to SD card. Safe to call anytime after NimBLEDevice::init().
// Returns number of bonds backed up, or -1 on error.
int backupBondsToSD() {
    if (!storageManager.isReady() || !storageManager.isSDCard()) {
        return -1;
    }

    BondCollector ourSecs = {};
    BondCollector peerSecs = {};
    if (collectBondEntries(ourSecs, peerSecs) == 0) {
        return 0;  // Nothing to backup
    }

    StorageManager::SDLockBlocking sdLock(storageManager.getSDMutex());
    if (!sdLock) {
        return -1;
    }

    fs::FS* sdFs = storageManager.getFilesystem();
    if (!sdFs) {
        return -1;
    }

    return writeBondBackupSnapshot(*sdFs, ourSecs, peerSecs);
}

int refreshBleBondBackup() {
    return backupBondsToSD();
}

int V1BLEClient::tryBackupBondsToSD() {
    if (!storageManager.isReady() || !storageManager.isSDCard()) {
        return -1;
    }

    BondCollector ourSecs = {};
    BondCollector peerSecs = {};
    if (collectBondEntries(ourSecs, peerSecs) == 0) {
        return 0;
    }

    StorageManager::SDTryLock sdLock(storageManager.getSDMutex());
    if (!sdLock) {
        return -1;
    }

    fs::FS* sdFs = storageManager.getFilesystem();
    if (!sdFs) {
        return -1;
    }

    return writeBondBackupSnapshot(*sdFs, ourSecs, peerSecs);
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
    const uint32_t now = static_cast<uint32_t>(millis());

    // 1. Unsubscribe from notifications if subscribed
    if (pDisplayDataChar_ && pDisplayDataChar_->canNotify()) {
        pDisplayDataChar_->unsubscribe();
    }

    // 2. Disconnect if connected_
    if (pClient_ && pClient_->isConnected()) {
        pClient_->disconnect();
    }

    // 3+4. Clear characteristic references and connection flags atomically under
    // bleMutex_ so that any in-flight notifyCallback cannot observe connected_==true
    // while the characteristic pointers are already null (use-after-free window).
    // Stores use release ordering so a reader that acquires connected_==false is
    // guaranteed to also see null characteristic pointers.
    pDisplayDataChar_ = nullptr;
    pCommandChar_ = nullptr;
    pCommandCharLong_ = nullptr;
    pRemoteService_ = nullptr;
    scanStopResultsCleared_ = false;
    // Atomic stores do not require the mutex — publish immediately so any
    // in-flight notifyCallback sees null pointers before we touch non-atomics.
    // Store IDs before pointers: a reader that sees null pointer is guaranteed
    // to also see the zeroed ID (release ordering on both).
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

// Hard reset of BLE client stack - use after repeated failures
// Reuses existing client to avoid NimBLE slot leak (max 3 slots)
void V1BLEClient::hardResetBLEClient() {
    Serial.println("[BLE] Hard reset...");
    const uint32_t now = static_cast<uint32_t>(millis());

    // Wait for any in-flight discovery task to finish before touching pClient_.
    // The task only calls pClient_->discoverAttributes() which completes quickly
    // once disconnect() fires the HCI terminate. Budget 500ms — if it hasn't
    // finished by then, proceed anyway (the task will see ENOTCONN and exit).
    if (discoveryTaskRunning_.load(std::memory_order_acquire)) {
        Serial.println("[BLE] Hard reset: waiting for discovery task...");
        const uint32_t waitStart = static_cast<uint32_t>(millis());
        while (discoveryTaskRunning_.load(std::memory_order_acquire) &&
               (static_cast<uint32_t>(millis()) - waitStart) < 500) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        if (discoveryTaskRunning_.load(std::memory_order_acquire)) {
            Serial.println("[BLE] Hard reset: discovery task still running after 500ms - proceeding");
        }
    }

    // Full cleanup first
    cleanupConnection();

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

    setBLEState(BLEState::DISCONNECTED, "hard reset complete");
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
                    backupBondsToSD,
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
        // NVS has bonds — keep SD backup fresh
        backupBondsToSD();
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

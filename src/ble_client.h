/**
 * BLE Client for Valentine1 Gen2
 * Handles connection and data reception from V1 over BLE
 * Also supports BLE Server mode for proxying to companion apps
 */

#pragma once
#ifndef BLE_CLIENT_H
#define BLE_CLIENT_H

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <array>
#include <vector>
#include <atomic>
#include <memory>

#include "ble_log_rate_limit.h"
#include "modules/obd/obd_ble_arbitration.h"

// Forward declarations
class V1BLEClient;

// Callback for receiving V1 display data packets
// data: pointer to packet data
// length: number of bytes
// charUUID: last 16-bit of source characteristic UUID (0xB2CE, 0xB4E0, etc)
// sessionGeneration: immutable V1 link generation captured by the notify callback
typedef void (*DataCallback)(const uint8_t* data, size_t length, uint16_t charUUID, uint32_t sessionGeneration);

// Callback for V1 connection events
typedef void (*ConnectionCallback)();
typedef void (*SessionBoundaryCallback)(uint32_t sessionGeneration);

// BLE Connection State Machine
// Centralized state to prevent overlapping operations and race conditions
// Connection phases are broken into discrete states for non-blocking operation
enum class BLEState {
    DISCONNECTED,    // Not connected_, not doing anything
    SCANNING,        // Actively scanning for V1
    SCAN_STOPPING,   // Scan stop requested, waiting for settle
    CONNECTING,      // Connection attempt initiated (async)
    CONNECTING_WAIT, // Waiting for async connect callback
    DISCOVERING,     // Service discovery in progress (uses cached handles if available)
    SUBSCRIBING,     // Subscribing to characteristics (multi-step, non-blocking)
    SUBSCRIBE_YIELD, // Yielding between subscribe steps to allow loop() to run
    CONNECTED,       // Successfully connected_ to V1
    BACKOFF,         // Failed connection, waiting before retry
    QUIESCING        // Disconnect/cancel in progress; client attributes remain owned
};

// Convert BLEState to string for logging
inline const char* bleStateToString(BLEState state) {
    switch (state) {
    case BLEState::DISCONNECTED:
        return "DISCONNECTED";
    case BLEState::SCANNING:
        return "SCANNING";
    case BLEState::SCAN_STOPPING:
        return "SCAN_STOPPING";
    case BLEState::CONNECTING:
        return "CONNECTING";
    case BLEState::CONNECTING_WAIT:
        return "CONNECTING_WAIT";
    case BLEState::DISCOVERING:
        return "DISCOVERING";
    case BLEState::SUBSCRIBING:
        return "SUBSCRIBING";
    case BLEState::SUBSCRIBE_YIELD:
        return "SUBSCRIBE_YIELD";
    case BLEState::CONNECTED:
        return "CONNECTED";
    case BLEState::BACKOFF:
        return "BACKOFF";
    case BLEState::QUIESCING:
        return "QUIESCING";
    default:
        return "UNKNOWN";
    }
}

inline uint8_t bleStateToCode(BLEState state) {
    return static_cast<uint8_t>(state);
}

// Atomic linearization gate shared by main-loop connection publication and
// NimBLE callback teardown. A successful claim is ordered before a later close;
// a close that wins first makes publication fail for that session generation.
class BleSessionPublicationGate {
  public:
    void open(uint32_t generation) { token_.store(generation, std::memory_order_release); }

    void close() { token_.exchange(0, std::memory_order_acq_rel); }

    bool accepts(uint32_t generation) const {
        return generation != 0 && token_.load(std::memory_order_acquire) == generation;
    }

    bool claim(uint32_t generation) {
        if (generation == 0) {
            return false;
        }
        uint32_t expected = generation;
        return token_.compare_exchange_strong(expected, generation, std::memory_order_acq_rel,
                                              std::memory_order_acquire);
    }

  private:
    std::atomic<uint32_t> token_{0};
};

inline bool bleQuiesceDeadlineExpired(uint32_t nowMs, uint32_t startedMs, uint32_t timeoutMs) {
    return static_cast<uint32_t>(nowMs - startedMs) >= timeoutMs;
}

// sendCommand result codes for proper retry semantics
enum class SendResult {
    SENT,    // Command successfully sent
    NOT_YET, // Pacing: too soon, caller should retry
    FAILED   // Hard failure: drop and count
};

// Proxy metrics for monitoring proxy health
struct ProxyMetrics {
    uint32_t sendCount = 0; // Successful notify sends
    std::atomic<uint32_t> dropCount{
        0};                      // Dropped due to queue full (atomic: incremented outside mutex on fast-fail path)
    uint32_t errorCount = 0;     // Notify failures
    uint32_t queueHighWater = 0; // Max queue depth seen
    uint32_t lastResetMs = 0;    // When metrics were last reset

    void reset() {
        sendCount = 0;
        dropCount.store(0, std::memory_order_relaxed);
        errorCount = 0;
        queueHighWater = 0;
        lastResetMs = millis();
    }
};

class V1BLEClient {
  public:
    V1BLEClient();
    ~V1BLEClient();

    // Initialize BLE stack only (no scanning)
    bool initBLE(bool enableProxy = false, const char* proxyName = "V1C-LE-S3");

    // Initialize BLE and start scanning
    // If enableProxy is true, also starts BLE server for app connections
    bool begin(bool enableProxy = false, const char* proxyName = "V1C-LE-S3");

    // Check connection status
    bool isConnected();

    // Get RSSI of connected_ V1 device (returns 0 if not connected_)
    int getConnectionRssi();

    // Get RSSI of connected_ proxy client (app) (returns 0 if not connected_)
    int getProxyClientRssi();

    // Check if proxy client (app) is connected_
    bool isProxyClientConnected();
    bool hasProxyClientConnectedThisBoot() const { return proxyClientConnectedOnceThisBoot_; }
    // Backward-compatible metrics surface; proxy no longer hard-latches off when
    // no phone connects, it downshifts to slow advertising instead.
    bool isProxyNoClientTimeoutLatched() const { return false; }

    // Check if BLE proxy is enabled
    bool isProxyEnabled() const { return proxyEnabled_; }
    bool setProxyRuntimeEnabled(bool enabled, const char* proxyName = nullptr);

    // Check if proxy is actively advertising (only true after V1 connects)
    bool isProxyAdvertising() const;
    void stopProxyAdvertising();
    void disconnectProxyPhones();
    bool isProxyFullyStopped() const;
    void setConnectionCycleProxyPolicy(bool advertisingAllowed, bool keepConnectionAllowed);
    void setConnectionCycleState(uint8_t stateCode, uint32_t timeInStateMs);

    void setObdBleArbitrationRequest(ObdBleArbitrationRequest request);

    // Debug/test control: force proxy advertising on/off at runtime.
    bool forceProxyAdvertising(bool enable, uint8_t reasonCode = 0);

    // Set proxy client connection status (for internal callback use)
    void setProxyClientConnected(bool connected_);

    // Register callback for received data
    void onDataReceived(DataCallback callback);

    // Register callback for immediate V1 connect work (critical path only).
    void onV1ConnectImmediate(ConnectionCallback callback);

    // Register authoritative main-loop session boundaries. Open fires after a
    // connect callback is accepted and before characteristic subscription;
    // close fires when quiescence invalidates the outgoing generation.
    void onV1SessionOpened(SessionBoundaryCallback callback);
    void onV1SessionClosed(SessionBoundaryCallback callback);

    // Register callback for stable V1 connection work after the connect burst settles.
    void onV1Connected(ConnectionCallback callback);

    // Record latest loop timings used by the connect-burst settle gate.
    void noteBleProcessDuration(uint32_t us);
    void noteDisplayPipelineDuration(uint32_t us);
    bool isConnectBurstSettling() const;
    uint32_t lastV1ConnectionEventMs() const { return lastV1ConnectionEventMs_.load(std::memory_order_relaxed); }
    uint32_t sessionGeneration() const { return sessionGeneration_.load(std::memory_order_acquire); }
    bool consumeVerifyPushMatchEdge() { return verifyPushMatchEdgePending_.exchange(false, std::memory_order_acq_rel); }

    // Send command to V1 (e.g., request alert data)
    bool sendCommand(const uint8_t* data, size_t length);

    // Send command with detailed result for retry logic
    SendResult sendCommandWithResult(const uint8_t* data, size_t length);

    // Request V1 to start sending alert data
    bool requestAlertData();

    // Request V1 version information (triggers data on B4E0)
    bool requestVersion();

    // Request the V1's authoritative volume settings
    // (RESPALLVOLUME 0x3D will be received with [main, muted, savedMain, savedMuted]).
    bool requestAllVolume();

    // Turn V1 display on/off (dark mode)
    bool setDisplayOn(bool on);

    // Send mute on/off command
    bool setMute(bool muted);

    // Change V1 operating mode (All Bogeys, Logic, Advanced Logic)
    bool setMode(uint8_t mode);

    // Set V1 volume settings (0-9 for each, 0xFF to keep current)
    bool setVolume(uint8_t mainVolume, uint8_t mutedVolume);

    // Request user settings bytes from V1 (6 bytes)
    bool requestUserBytes();

    // Write user settings bytes to V1 (6 bytes)
    bool writeUserBytes(const uint8_t* bytes);

    // Write user settings with optional verification (verification disabled - see implementation)
    enum WriteVerifyResult { VERIFY_OK = 0, VERIFY_WRITE_FAILED = 1, VERIFY_TIMEOUT = 2, VERIFY_MISMATCH = 3 };
    WriteVerifyResult writeUserBytesVerified(const uint8_t* bytes, int maxRetries = 2);

    // Prepare verification of user bytes on next read-back
    void startUserBytesVerification(const uint8_t* expected);

    // Called by main loop when RESP_USER_BYTES received to complete verification
    void onUserBytesReceived(const uint8_t* bytes);

    // Disconnect and cleanup
    void disconnect();

    // Full cleanup of BLE connection state (clears characteristic refs, unsubscribes)
    void cleanupConnection();

    // Hard reset of BLE client stack after repeated failures
    void hardResetBLEClient();

    // Process BLE events (call in loop)
    void process();

    // Retry deferred bond backup work outside the ingest phase.
    void serviceDeferredBondBackup(uint32_t nowMs);

    // Restart scanning for V1
    void startScanning();

    // Check if currently scanning
    bool isScanning();

    // Get current BLE state (for diagnostics)
    BLEState getBLEState() const { return bleState_; }
    uint8_t getBLEStateCode() const { return bleStateToCode(bleState_); }
    bool isConnectInProgress() const { return connectInProgress_; }
    bool isAsyncConnectPending() const { return asyncConnectPending_.load(std::memory_order_relaxed); }
    bool hasPendingDisconnectCleanup() const { return pendingDisconnectCleanup_.load(std::memory_order_relaxed); }
    uint32_t discoveryTaskStackMinFreeBytes() const {
        return discoveryTaskStackMinFreeBytes_.load(std::memory_order_relaxed);
    }
    uint32_t quiesceTimeoutRecoveryCount() const {
        return quiesceTimeoutRecoveryCount_.load(std::memory_order_relaxed);
    }
    uint8_t getSubscribeStepCode() const { return static_cast<uint8_t>(subscribeStep_); }
    const char* getSubscribeStepName() const;

    // Get the connected_ V1's BLE address
    NimBLEAddress getConnectedAddress() const;

    // Forward data to proxy clients (queues data for async send)
    // sourceCharUUID: last 16-bit of source characteristic UUID (0xB2CE, 0xB4E0, etc)
    void forwardToProxy(const uint8_t* data, size_t length, uint16_t sourceCharUUID);

    // Process pending proxy notifications (call from main loop after display update)
    // Returns number of packets sent
    int processProxyQueue();

    // Phone->V1 command drop counters (observability)
    uint32_t getPhoneCmdDropsOverflow() const;
    uint32_t getPhoneCmdDropsInvalid() const; // Malformed packets
    uint32_t getPhoneCmdDropsBleFail() const; // Hard BLE failures
    uint32_t getPhoneCmdDropsLockBusy() const;

    // Get proxy metrics (for instrumentation)
    const ProxyMetrics& getProxyMetrics() const { return proxyMetrics_; }

    // Reset proxy notify/send metrics only; phone command drop counters reset with perfMetricsReset().
    void resetProxyMetrics() { proxyMetrics_.reset(); }

    // WiFi priority mode - deprioritize BLE when web UI is active
    void setWifiPriority(bool enabled); // Enable = suppress BLE activity
    bool isWifiPriority() const { return wifiPriorityMode_; }

    // Boot readiness gate - blocks BLE scan/connect state machine until setup is ready
    void setBootReady(bool ready);
    bool isBootReady() const { return bootReadyFlag_; }

  private:
    // Nested callback classes - defined before member declarations that use them
    class ClientCallbacks : public NimBLEClientCallbacks {
      public:
        void onConnect(NimBLEClient* pClient_) override;
        void onConnectFail(NimBLEClient* pClient_, int reason) override;
        void onDisconnect(NimBLEClient* pClient_, int reason) override;
        void onPhyUpdate(NimBLEClient* pClient_, uint8_t txPhy, uint8_t rxPhy) override;
    };

    // NimBLE 2.x uses NimBLEScanCallbacks
    class ScanCallbacks : public NimBLEScanCallbacks {
      public:
        ScanCallbacks(V1BLEClient* client) : bleClient(client) {}
        void onResult(const NimBLEAdvertisedDevice* advertisedDevice) override;
        void onScanEnd(const NimBLEScanResults& scanResults, int reason) override;

      private:
        V1BLEClient* bleClient;
    };

    class ProxyServerCallbacks : public NimBLEServerCallbacks {
      public:
        ProxyServerCallbacks(V1BLEClient* client) : bleClient(client) {}
        void onConnect(NimBLEServer* pServer_, NimBLEConnInfo& connInfo) override;
        void onDisconnect(NimBLEServer* pServer_, NimBLEConnInfo& connInfo, int reason) override;

      private:
        V1BLEClient* bleClient;
    };

    class ProxyWriteCallbacks : public NimBLECharacteristicCallbacks {
      public:
        ProxyWriteCallbacks(V1BLEClient* client) : bleClient(client) {}
        void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) override;

      private:
        V1BLEClient* bleClient;
    };

    enum class ProxyCallbackEventType : uint8_t {
        APP_CONNECTED = 0,
        APP_DISCONNECTED = 1,
        V1_DISCONNECTED = 2,
    };

    struct ProxyCallbackEvent {
        ProxyCallbackEventType type = ProxyCallbackEventType::APP_CONNECTED;
        uint16_t connHandle = 0;
        int reason = 0;
    };

    // pClient_ and all remote attribute pointers are owned by the main loop.
    // BLE callbacks publish atomic events only; they never clear these handles.
    // The discovery task may mutate NimBLE's attribute cache only while the
    // state machine is DISCOVERING, and reconnect is forbidden until it exits.
    NimBLEClient* pClient_;
    NimBLERemoteService* pRemoteService_;
    NimBLERemoteCharacteristic* pDisplayDataChar_;
    NimBLERemoteCharacteristic* pCommandChar_;
    NimBLERemoteCharacteristic* pCommandCharLong_; // B8D2 - for long commands like voltage request
    // Cached notify characteristic -> short UUID mapping for callback hot path.
    // Written during subscribe steps; read from notify callback without locking.
    std::atomic<NimBLERemoteCharacteristic*> notifyShortChar_{nullptr};
    std::atomic<uint16_t> notifyShortCharId_{0};
    std::atomic<NimBLERemoteCharacteristic*> notifyLongChar_{nullptr};
    std::atomic<uint16_t> notifyLongCharId_{0};

    // BLE Server (proxy) objects
    NimBLEServer* pServer_;
    NimBLEService* pProxyService_;
    NimBLECharacteristic* pProxyNotifyChar_;     // B2CE proxy - short display data
    NimBLECharacteristic* pProxyNotifyLongChar_; // B4E0 proxy - long alert/response data
    NimBLECharacteristic* pProxyWriteChar_;
    bool proxyEnabled_;
    bool proxyServerInitialized_;
    bool proxyServerInitAttempted_;
    std::atomic<bool> proxyClientConnected_{false}; // Atomic for thread safety (set from BLE callbacks)
    String proxyName_;

    // Synchronization primitives (mirroring Kenny's approach)
    SemaphoreHandle_t bleMutex_ = nullptr;
    SemaphoreHandle_t bleNotifyMutex_ = nullptr;
    SemaphoreHandle_t phoneCmdMutex_ = nullptr;

    // Proxy queue for decoupling notify from hot path
    static constexpr size_t PROXY_QUEUE_SIZE = 8;   // Small queue, drop-oldest on overflow
    static constexpr size_t PROXY_PACKET_MAX = 512; // Max packet size for proxy (handles full V1 packets)
    struct ProxyPacket {
        uint8_t data[PROXY_PACKET_MAX];
        size_t length;
        uint16_t charUUID;
        uint32_t tsMs;
    };
    ProxyPacket* proxyQueue_ = nullptr;

    // Phone→V1 command queue for safe writes (decoupled from callback context)
    static constexpr size_t PHONE_CMD_QUEUE_SIZE = 16; // Small burst-tolerant queue for phone commands
    static constexpr size_t MAX_PHONE_CMDS_PER_LOOP = 4;
    ProxyPacket* phone2v1Queue_ = nullptr;
    bool proxyQueuesInPsram_ = false;
    // Closes queue admission before runtime teardown. The main loop retries
    // release until both callback-owned queue mutexes can be acquired.
    std::atomic<bool> proxyQueueReleasePending_{false};
    // Fences callbacks that began before a disable/re-enable allocation cycle.
    std::atomic<uint32_t> proxyQueueEpoch_{0};
    std::atomic<size_t> phone2v1QueueHead_{0};
    std::atomic<size_t> phone2v1QueueTail_{0};
    std::atomic<size_t> phone2v1QueueCount_{0};
    std::atomic<size_t> proxyQueueHead_{0};  // Next write position
    std::atomic<size_t> proxyQueueTail_{0};  // Next read position
    std::atomic<size_t> proxyQueueCount_{0}; // Current items in queue
    ProxyMetrics proxyMetrics_;

    DataCallback dataCallback_;
    ConnectionCallback connectImmediateCallback_;
    SessionBoundaryCallback sessionOpenedCallback_;
    SessionBoundaryCallback sessionClosedCallback_;
    ConnectionCallback connectStableCallback_;
    std::atomic<bool> connected_{false}; // Standalone connection flag; use atomic load/store for all direct accesses
    std::atomic<bool> shouldConnect_{false};             // Atomic for thread safety (set from BLE callbacks)
    std::atomic<bool> pendingConnectStateUpdate_{false}; // Deferred update from BLE callbacks
    std::atomic<uint32_t> pendingConnectStateGeneration_{0};
    std::atomic<bool> pendingDisconnectCleanup_{false};  // Deferred cleanup from BLE callbacks
    std::atomic<int> pendingDisconnectReason_{0};        // NimBLE reason, logged from the main loop
    std::atomic<bool> pendingDeleteBond_{false};         // Deferred bond deletion from BLE callback
    NimBLEAddress pendingDeleteBondAddr_;                // Address to delete bond for
    std::atomic<bool> pendingLastV1AddressValid_{false}; // Deferred settings save from BLE scan callback
    char pendingLastV1Address_[18] = {0};                // "AA:BB:CC:DD:EE:FF" + null
    std::atomic<bool> pendingLastV1NameValid_{false};    // Deferred proxy auto-name from BLE scan callback
    char pendingLastV1Name_[33] = {0};                   // BLE advertising name, 32 bytes + null
    std::atomic<bool> pendingScanEndUpdate_{false};      // Deferred scan-end state update from BLE callback
    std::atomic<bool> pendingScanTargetUpdate_{false};   // Deferred target update from BLE scan callback
    std::atomic<bool> phoneCmdPendingClear_{false};      // Clear stale phone cmd state on reconnect
    static constexpr size_t PROXY_CALLBACK_EVENT_QUEUE_DEPTH = 8;
    std::array<ProxyCallbackEvent, PROXY_CALLBACK_EVENT_QUEUE_DEPTH> proxyCallbackEventQueue_{};
    size_t proxyCallbackEventQueueHead_ = 0;
    size_t proxyCallbackEventQueueCount_ = 0;
    portMUX_TYPE proxyCallbackEventMux_ = portMUX_INITIALIZER_UNLOCKED;

    // Async discovery task (avoids ~2s block on main loop). Each task captures
    // an immutable session generation. Completion is accepted only for the
    // generation that launched it; timeout/disconnect invalidates that session.
    struct DiscoveryTaskContext {
        V1BLEClient* owner = nullptr;
        NimBLEClient* client = nullptr;
        uint32_t generation = 0;
    };
    std::atomic<bool> discoveryTaskRunning_{false};
    std::atomic<bool> discoveryTaskDone_{false};
    std::atomic<bool> discoveryTaskResult_{false};
    std::atomic<uint32_t> discoveryCompletedGeneration_{0};
    // UINT32_MAX means no discovery task has completed; zero is a valid and
    // critical measured minimum that must remain visible.
    std::atomic<uint32_t> discoveryTaskStackMinFreeBytes_{UINT32_MAX};
    DiscoveryTaskContext discoveryTaskContext_{};
    std::atomic<uint32_t> sessionGeneration_{0};
    uint32_t activeDiscoveryGeneration_ = 0;
    static void discoveryTaskFunc(void* param);

    // Client teardown is an explicit state. A new scan/connect cannot begin
    // until both async connect cancellation and discovery have quiesced, and
    // a requested link disconnect has delivered its callback.
    std::atomic<bool> acceptClientCallbacks_{false};
    BleSessionPublicationGate sessionPublicationGate_;
    std::atomic<bool> disconnectCallbackPending_{false};
    std::atomic<uint16_t> activeConnectionHandle_{0xFFFF};
    std::atomic<uint16_t> quiescingConnectionHandle_{0xFFFF};
    std::atomic<uint32_t> quiesceTimeoutRecoveryCount_{0};
    bool quiesceAwaitingConnectCancel_ = false;
    bool hardResetPending_ = false;
    uint32_t quiesceStartedMs_ = 0;
    uint32_t quiesceLastRetryMs_ = 0;
    static constexpr uint32_t QUIESCE_RETRY_MS = 250;
    static constexpr uint32_t QUIESCE_FATAL_TIMEOUT_MS = 15000;

    char pendingScanTargetAddress_[18] = {0}; // "AA:BB:CC:DD:EE:FF" + null
    uint8_t pendingScanTargetAddressType_ = BLE_ADDR_PUBLIC;
    bool hasTargetDevice_ = false;
    NimBLEAdvertisedDevice targetDevice_;
    NimBLEAddress targetAddress_;
    uint8_t targetAddressType_ = BLE_ADDR_PUBLIC; // Saved from advertisement
    uint32_t lastScanStart_;

    // BLE State Machine - centralized connection state
    BLEState bleState_ = BLEState::DISCONNECTED;
    uint32_t stateEnteredMs_ = 0;         // When current state was entered
    uint32_t scanStopRequestedMs_ = 0;    // When scan stop was requested
    bool scanStopResultsCleared_ = false; // One-shot clearResults gate for SCAN_STOPPING
    // ESP32-S3 BLE: radio needs time after scan to be ready for connect
    // Cold boot needs significantly more time for NimBLE stack to stabilize
    static constexpr uint32_t SCAN_STOP_SETTLE_MS = 100; // 100ms settle for reconnects
    static constexpr uint32_t SCAN_STOP_SETTLE_FRESH_MS =
        200;                         // 200ms on cold boot - tuned lower for faster first connect
    bool firstScanAfterBoot_ = true; // Use longer settle on first scan

    // Connection attempt guard - prevents overlapping attempts
    bool connectInProgress_ = false;
    uint32_t connectStartMs_ = 0; // When connect started (for stuck detection)

    // Async connection tracking
    std::atomic<bool> asyncConnectPending_{false};     // Async connect in progress
    std::atomic<bool> asyncConnectSuccess_{false};     // Result from onConnect callback
    uint8_t connectAttemptNumber_ = 0;                 // Current attempt (1-based)
    static constexpr uint8_t MAX_CONNECT_ATTEMPTS = 5; // 5 attempts - more retries
    static constexpr uint16_t NIMBLE_CONN_INTERVAL_MIN = 12;
    static constexpr uint16_t NIMBLE_CONN_INTERVAL_MAX = 24;
    static constexpr uint16_t NIMBLE_CONN_LATENCY = 0;
    static constexpr uint16_t NIMBLE_CONN_SUPERVISION_TIMEOUT = 400;
    static constexpr uint32_t CONNECT_TIMEOUT_MS = 3000; // 3s timeout - if it works, it's fast
    // NimBLE timeout API is milliseconds. Align client-level timeout with the state-machine budget.
    static constexpr uint32_t NIMBLE_CONNECT_TIMEOUT_INIT_MS = CONNECT_TIMEOUT_MS;
    static constexpr uint32_t NIMBLE_CONNECT_TIMEOUT_ACTIVE_MS = CONNECT_TIMEOUT_MS;
    static constexpr uint32_t DISCOVERY_TIMEOUT_MS = 5000; // 5s for discovery
    static constexpr uint32_t SUBSCRIBE_TIMEOUT_MS = 3000; // 3s for subscriptions
    uint32_t connectPhaseStartUs_ = 0;                     // For timing individual phases

    // Fresh flash detection - set when firmware version changed
    bool freshFlashBoot_ = false;
    // Tracks the bond-count snapshot that has already been persisted to SD.
    // 0xFF means unknown/uninitialized.
    uint8_t lastBondBackupCount_ = 0xFF;
    bool pendingBondBackup_ = false;
    uint8_t pendingBondBackupCount_ = 0xFF;
    uint32_t pendingBondBackupRetryAtMs_ = 0;
    static constexpr uint32_t DEFERRED_BOND_BACKUP_RETRY_MS = 1000;

    // Non-blocking subscribe step machine
    // Each step does one BLE operation then yields to loop()
    enum class SubscribeStep {
        GET_SERVICE = 0,       // Get V1 service reference
        GET_DISPLAY_CHAR = 1,  // Get B2CE display data characteristic
        GET_COMMAND_CHAR = 2,  // Get command write characteristic
        GET_COMMAND_LONG = 3,  // Get B8D2 long command characteristic
        SUBSCRIBE_DISPLAY = 4, // Subscribe to B2CE notifications
        // 5 reserved for the retired manual B2CE CCCD-write step.
        GET_DISPLAY_LONG = 6, // Get B4E0 characteristic
        SUBSCRIBE_LONG = 7,   // Subscribe to B4E0 notifications
        // 8 reserved for the retired manual B4E0 CCCD-write step.
        REQUEST_ALERT_DATA = 9, // Send alert data request
        REQUEST_VERSION = 10,   // Send version request
        COMPLETE = 11           // All steps done
    };
    enum class ConnectedFollowupStep {
        NONE,
        REQUEST_ALERT_DATA,
        WAIT_CONNECT_BURST_SETTLE,
        REQUEST_VERSION,
        NOTIFY_STABLE_CALLBACK,
        BACKUP_BONDS,
    };
    SubscribeStep subscribeStep_ = SubscribeStep::GET_SERVICE;
    ConnectedFollowupStep connectedFollowupStep_ = ConnectedFollowupStep::NONE;
    uint32_t subscribeStepStartUs_ = 0;                         // When current step started
    uint32_t subscribeYieldUntilMs_ = 0;                        // When to resume from SUBSCRIBE_YIELD
    static constexpr uint32_t SUBSCRIBE_STEP_BUDGET_US = 50000; // 50ms per step max
    static constexpr uint32_t SUBSCRIBE_YIELD_MS = 5;           // 5ms yield between steps
    static constexpr uint32_t CONNECT_BURST_STABLE_BLE_MAX_US = 25000;
    static constexpr uint32_t CONNECT_BURST_STABLE_DISP_MAX_US = 50000;
    static constexpr uint8_t CONNECT_BURST_STABLE_CONSECUTIVE_LOOPS = 3;
    static constexpr uint32_t CONNECT_BURST_SETTLE_AFTER_FIRST_RX_MS = 1500;
    static constexpr uint32_t CONNECT_BURST_SETTLE_AFTER_CONNECTED_MS = 2500;
    std::atomic<uint32_t> lastV1ConnectionEventMs_{0};
    std::atomic<uint32_t> connectCompletedAtMs_{0};
    std::atomic<uint32_t> firstRxAfterConnectMs_{0};
    std::atomic<uint32_t> lastBleProcessDurationUs_{0};
    std::atomic<uint32_t> lastDisplayPipelineDurationUs_{0};
    uint8_t connectBurstStableLoopCount_ = 0;
    BleLogRateLimitState followupRequestAlertFailLog_;
    BleLogRateLimitState followupRequestVersionFailLog_;
    BleLogRateLimitState followupRequestAllVolumeFailLog_;

    // Async connect step functions
    bool startAsyncConnect();     // Initiate async connect
    void processConnectingWait(); // Handle CONNECTING_WAIT state
    void processDiscovering();    // Handle DISCOVERING state
    void beginClientQuiesce(const char* reason, bool requestHardReset = false);
    void processClientQuiesce();
    void processSubscribing();              // Handle SUBSCRIBING state (step machine)
    void processSubscribeYield();           // Handle SUBSCRIBE_YIELD state
    void processConnectedFollowup();        // Spread post-connect work across loop turns
    int enqueueCurrentBondBackupSnapshot(); // Non-blocking handoff to Core-0 writer
    bool executeSubscribeStep();            // Execute one subscribe step, return true if done

    // Called from connectToServer() after successful sync connect
    bool finishConnection();
    void completeHardResetBLEClient();

    // Queue phone->V1 commands from BLE callback context
    bool enqueuePhoneCommand(const uint8_t* data, size_t length, uint16_t sourceCharUUID);
    bool enqueuePhoneCommandForEpoch(const uint8_t* data, size_t length, uint16_t sourceCharUUID, uint32_t queueEpoch);
    int processPhoneCommandQueue();
    // Diagnostic helper to log negotiated connection parameters
    void logConnParams(const char* tag);

    // State transition helper
    void setBLEState(BLEState newState, const char* reason);

    // Defer settings writes from BLE scan callback
    void deferLastV1Address(const char* addr, const char* advertisedName = nullptr);

    // Consecutive V1 connection failure tracking.
    // nextConnectAllowedMs_ is only used for short intra-sequence retry pacing.
    uint8_t consecutiveConnectFailures_ = 0;
    uint32_t nextConnectAllowedMs_ = 0;

    // Deferred proxy advertising start (non-blocking - avoids stall)
    // Tuned lower to reduce post-connect latency while preserving radio settle margin
    uint32_t proxyAdvertisingStartMs_ = 0;        // When to start advertising (0 = not pending)
    uint8_t proxyAdvertisingStartReasonCode_ = 0; // PerfProxyAdvertisingTransitionReason
    static constexpr uint32_t PROXY_STABILIZE_MS = 100;
    // Fast advertising is only for app discovery. If no phone attaches, keep
    // proxy available but downshift to low-duty advertising for the rest of the
    // drive. A dropped app connection gets a shorter fast retry window.
    static constexpr uint32_t PROXY_FAST_START_WINDOW_MS = 120000;    // 2 minutes
    static constexpr uint32_t PROXY_FAST_RECONNECT_WINDOW_MS = 30000; // 30 seconds
    static constexpr uint16_t PROXY_ADV_FAST_MIN_INTERVAL = 0x50;     // ~50 ms
    static constexpr uint16_t PROXY_ADV_FAST_MAX_INTERVAL = 0xA0;     // ~100 ms
    static constexpr uint16_t PROXY_ADV_SLOW_MIN_INTERVAL = 0x0640;   // ~1 second
    static constexpr uint16_t PROXY_ADV_SLOW_MAX_INTERVAL = 0x0C80;   // ~2 seconds
    uint32_t proxyFastAdvertisingUntilMs_ = 0;
    bool proxyFastStartWindowArmed_ = false;
    bool proxyAdvertisingFastCadence_ = true;
    bool proxyClientConnectedOnceThisBoot_ = false;
    bool proxySuppressedForObdHold_ = false;
    bool proxyDisconnectRequestedByCoordinator_ = false;
    uint8_t proxySuppressedResumeReasonCode_ = 0;
    bool proxyAdvertisingAllowed_ = false;
    bool proxyKeepConnectionAllowed_ = false;
    uint8_t connectionCycleStateCode_ = 0;
    uint32_t connectionCycleTimeInStateMs_ = 0;
    ObdBleArbitrationRequest obdBleArbitrationRequest_ = ObdBleArbitrationRequest::NONE;

    // Write verification state
    bool verifyPending_ = false;
    uint8_t verifyExpected_[6] = {0};
    uint8_t verifyReceived_[6] = {0};
    bool verifyComplete_ = false;
    bool verifyMatch_ = false;
    std::atomic<bool> verifyPushMatchEdgePending_{false};

    // Callback handlers are RAII-owned to prevent manual delete mistakes.
    std::unique_ptr<ScanCallbacks> pScanCallbacks_;
    std::unique_ptr<ClientCallbacks> pClientCallbacks_;
    std::unique_ptr<ProxyServerCallbacks> pProxyServerCallbacks_;
    std::unique_ptr<ProxyWriteCallbacks> pProxyWriteCallbacks_;

    // WiFi priority mode flag
    bool wifiPriorityMode_ = false;
    bool bootReadyFlag_ = false;

    // Initialize BLE server for proxy mode
    bool initProxyServer(const char* deviceName);
    void configureProxyAdvertisingPayload(const char* deviceName);
    void adoptV1AdvertisedNameForProxy(const char* advertisedName);
    bool allocateProxyQueues();
    void releaseProxyQueues();
    bool tryFinalizeProxyQueueRelease();
    void forwardToProxyForEpoch(const uint8_t* data, size_t length, uint16_t sourceCharUUID, uint32_t queueEpoch);
    bool enqueueProxyCallbackEvent(const ProxyCallbackEvent& event);
    bool popProxyCallbackEvent(ProxyCallbackEvent& event);
    void drainProxyCallbackEvents();
    void handleProxyCallbackEvent(const ProxyCallbackEvent& event);
    bool localV1WriteSuppressedByProxy(const char* operation) const;
    void clearProxyAdvertisingSchedule();
    void stopProxyAdvertisingFromMainLoop(uint8_t reasonCode);
    void armProxyFastAdvertisingWindow(uint32_t nowMs, uint32_t durationMs);
    bool proxyFastAdvertisingActive(uint32_t nowMs) const;
    void applyProxyAdvertisingCadence(bool fastCadence);
    void refreshProxyAdvertisingCadence(uint32_t nowMs, uint8_t reasonCode);

    // Start advertising proxy service
    void startProxyAdvertising(uint8_t reasonCode = 0, bool ignoreWifiPriority = false);

    // Internal callbacks
    static void notifyCallback(NimBLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify);

    bool connectToServer();
};

// Refresh the shared NimBLE bond backup snapshot after an external module
// adds or deletes bond records. This only snapshots/enqueues; SD I/O runs on
// the dedicated Core-0 writer.
int refreshBleBondBackup();
#endif // BLE_CLIENT_H

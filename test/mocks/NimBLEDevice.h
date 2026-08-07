#pragma once

#include <cstdint>
#include <cstring>
#include <functional>
#include <cctype>
#include <memory>
#include <string>
#include <vector>

// Pull in FreeRTOS mock so headers that include <NimBLEDevice.h> get
// TickType_t, portMUX_TYPE, pdTRUE, xSemaphoreTake etc. automatically
#include "freertos/FreeRTOS.h"

// BLE security / pairing constants
static constexpr int BLE_HS_ETIMEOUT            = 5;
static constexpr int BLE_HS_ENOTCONN             = 7;
static constexpr uint16_t BLE_HS_CONN_HANDLE_NONE = 0xFFFF;
static constexpr int BLE_SM_IO_CAP_NO_IO        = 3;
static constexpr int BLE_SM_PAIR_KEY_DIST_ENC   = 0x01;
static constexpr int BLE_SM_PAIR_KEY_DIST_ID    = 0x02;

static constexpr uint8_t BLE_ADDR_PUBLIC = 0;
static constexpr int BLE_UUID_TYPE_16 = 16;
static constexpr int BLE_UUID_TYPE_128 = 128;
static constexpr int ESP_PWR_LVL_P9 = 11;

// BLE PHY constants
static constexpr uint8_t BLE_GAP_LE_PHY_1M       = 1;
static constexpr uint8_t BLE_GAP_LE_PHY_2M       = 2;
static constexpr uint8_t BLE_GAP_LE_PHY_CODED    = 3;
static constexpr uint8_t BLE_GAP_LE_PHY_1M_MASK  = 0x01;
static constexpr uint8_t BLE_GAP_LE_PHY_2M_MASK  = 0x02;

enum class NimBLETxPowerType { All = 0, Advertise = 1, Scan = 2, Connection = 3 };

enum NIMBLE_PROPERTY : uint32_t {
    READ = 1 << 0,
    NOTIFY = 1 << 1,
    WRITE_NR = 1 << 2,
    WRITE = 1 << 3,
};

struct MockNimBLEState {
    uint32_t createServerCalls = 0;
    uint32_t createServiceCalls = 0;
    uint32_t createCharacteristicCalls = 0;
    uint32_t updateConnParamsCalls = 0;
    uint32_t serverDisconnectCalls = 0;
    uint32_t characteristicNotifyCalls = 0;
    uint32_t startAdvertisingCalls = 0;
    uint32_t stopAdvertisingCalls = 0;
    uint32_t setMinIntervalCalls = 0;
    uint32_t setMaxIntervalCalls = 0;
    uint16_t minInterval = 0;
    uint16_t maxInterval = 0;
    uint32_t scanStartCalls = 0;
    uint32_t setScanCallbacksCalls = 0;
    bool scanStartResult = true;
    bool scanning = false;
    const void* scanCallbacks = nullptr;
    std::string lastNotifyUuid;
    std::vector<uint8_t> lastNotifyData;
    std::string deviceName;
    std::string advertisementName;
    std::string scanResponseName;
    bool advertising = false;
    uint8_t bondCount = 0;
};

inline MockNimBLEState g_mock_nimble_state{};

inline void mock_reset_nimble_state() {
    g_mock_nimble_state = MockNimBLEState{};
}

class NimBLEAddress {
public:
    NimBLEAddress() = default;
    explicit NimBLEAddress(const std::string& value, uint8_t type = BLE_ADDR_PUBLIC)
        : value_(value), type_(type) {}
    bool isNull() const { return value_.empty(); }
    std::string toString() const { return value_; }
    uint8_t getType() const { return type_; }
    bool operator==(const NimBLEAddress& other) const {
        return value_ == other.value_ && type_ == other.type_;
    }
    bool operator!=(const NimBLEAddress& other) const { return !(*this == other); }

private:
    std::string value_;
    uint8_t type_ = BLE_ADDR_PUBLIC;
};

class NimBLEUUID {
public:
    NimBLEUUID() = default;
    explicit NimBLEUUID(const char* uuid)
        : value_(uuid ? uuid : "") {
        parseValue();
    }
    explicit NimBLEUUID(uint16_t uuid)
        : value_(std::to_string(uuid)), bitSize_(BLE_UUID_TYPE_16) {
        raw_[0] = static_cast<uint8_t>(uuid & 0xFFu);
        raw_[1] = static_cast<uint8_t>((uuid >> 8) & 0xFFu);
    }

    int bitSize() const { return bitSize_; }
    const uint8_t* getValue() const { return raw_; }
    void to16() {
        if (bitSize_ != BLE_UUID_TYPE_128 || !isV1Base()) {
            return;
        }
        const uint8_t lo = raw_[12];
        const uint8_t hi = raw_[13];
        memset(raw_, 0, sizeof(raw_));
        raw_[0] = lo;
        raw_[1] = hi;
        bitSize_ = BLE_UUID_TYPE_16;
    }
    const std::string& toString() const { return value_; }

private:
    static int hexNibble(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        return -1;
    }

    static bool parseHexByte(const std::string& text, size_t pos, uint8_t& out) {
        if (pos + 1 >= text.size()) return false;
        const int hi = hexNibble(text[pos]);
        const int lo = hexNibble(text[pos + 1]);
        if (hi < 0 || lo < 0) return false;
        out = static_cast<uint8_t>((hi << 4) | lo);
        return true;
    }

    static bool parseHex16At(const std::string& text, size_t pos, uint16_t& out) {
        uint8_t hi = 0;
        uint8_t lo = 0;
        if (!parseHexByte(text, pos, hi) || !parseHexByte(text, pos + 2, lo)) {
            return false;
        }
        out = static_cast<uint16_t>((static_cast<uint16_t>(hi) << 8) | lo);
        return true;
    }

    bool isV1Base() const {
        static constexpr uint8_t kV1UuidBaseLePrefix[12] = {
            0x5E, 0xC0, 0xAE, 0x91, 0x3C, 0xF2, 0x59, 0xAA, 0xE2, 0x11, 0x05, 0x9E
        };
        return memcmp(raw_, kV1UuidBaseLePrefix, sizeof(kV1UuidBaseLePrefix)) == 0;
    }

    void parseValue() {
        memset(raw_, 0, sizeof(raw_));
        bitSize_ = BLE_UUID_TYPE_128;

        if (value_.size() == 4) {
            uint16_t shortUuid = 0;
            if (parseHex16At(value_, 0, shortUuid)) {
                bitSize_ = BLE_UUID_TYPE_16;
                raw_[0] = static_cast<uint8_t>(shortUuid & 0xFFu);
                raw_[1] = static_cast<uint8_t>((shortUuid >> 8) & 0xFFu);
            }
            return;
        }

        static constexpr const char* kV1Suffix = "-9E05-11E2-AA59-F23C91AEC05E";
        if (value_.size() == 36 &&
            value_.compare(8, strlen(kV1Suffix), kV1Suffix) == 0) {
            uint16_t shortUuid = 0;
            if (parseHex16At(value_, 4, shortUuid)) {
                static constexpr uint8_t kV1UuidBaseLePrefix[12] = {
                    0x5E, 0xC0, 0xAE, 0x91, 0x3C, 0xF2, 0x59, 0xAA,
                    0xE2, 0x11, 0x05, 0x9E
                };
                memcpy(raw_, kV1UuidBaseLePrefix, sizeof(kV1UuidBaseLePrefix));
                raw_[12] = static_cast<uint8_t>(shortUuid & 0xFFu);
                raw_[13] = static_cast<uint8_t>((shortUuid >> 8) & 0xFFu);
                raw_[14] = 0xA0;
                raw_[15] = 0x92;
            }
        }
    }

    std::string value_;
    int bitSize_ = BLE_UUID_TYPE_128;
    uint8_t raw_[16] = {0};
};

class NimBLEConnInfo {
public:
    uint16_t getConnHandle() const { return 1; }
    uint16_t getConnInterval() const { return 12; }
    uint16_t getConnLatency() const { return 0; }
    bool isEncrypted() const { return encrypted_; }
    bool isBonded() const { return bonded_; }
    bool isAuthenticated() const { return authenticated_; }
    NimBLEAddress getIdAddress() const { return idAddress_; }

    void setSecurityState(bool encrypted, bool bonded, bool authenticated) {
        encrypted_ = encrypted;
        bonded_ = bonded;
        authenticated_ = authenticated;
    }
    void setIdAddress(const NimBLEAddress& address) { idAddress_ = address; }

private:
    bool encrypted_ = false;
    bool bonded_ = false;
    bool authenticated_ = false;
    NimBLEAddress idAddress_;
};

class NimBLEAdvertisedDevice {
public:
    bool haveName() const { return !getName().empty(); }
    std::string getName() const { return ""; }
    NimBLEAddress getAddress() const { return NimBLEAddress(); }
    int getRSSI() const { return -70; }
    bool isAdvertisingService(const NimBLEUUID&) const { return false; }
    int getAddressType() const { return BLE_ADDR_PUBLIC; }
};
class NimBLEScanResults {};

// Forward declaration needed by NimBLERemoteService
class NimBLERemoteCharacteristic;

class NimBLERemoteService {
public:
    NimBLERemoteCharacteristic* getCharacteristic(const NimBLEUUID& uuid);
    NimBLERemoteCharacteristic* getCharacteristic(const char* uuid);
    void addCharacteristic(NimBLERemoteCharacteristic* characteristic) {
        if (characteristic) {
            characteristics_.push_back(characteristic);
        }
    }

private:
    std::vector<NimBLERemoteCharacteristic*> characteristics_;
};

class NimBLERemoteDescriptor {
public:
    bool writeValue(const uint8_t*, size_t, bool = false) { return true; }
};

class NimBLERemoteCharacteristic {
public:
    explicit NimBLERemoteCharacteristic(const char* uuid = "")
        : uuid_(uuid ? uuid : "") {}

    bool writeValue(const uint8_t*, size_t, bool) {
        writeValueCalls_++;
        if (writeEntryHook_) {
            writeEntryHook_();
        }
        return writeValueResult_;
    }
    bool canWrite() const { return true; }
    bool canWriteNoResponse() const { return false; }
    bool canNotify() const { return true; }
    bool canIndicate() const { return false; }
    bool unsubscribe() { return true; }
    bool subscribe(bool notify,
                   std::function<void(NimBLERemoteCharacteristic*, uint8_t*, size_t, bool)> callback,
                   bool = false) {
        subscribeCalls_++;
        if (subscribeEntryHook_) {
            subscribeEntryHook_();
        }
        notify_ = notify;
        callback_ = callback;
        return subscribeResult_;
    }
    NimBLEUUID getUUID() const { return NimBLEUUID(uuid_.c_str()); }
    NimBLERemoteDescriptor* getDescriptor(const NimBLEUUID&) { return nullptr; }
    void setSubscribeResult(bool ok) { subscribeResult_ = ok; }
    void setWriteValueResult(bool ok) { writeValueResult_ = ok; }
    void setWriteEntryHook(std::function<void()> hook) { writeEntryHook_ = std::move(hook); }
    void setSubscribeEntryHook(std::function<void()> hook) { subscribeEntryHook_ = std::move(hook); }
    uint32_t writeValueCalls() const { return writeValueCalls_; }
    uint32_t subscribeCalls() const { return subscribeCalls_; }
    void emit(uint8_t* data, size_t length) {
        if (callback_) {
            callback_(this, data, length, notify_);
        }
    }

private:
    std::string uuid_;
    std::function<void(NimBLERemoteCharacteristic*, uint8_t*, size_t, bool)> callback_;
    std::function<void()> writeEntryHook_;
    std::function<void()> subscribeEntryHook_;
    bool notify_ = true;
    bool subscribeResult_ = true;
    bool writeValueResult_ = true;
    uint32_t writeValueCalls_ = 0;
    uint32_t subscribeCalls_ = 0;
};

inline NimBLERemoteCharacteristic* NimBLERemoteService::getCharacteristic(const NimBLEUUID& uuid) {
    for (NimBLERemoteCharacteristic* characteristic : characteristics_) {
        if (characteristic && characteristic->getUUID().toString() == uuid.toString()) {
            return characteristic;
        }
    }
    return nullptr;
}

inline NimBLERemoteCharacteristic* NimBLERemoteService::getCharacteristic(const char* uuid) {
    return getCharacteristic(NimBLEUUID(uuid));
}

class NimBLEAttValue {
public:
    NimBLEAttValue() = default;

    const uint8_t* data() const { return data_.empty() ? nullptr : data_.data(); }
    size_t size() const { return data_.size(); }
    void assign(const uint8_t* data, size_t size) { data_.assign(data, data + size); }

private:
    std::vector<uint8_t> data_;
};

class NimBLECharacteristicCallbacks;

class NimBLECharacteristic {
public:
    explicit NimBLECharacteristic(const char* uuid = "")
        : uuid_(uuid ? uuid : "") {}

    void setCallbacks(NimBLECharacteristicCallbacks* callbacks) { callbacks_ = callbacks; }
    bool notify(const uint8_t* data, size_t size) {
        g_mock_nimble_state.characteristicNotifyCalls++;
        g_mock_nimble_state.lastNotifyUuid = uuid_;
        if (data && size > 0) {
            g_mock_nimble_state.lastNotifyData.assign(data, data + size);
        } else {
            g_mock_nimble_state.lastNotifyData.clear();
        }
        return notifyResult_;
    }
    bool writeValue(const uint8_t*, size_t, bool) { return writeValueResult_; }
    NimBLEAttValue getValue() const { return value_; }
    NimBLEUUID getUUID() const { return NimBLEUUID(uuid_.c_str()); }
    void setValue(const uint8_t* data, size_t size) { value_.assign(data, size); }
    void setNotifyResult(bool ok) { notifyResult_ = ok; }
    void setWriteValueResult(bool ok) { writeValueResult_ = ok; }

private:
    std::string uuid_;
    NimBLECharacteristicCallbacks* callbacks_ = nullptr;
    NimBLEAttValue value_;
    bool notifyResult_ = true;
    bool writeValueResult_ = true;
};

class NimBLEService {
public:
    explicit NimBLEService(const char* uuid = "")
        : uuid_(uuid ? uuid : "") {}

    NimBLECharacteristic* createCharacteristic(const char* uuid, uint32_t) {
        g_mock_nimble_state.createCharacteristicCalls++;
        characteristics_.push_back(std::make_unique<NimBLECharacteristic>(uuid));
        return characteristics_.back().get();
    }

    void start() {}
    NimBLEUUID getUUID() const { return NimBLEUUID(uuid_.c_str()); }

private:
    std::string uuid_;
    std::vector<std::unique_ptr<NimBLECharacteristic>> characteristics_;
};

class NimBLEServerCallbacks;

class NimBLEServer {
public:
    NimBLEService* createService(const char* uuid) {
        g_mock_nimble_state.createServiceCalls++;
        services_.push_back(std::make_unique<NimBLEService>(uuid));
        return services_.back().get();
    }

    void setCallbacks(NimBLEServerCallbacks* callbacks) { callbacks_ = callbacks; }
    void updateConnParams(uint16_t, uint16_t, uint16_t, uint16_t, uint16_t) {
        g_mock_nimble_state.updateConnParamsCalls++;
    }
    int getConnectedCount() const { return connectedCount_; }
    NimBLEConnInfo getPeerInfo(int) const { return NimBLEConnInfo(); }
    std::vector<uint16_t> getPeerDevices() const { return {}; }
    void setConnectedCount(int count) { connectedCount_ = count; }
    bool disconnect(uint16_t, uint8_t = 0) {
        g_mock_nimble_state.serverDisconnectCalls++;
        connectedCount_ = 0;
        return true;
    }
    bool disconnect(const NimBLEConnInfo&, uint8_t = 0) {
        g_mock_nimble_state.serverDisconnectCalls++;
        connectedCount_ = 0;
        return true;
    }

private:
    NimBLEServerCallbacks* callbacks_ = nullptr;
    std::vector<std::unique_ptr<NimBLEService>> services_;
    int connectedCount_ = 0;
};

class NimBLEClientCallbacks;

class NimBLEClient {
public:
    void setClientCallbacks(NimBLEClientCallbacks* callbacks) { callbacks_ = callbacks; }
    void setConnectionParams(uint16_t, uint16_t, uint16_t, uint16_t) {}
    void setConnectTimeout(unsigned long) {}
    void setConnectRetries(uint8_t) {}
    bool isConnected() const { return connected_; }
    bool disconnect() {
        disconnectCalls_++;
        if (disconnectEntryHook_) {
            disconnectEntryHook_();
        }
        if (!disconnectResult_) {
            return false;
        }
        connected_ = false;
        return true;
    }
    bool cancelConnect() const { return cancelConnectResult_; }
    int getRssi() const { return -60; }
    uint16_t getConnHandle() const { return connHandle_; }
    NimBLEConnInfo getConnInfo() const { return NimBLEConnInfo(); }
    NimBLEAddress getPeerAddress() const { return NimBLEAddress(); }
    bool connect(const NimBLEAddress&, bool = false, bool = false, bool = true) {
        connected_ = true;
        return true;
    }
    bool secureConnection(bool = true) { return secureConnectionResult_; }
    int getLastError() const { return lastError_; }
    bool discoverAttributes() { return true; }
    NimBLERemoteService* getService(const NimBLEUUID&) { return service_; }
    NimBLERemoteService* getService(const char*) { return service_; }
    void setService(NimBLERemoteService* service) { service_ = service; }
    void setConnected(bool connected) {
        connected_ = connected;
        connHandle_ = connected ? 1 : BLE_HS_CONN_HANDLE_NONE;
    }
    void setLastError(int error) { lastError_ = error; }
    void setDisconnectResult(bool result) { disconnectResult_ = result; }
    void setCancelConnectResult(bool result) { cancelConnectResult_ = result; }
    void setDisconnectEntryHook(std::function<void()> hook) { disconnectEntryHook_ = std::move(hook); }
    uint32_t disconnectCalls() const { return disconnectCalls_; }
    void emitDisconnect(int reason);
    void reset() {
        callbacks_ = nullptr;
        service_ = nullptr;
        connected_ = false;
        lastError_ = 0;
        secureConnectionResult_ = true;
        disconnectResult_ = true;
        cancelConnectResult_ = true;
        connHandle_ = BLE_HS_CONN_HANDLE_NONE;
        disconnectEntryHook_ = nullptr;
        disconnectCalls_ = 0;
    }

private:
    NimBLEClientCallbacks* callbacks_ = nullptr;
    NimBLERemoteService* service_ = nullptr;
    bool connected_ = false;
    bool secureConnectionResult_ = true;
    bool disconnectResult_ = true;
    bool cancelConnectResult_ = true;
    int lastError_ = 0;
    uint16_t connHandle_ = BLE_HS_CONN_HANDLE_NONE;
    std::function<void()> disconnectEntryHook_;
    uint32_t disconnectCalls_ = 0;
};

class NimBLEAdvertisementData {
public:
    void setFlags(uint8_t) {}
    void setCompleteServices(const NimBLEUUID&) {}
    void setAppearance(uint16_t) {}
    void setName(const char* name) { name_ = name ? name : ""; }
    const std::string& name() const { return name_; }

private:
    std::string name_;
};

class NimBLEAdvertising {
public:
    void setAdvertisementData(const NimBLEAdvertisementData& data) {
        g_mock_nimble_state.advertisementName = data.name();
    }
    void setScanResponseData(const NimBLEAdvertisementData& data) {
        g_mock_nimble_state.scanResponseName = data.name();
    }
    void setMinInterval(uint16_t interval) {
        g_mock_nimble_state.minInterval = interval;
        g_mock_nimble_state.setMinIntervalCalls++;
    }
    void setMaxInterval(uint16_t interval) {
        g_mock_nimble_state.maxInterval = interval;
        g_mock_nimble_state.setMaxIntervalCalls++;
    }
    bool start() {
        g_mock_nimble_state.advertising = true;
        g_mock_nimble_state.startAdvertisingCalls++;
        return true;
    }
    bool isAdvertising() const { return g_mock_nimble_state.advertising; }
};

class NimBLEScanCallbacks;

class NimBLEScan {
public:
    void setActiveScan(bool) {}
    void setScanCallbacks(NimBLEScanCallbacks* callbacks) {
        g_mock_nimble_state.scanCallbacks = callbacks;
        g_mock_nimble_state.setScanCallbacksCalls++;
    }
    void setInterval(uint16_t) {}
    void setWindow(uint16_t) {}
    void setMaxResults(uint16_t) {}
    void setDuplicateFilter(bool) {}
    void clearResults() {}
    bool start(uint32_t, bool, bool) {
        g_mock_nimble_state.scanStartCalls++;
        g_mock_nimble_state.scanning = g_mock_nimble_state.scanStartResult;
        return g_mock_nimble_state.scanStartResult;
    }
    void stop() { g_mock_nimble_state.scanning = false; }
    bool isScanning() const { return g_mock_nimble_state.scanning; }
};

class NimBLEDevice {
public:
    static void init(const char*) {}
    static void deinit(bool = false) {}
    static void setDeviceName(const char* name) {
        g_mock_nimble_state.deviceName = name ? name : "";
    }
    static void setPower(int) {}
    static bool setDefaultPhy(uint8_t, uint8_t) { return true; }
    static int getPower(NimBLETxPowerType = NimBLETxPowerType::All) { return 9; }
    static void setMTU(uint16_t) {}
    static NimBLEScan* getScan() { static NimBLEScan scan; return &scan; }
    static NimBLEServer* createServer() {
        g_mock_nimble_state.createServerCalls++;
        static NimBLEServer server;
        return &server;
    }
    static NimBLEClient* createClient() {
        static NimBLEClient client;
        return &client;
    }
    static NimBLEAdvertising* getAdvertising() {
        static NimBLEAdvertising advertising;
        return &advertising;
    }
    static bool startAdvertising() {
        g_mock_nimble_state.advertising = true;
        g_mock_nimble_state.startAdvertisingCalls++;
        return true;
    }
    static bool startAdvertising(int) {
        g_mock_nimble_state.advertising = true;
        g_mock_nimble_state.startAdvertisingCalls++;
        return true;
    }
    static void stopAdvertising() {
        g_mock_nimble_state.advertising = false;
        g_mock_nimble_state.stopAdvertisingCalls++;
    }
    static uint8_t getNumBonds() { return g_mock_nimble_state.bondCount; }
    static void deleteAllBonds() {}
    static bool isBonded(const NimBLEAddress&) { return false; }
    static bool deleteBond(const NimBLEAddress&) { return true; }
    static NimBLEAddress getAddress() { return NimBLEAddress(); }
    static void setSecurityAuth(bool, bool, bool) {}
    static void setSecurityIOCap(int) {}
    static void setSecurityInitKey(int) {}
    static void setSecurityRespKey(int) {}
};

class NimBLEClientCallbacks {
public:
    virtual ~NimBLEClientCallbacks() = default;
    virtual void onConnect(NimBLEClient*) {}
    virtual void onConnectFail(NimBLEClient*, int) {}
    virtual void onDisconnect(NimBLEClient*, int) {}
    virtual void onAuthenticationComplete(NimBLEConnInfo&) {}
    virtual void onIdentity(NimBLEConnInfo&) {}
    virtual void onPhyUpdate(NimBLEClient*, uint8_t, uint8_t) {}
};

inline void NimBLEClient::emitDisconnect(int reason) {
    connected_ = false;
    lastError_ = reason;
    if (callbacks_) {
        callbacks_->onDisconnect(this, reason);
    }
    connHandle_ = BLE_HS_CONN_HANDLE_NONE;
}

class NimBLEScanCallbacks {
public:
    virtual ~NimBLEScanCallbacks() = default;
    virtual void onResult(const NimBLEAdvertisedDevice*) {}
    virtual void onScanEnd(const NimBLEScanResults&, int) {}
};

class NimBLEServerCallbacks {
public:
    virtual ~NimBLEServerCallbacks() = default;
    virtual void onConnect(NimBLEServer*, NimBLEConnInfo&) {}
    virtual void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) {}
};

class NimBLECharacteristicCallbacks {
public:
    virtual ~NimBLECharacteristicCallbacks() = default;
    virtual void onWrite(NimBLECharacteristic*, NimBLEConnInfo&) {}
};

inline int ble_gap_conn_rssi(uint16_t, int8_t* rssi) {
    if (rssi) {
        *rssi = -55;
    }
    return 0;
}

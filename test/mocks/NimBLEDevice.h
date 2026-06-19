#pragma once

#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// Pull in FreeRTOS mock so headers that include <NimBLEDevice.h> get
// TickType_t, portMUX_TYPE, pdTRUE, xSemaphoreTake etc. automatically
#include "freertos/FreeRTOS.h"

// BLE security / pairing constants
static constexpr int BLE_HS_ETIMEOUT            = 5;
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
    explicit NimBLEAddress(const std::string&, uint8_t = BLE_ADDR_PUBLIC) {}
    bool isNull() const { return false; }
    std::string toString() const { return ""; }
    bool operator==(const NimBLEAddress&) const { return false; }
    bool operator!=(const NimBLEAddress&) const { return true; }
};

class NimBLEUUID {
public:
    NimBLEUUID() = default;
    explicit NimBLEUUID(const char* uuid)
        : value_(uuid ? uuid : "") {}
    explicit NimBLEUUID(uint16_t uuid)
        : value_(std::to_string(uuid)) {}

    int bitSize() const { return BLE_UUID_TYPE_128; }
    const uint8_t* getValue() const { return raw_; }
    void to16() {}

private:
    std::string value_;
    uint8_t raw_[16] = {0};
};

class NimBLEConnInfo {
public:
    uint16_t getConnHandle() const { return 1; }
    uint16_t getConnInterval() const { return 12; }
    uint16_t getConnLatency() const { return 0; }
};

class NimBLEAdvertisedDevice {
public:
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
    NimBLERemoteCharacteristic* getCharacteristic(const NimBLEUUID&) { return nullptr; }
    NimBLERemoteCharacteristic* getCharacteristic(const char*) { return nullptr; }
};

class NimBLERemoteDescriptor {
public:
    bool writeValue(const uint8_t*, size_t, bool = false) { return true; }
};

class NimBLERemoteCharacteristic {
public:
    bool writeValue(const uint8_t*, size_t, bool) { return true; }
    bool canWrite() const { return true; }
    bool canWriteNoResponse() const { return false; }
    bool canNotify() const { return true; }
    bool canIndicate() const { return false; }
    bool unsubscribe() { return true; }
    bool subscribe(bool, std::function<void(NimBLERemoteCharacteristic*, uint8_t*, size_t, bool)>, bool = false) { return true; }
    NimBLEUUID getUUID() const { return NimBLEUUID(""); }
    NimBLERemoteDescriptor* getDescriptor(const NimBLEUUID&) { return nullptr; }
};

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
    bool notify(const uint8_t*, size_t) {
        g_mock_nimble_state.characteristicNotifyCalls++;
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
    void setClientCallbacks(NimBLEClientCallbacks*) {}
    void setConnectionParams(uint16_t, uint16_t, uint16_t, uint16_t) {}
    void setConnectTimeout(unsigned long) {}
    bool isConnected() const { return connected_; }
    void disconnect() { connected_ = false; }
    int getRssi() const { return -60; }
    NimBLEConnInfo getConnInfo() const { return NimBLEConnInfo(); }
    NimBLEAddress getPeerAddress() const { return NimBLEAddress(); }
    bool connect(const NimBLEAddress&, bool = false, bool = false) { connected_ = true; return true; }
    int getLastError() const { return 0; }
    bool discoverAttributes() { return true; }
    NimBLERemoteService* getService(const NimBLEUUID&) { return nullptr; }
    NimBLERemoteService* getService(const char*) { return nullptr; }

private:
    bool connected_ = false;
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
    void setScanCallbacks(NimBLEScanCallbacks*) {}
    void setInterval(uint16_t) {}
    void setWindow(uint16_t) {}
    void setMaxResults(uint16_t) {}
    void setDuplicateFilter(bool) {}
    void clearResults() {}
    bool start(uint32_t, bool, bool) { scanning_ = true; return true; }
    void stop() { scanning_ = false; }
    bool isScanning() const { return scanning_; }

private:
    bool scanning_ = false;
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
    static void deleteBond(const NimBLEAddress&) {}
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
    virtual void onDisconnect(NimBLEClient*, int) {}
    virtual void onPhyUpdate(NimBLEClient*, uint8_t, uint8_t) {}
};

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

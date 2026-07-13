/// @file obd_ble_client.cpp
/// OBD-owned BLE client implementation. Fully independent of ble_client.cpp.

#include "obd_ble_client.h"

#ifndef UNIT_TEST

#include <Arduino.h>
#include <cstring>

// NimBLE's BLE controller resolves ALL scanned addresses against stored IRKs
// (resolving list populated from V1 bonds). A 24-bit AES hash false-positive
// can replace the OBDLink CX address with a zeroed identity address. Disable
// controller-level address resolution during OBD scans and re-enable afterward.
// ble_hs_pvcy_set_resolve_enabled is compiled when BLE_HOST_BASED_PRIVACY==0
// (ESP32-S3 with controller-based privacy).
extern "C" {
    int ble_hs_pvcy_set_resolve_enabled(int enable);
    #include "nimble/nimble/host/include/host/ble_hs.h"
}

#include "obd_runtime_module.h"
#include "obd_scan_policy.h"

namespace {

const NimBLEUUID kCxServiceUuid("FFF0");
const NimBLEUUID kCxNotifyUuid("FFF1");
const NimBLEUUID kCxWriteUuid("FFF2");

bool stringEquals(const std::string& lhs, const char* rhs) {
    return rhs != nullptr && lhs == rhs;
}

}  // namespace

ObdBleClient obdBleClient;

void ObdScanCallback::configure(ObdRuntimeModule* parent, int8_t minRssi) {
    parent_ = parent;
    minRssi_ = minRssi;
}

void ObdScanCallback::onResult(const NimBLEAdvertisedDevice* device) {
    if (!device || !parent_ || !device->haveName()) return;

    const std::string name = device->getName();
    if (!stringEquals(name, obd::DEVICE_NAME_CX)) {
        return;
    }

    const int rssi = device->getRSSI();
    if (rssi < minRssi_) {
        return;
    }

    const NimBLEAddress& addr = device->getAddress();
    if (addr.isNull()) {
        return;  // Identity resolution produced a null address — skip
    }

    // NimBLE may return identity types (PUBLIC_ID=2, RANDOM_ID=3) when it
    // resolves an RPA via a stored IRK.  The connect API only accepts
    // PUBLIC(0) or RANDOM(1), so strip the identity bit.
    uint8_t addrType = addr.getType();
    if (addrType >= 2) {
        addrType = addrType & 0x01;  // PUBLIC_ID→PUBLIC, RANDOM_ID→RANDOM
    }

    parent_->onDeviceFound(name.c_str(), addr.toString().c_str(), rssi, addrType);
    NimBLEDevice::getScan()->stop();
}

void ObdScanCallback::onScanEnd(const NimBLEScanResults& /*results*/, int /*reason*/) {
    ble_hs_pvcy_set_resolve_enabled(1);
}

void ObdClientCallback::configure(ObdBleClient* owner, ObdRuntimeModule* parent) {
    owner_ = owner;
    parent_ = parent;
}

void ObdClientCallback::onConnect(NimBLEClient* /*client*/) {
    if (owner_) {
        owner_->handleConnected();
    }
}

void ObdClientCallback::onConnectFail(NimBLEClient* /*client*/, int reason) {
    if (owner_) {
        owner_->handleDisconnected(reason);
    }
    if (parent_) {
        parent_->onBleDisconnect(reason);
    }
}

void ObdClientCallback::onDisconnect(NimBLEClient* /*client*/, int reason) {
    if (owner_) {
        owner_->handleDisconnected(reason);
    }
    if (parent_) {
        parent_->onBleDisconnect(reason);
    }
}

void ObdClientCallback::onAuthenticationComplete(NimBLEConnInfo& connInfo) {
    if (owner_) {
        owner_->handleAuthenticationComplete(connInfo);
    }
}

void ObdClientCallback::onIdentity(NimBLEConnInfo& connInfo) {
    if (owner_) {
        owner_->handleIdentityResolved(connInfo);
    }
}

void ObdBleClient::clearLinkState(bool clearErrors) {
    connectPending_ = false;
    securityPending_ = false;
    securityReady_ = false;
    encrypted_ = false;
    bonded_ = false;
    authenticated_ = false;
    pTxChar_ = nullptr;
    pRxChar_ = nullptr;
    if (clearErrors) {
        lastBleError_ = 0;
        lastSecurityError_ = 0;
    }
}

void ObdBleClient::syncSecurityStateFromConnInfo() {
    if (!pClient_ || !pClient_->isConnected()) {
        return;
    }

    NimBLEConnInfo info = pClient_->getConnInfo();
    encrypted_ = info.isEncrypted();
    bonded_ = info.isBonded();
    authenticated_ = info.isAuthenticated();
    if (encrypted_) {
        securityReady_ = true;
        securityPending_ = false;
        lastSecurityError_ = 0;
    }
}

void ObdBleClient::handleConnected() {
    connectPending_ = false;
    lastBleError_ = 0;
    securityPending_ = false;
    securityReady_ = false;
    encrypted_ = false;
    bonded_ = false;
    authenticated_ = false;
    syncSecurityStateFromConnInfo();
}

void ObdBleClient::handleDisconnected(int reason) {
    lastBleError_ = reason;
    if (securityPending_ && !securityReady_ && lastSecurityError_ == 0) {
        lastSecurityError_ = reason;
    }
    clearLinkState(false);
}

void ObdBleClient::handleAuthenticationComplete(const NimBLEConnInfo& connInfo) {
    encrypted_ = connInfo.isEncrypted();
    bonded_ = connInfo.isBonded();
    authenticated_ = connInfo.isAuthenticated();
    securityPending_ = false;
    securityReady_ = encrypted_;
    lastBleError_ = encrypted_ ? 0 : lastBleError_;
    lastSecurityError_ = encrypted_ ? 0 : lastBleError_;
}

void ObdBleClient::handleIdentityResolved(const NimBLEConnInfo& connInfo) {
    bonded_ = connInfo.isBonded();
    authenticated_ = connInfo.isAuthenticated();
    encrypted_ = connInfo.isEncrypted();
    if (encrypted_) {
        securityReady_ = true;
        securityPending_ = false;
    }
}

void ObdBleClient::init(ObdRuntimeModule* parent) {
    if (pClient_ != nullptr) return;

    pClient_ = NimBLEDevice::createClient();
    clientCallback_.configure(this, parent);
    pClient_->setClientCallbacks(&clientCallback_);
    // min=12 (15ms), max=40 (50ms): give the BLE 4.2 OBDLink CX room to
    // negotiate a comfortable interval.  Fixed min==max==12 caused connection
    // parameter update rejections when the CX requested a wider interval.
    pClient_->setConnectionParams(12, 40, 0, 400);
    pClient_->setConnectTimeout(obd::CONNECT_TIMEOUT_MS);
}

bool ObdBleClient::startScan(ObdRuntimeModule* parent, int8_t minRssi) {
    NimBLEScan* pScan = NimBLEDevice::getScan();
    if (pScan->isScanning()) return false;

    ble_hs_pvcy_set_resolve_enabled(0);

    scanCallback_.configure(parent, minRssi);
    pScan->setScanCallbacks(&scanCallback_);
    pScan->setActiveScan(true);
    pScan->setInterval(100);
    pScan->setWindow(75);
    pScan->setMaxResults(0);
    pScan->setDuplicateFilter(false);

    const bool started = pScan->start(obd::SCAN_DURATION_MS, false, false);
    if (!started) {
        // Failed start means onScanEnd will never fire: re-enable RPA
        // resolution here instead of leaking it disabled stack-wide.
        ble_hs_pvcy_set_resolve_enabled(1);
    }
    return started;
}

void ObdBleClient::stopScan() {
    NimBLEScan* pScan = NimBLEDevice::getScan();
    if (pScan->isScanning()) {
        pScan->stop();
    }
    ble_hs_pvcy_set_resolve_enabled(1);
}

bool ObdBleClient::connect(const char* address, uint8_t addrType, uint32_t timeoutMs, bool preferCachedAttributes) {
    if (!pClient_ || !address || address[0] == '\0') return false;
    if (pClient_->isConnected()) return true;

    clearLinkState(true);

    NimBLEAddress addr{std::string(address), addrType};
    pClient_->setConnectTimeout(timeoutMs);
    connectPending_ = true;
    // exchangeMTU=false: the global MTU is 517 (set for V1 BLE 5.x) which can
    // race with GATT discovery on the BLE 4.2 OBDLink CX.  OBD responses are
    // ~10 bytes so the default 23-byte ATT MTU is sufficient.
    const bool ok = pClient_->connect(addr, !preferCachedAttributes, true, false);
    if (!ok) {
        Serial.println("[OBD] connect() returned false");
        lastBleError_ = pClient_->getLastError();
        connectPending_ = false;
    }
    return ok;
}

void ObdBleClient::disconnect() {
    if (!pClient_) {
        clearLinkState(false);
        return;
    }

    if (!pClient_->isConnected()) {
        pClient_->cancelConnect();
    } else {
        pClient_->disconnect();
    }

    clearLinkState(false);
}

bool ObdBleClient::isConnected() const {
    return pClient_ && pClient_->isConnected();
}

bool ObdBleClient::beginSecurity() {
    if (!pClient_ || !pClient_->isConnected()) {
        lastSecurityError_ = BLE_HS_ENOTCONN;
        return false;
    }

    syncSecurityStateFromConnInfo();
    if (securityReady_) {
        return true;
    }
    if (securityPending_) {
        return true;
    }

    lastSecurityError_ = 0;
    const bool ok = pClient_->secureConnection(true);
    if (!ok) {
        lastSecurityError_ = pClient_->getLastError();
        lastBleError_ = lastSecurityError_;
        securityPending_ = false;
        return false;
    }

    securityPending_ = true;
    return true;
}

bool ObdBleClient::isSecurityReady() const {
    if (pClient_ && pClient_->isConnected()) {
        const_cast<ObdBleClient*>(this)->syncSecurityStateFromConnInfo();
    }
    return securityReady_;
}

bool ObdBleClient::isEncrypted() const {
    if (pClient_ && pClient_->isConnected()) {
        const_cast<ObdBleClient*>(this)->syncSecurityStateFromConnInfo();
    }
    return encrypted_;
}

bool ObdBleClient::isBonded() const {
    if (pClient_ && pClient_->isConnected()) {
        const_cast<ObdBleClient*>(this)->syncSecurityStateFromConnInfo();
    }
    return bonded_;
}

bool ObdBleClient::isAuthenticated() const {
    if (pClient_ && pClient_->isConnected()) {
        const_cast<ObdBleClient*>(this)->syncSecurityStateFromConnInfo();
    }
    return authenticated_;
}

bool ObdBleClient::deleteBond(const char* address, uint8_t addrType) {
    if (!address || address[0] == '\0') {
        return false;
    }

    NimBLEAddress addr{std::string(address), addrType};
    if (!NimBLEDevice::isBonded(addr) && pClient_ && pClient_->isConnected()) {
        const NimBLEConnInfo info = pClient_->getConnInfo();
        const NimBLEAddress idAddr = info.getIdAddress();
        if (!idAddr.isNull() && NimBLEDevice::isBonded(idAddr)) {
            addr = idAddr;
        }
    }

    if (!NimBLEDevice::isBonded(addr)) {
        return false;
    }

    const bool deleted = NimBLEDevice::deleteBond(addr);
    if (deleted && pClient_ && pClient_->isConnected() && pClient_->getPeerAddress() == addr) {
        clearLinkState(false);
    }
    return deleted;
}

bool ObdBleClient::discoverServices() {
    if (!pClient_ || !pClient_->isConnected()) {
        Serial.println("[OBD] discoverServices: not connected");
        return false;
    }

    syncSecurityStateFromConnInfo();
    connectPending_ = false;
    // Force a full GATT attribute refresh before accessing services.
    // Main's working flow calls discoverAttributes() explicitly — without it,
    // NimBLE may use stale/missing attribute handles and silently fail.
    if (!pClient_->discoverAttributes()) {
        Serial.println("[OBD] discoverAttributes failed (continuing anyway)");
    }

    NimBLERemoteService* svc = pClient_->getService(kCxServiceUuid);
    if (!svc) {
        Serial.println("[OBD] discoverServices: FFF0 service not found");
        lastBleError_ = pClient_->getLastError();
        return false;
    }

    pTxChar_ = svc->getCharacteristic(kCxNotifyUuid);
    pRxChar_ = svc->getCharacteristic(kCxWriteUuid);
    if (!pTxChar_ || !pRxChar_) {
        Serial.printf("[OBD] discoverServices: char missing tx=%d rx=%d\n",
                      pTxChar_ != nullptr, pRxChar_ != nullptr);
        lastBleError_ = pClient_->getLastError();
        pTxChar_ = nullptr;
        pRxChar_ = nullptr;
        return false;
    }

    if (!pTxChar_->canNotify() || !(pRxChar_->canWrite() || pRxChar_->canWriteNoResponse())) {
        Serial.printf("[OBD] discoverServices: capability mismatch notify=%d write=%d writeNR=%d\n",
                      pTxChar_->canNotify(), pRxChar_->canWrite(), pRxChar_->canWriteNoResponse());
        lastBleError_ = pClient_->getLastError();
        pTxChar_ = nullptr;
        pRxChar_ = nullptr;
        return false;
    }

    return true;
}

bool ObdBleClient::writeCommand(const char* cmd, bool withResponse) {
    if (!pRxChar_ || !pClient_ || !pClient_->isConnected() || !cmd) return false;
    syncSecurityStateFromConnInfo();
    const bool ok = pRxChar_->writeValue(reinterpret_cast<const uint8_t*>(cmd), strlen(cmd), withResponse);
    lastBleError_ = ok ? 0 : pClient_->getLastError();
    return ok;
}

bool ObdBleClient::subscribeNotify(void (*callback)(const uint8_t* data, size_t len)) {
    if (!pTxChar_) return false;
    if (!pClient_ || !pClient_->isConnected()) {
        Serial.println("[OBD] subscribeNotify: connection lost before subscribe");
        return false;
    }
    syncSecurityStateFromConnInfo();

    // Main's working flow: simple subscribe(true, callback) — two args,
    // defaults to CCCD write-no-response.  The DA14531 rejects
    // write-with-response for CCCD, and trying it first can leave the GATT
    // state machine confused, so match main exactly.
    const bool ok = pTxChar_->subscribe(
        true,
        [callback](NimBLERemoteCharacteristic* /*chr*/, uint8_t* data, size_t length, bool /*isNotify*/) {
            if (callback && data && length > 0) {
                callback(data, length);
            }
        });
    lastBleError_ = ok ? 0 : pClient_->getLastError();
    if (!ok) {
        Serial.printf("[OBD] subscribeNotify: failed rc=%d\n", lastBleError_);
    }
    return ok;
}

int8_t ObdBleClient::getRssi(uint32_t nowMs) {
    if (!pClient_ || !pClient_->isConnected()) return 0;

    if (nowMs - lastRssiQueryMs_ >= RSSI_QUERY_INTERVAL_MS) {
        lastRssiQueryMs_ = nowMs;
        cachedRssi_ = static_cast<int8_t>(pClient_->getRssi());
    }
    return cachedRssi_;
}

#endif  // UNIT_TEST

/// @file obd_ble_client.cpp
/// OBD-owned BLE client implementation. Fully independent of ble_client.cpp.

#include "obd_ble_client.h"

#if !defined(UNIT_TEST) || defined(V1_LINKED_TEST_OBD_BLE_CLIENT)

#include <Arduino.h>
#include <cstring>

// NimBLE's BLE controller resolves ALL scanned addresses against stored IRKs
// (resolving list populated from V1 bonds). A 24-bit AES hash false-positive
// can replace the OBDLink CX address with a zeroed identity address. Disable
// controller-level address resolution during OBD scans and re-enable afterward.
// ble_hs_pvcy_set_resolve_enabled is compiled when BLE_HOST_BASED_PRIVACY==0
// (ESP32-S3 with controller-based privacy).
#ifndef UNIT_TEST
extern "C" {
int ble_hs_pvcy_set_resolve_enabled(int enable);
#include "nimble/nimble/host/include/host/ble_hs.h"
}
#else
extern "C" int ble_hs_pvcy_set_resolve_enabled(int enable);
#endif

#include "obd_runtime_module.h"
#include "obd_scan_policy.h"

namespace {

const NimBLEUUID kCxServiceUuid("FFF0");
const NimBLEUUID kCxNotifyUuid("FFF1");
const NimBLEUUID kCxWriteUuid("FFF2");

bool stringEquals(const std::string& lhs, const char* rhs) {
    return rhs != nullptr && lhs == rhs;
}

} // namespace

ObdBleClient obdBleClient;

void ObdScanCallback::configure(ObdRuntimeModule* parent, int8_t minRssi) {
    parent_ = parent;
    minRssi_ = minRssi;
}

void ObdScanCallback::onResult(const NimBLEAdvertisedDevice* device) {
    if (!device || !parent_ || !device->haveName())
        return;

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
        return; // Identity resolution produced a null address — skip
    }

    // NimBLE may return identity types (PUBLIC_ID=2, RANDOM_ID=3) when it
    // resolves an RPA via a stored IRK.  The connect API only accepts
    // PUBLIC(0) or RANDOM(1), so strip the identity bit.
    uint8_t addrType = addr.getType();
    if (addrType >= 2) {
        addrType = addrType & 0x01; // PUBLIC_ID→PUBLIC, RANDOM_ID→RANDOM
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
    connectPending_.store(false, std::memory_order_release);
    securityPending_.store(false, std::memory_order_release);
    securityReady_.store(false, std::memory_order_release);
    encrypted_.store(false, std::memory_order_release);
    bonded_.store(false, std::memory_order_release);
    authenticated_.store(false, std::memory_order_release);
    if (clearErrors) {
        lastBleError_.store(0, std::memory_order_release);
        lastSecurityError_.store(0, std::memory_order_release);
    }
}

void ObdBleClient::clearCharacteristicHandles() {
    pTxChar_ = nullptr;
    pRxChar_ = nullptr;
}

void ObdBleClient::serviceDeferredLinkState() {
    uint32_t generation = 0;
    int reason = 0;
    if (linkDownFence_.consume(generation, reason)) {
        (void)generation;
        (void)reason;
        clearCharacteristicHandles();
    }
}

void ObdBleClient::syncSecurityStateFromConnInfo() {
    if (!pClient_ || !pClient_->isConnected()) {
        return;
    }

    NimBLEConnInfo info = pClient_->getConnInfo();
    const bool encrypted = info.isEncrypted();
    encrypted_.store(encrypted, std::memory_order_release);
    bonded_.store(info.isBonded(), std::memory_order_release);
    authenticated_.store(info.isAuthenticated(), std::memory_order_release);
    if (encrypted) {
        securityReady_.store(true, std::memory_order_release);
        securityPending_.store(false, std::memory_order_release);
        lastSecurityError_.store(0, std::memory_order_release);
    }
}

void ObdBleClient::handleConnected() {
    connectPending_.store(false, std::memory_order_release);
    lastBleError_.store(0, std::memory_order_release);
    securityPending_.store(false, std::memory_order_release);
    securityReady_.store(false, std::memory_order_release);
    encrypted_.store(false, std::memory_order_release);
    bonded_.store(false, std::memory_order_release);
    authenticated_.store(false, std::memory_order_release);
    syncSecurityStateFromConnInfo();
}

void ObdBleClient::handleDisconnected(int reason) {
    lastBleError_.store(reason, std::memory_order_release);
    if (securityPending_.load(std::memory_order_acquire) && !securityReady_.load(std::memory_order_acquire) &&
        lastSecurityError_.load(std::memory_order_acquire) == 0) {
        lastSecurityError_.store(reason, std::memory_order_release);
    }
    clearLinkState(false);
    const uint32_t generation = activeLinkGeneration_.load(std::memory_order_acquire);
    confirmedDownGeneration_.store(generation, std::memory_order_release);
    linkDownFence_.publish(generation, reason);
}

void ObdBleClient::handleAuthenticationComplete(const NimBLEConnInfo& connInfo) {
    const bool encrypted = connInfo.isEncrypted();
    encrypted_.store(encrypted, std::memory_order_release);
    bonded_.store(connInfo.isBonded(), std::memory_order_release);
    authenticated_.store(connInfo.isAuthenticated(), std::memory_order_release);
    securityPending_.store(false, std::memory_order_release);
    securityReady_.store(encrypted, std::memory_order_release);
    if (encrypted) {
        lastBleError_.store(0, std::memory_order_release);
        lastSecurityError_.store(0, std::memory_order_release);
    } else {
        lastSecurityError_.store(lastBleError_.load(std::memory_order_acquire), std::memory_order_release);
    }
}

void ObdBleClient::handleIdentityResolved(const NimBLEConnInfo& connInfo) {
    bonded_.store(connInfo.isBonded(), std::memory_order_release);
    authenticated_.store(connInfo.isAuthenticated(), std::memory_order_release);
    const bool encrypted = connInfo.isEncrypted();
    encrypted_.store(encrypted, std::memory_order_release);
    if (encrypted) {
        securityReady_.store(true, std::memory_order_release);
        securityPending_.store(false, std::memory_order_release);
    }
}

void ObdBleClient::init(ObdRuntimeModule* parent) {
    if (pClient_ != nullptr)
        return;

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
    if (pScan->isScanning())
        return false;

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
    if (!pClient_ || !address || address[0] == '\0')
        return false;
    if (pClient_->isConnected())
        return true;

    serviceDeferredLinkState();
    clearCharacteristicHandles();
    clearLinkState(true);
    uint32_t generation = activeLinkGeneration_.load(std::memory_order_relaxed) + 1;
    if (generation == 0) {
        generation = 1;
    }
    activeLinkGeneration_.store(generation, std::memory_order_release);

    NimBLEAddress addr{std::string(address), addrType};
    pClient_->setConnectTimeout(timeoutMs);
    connectPending_.store(true, std::memory_order_release);
    // exchangeMTU=false: the global MTU is 517 (set for V1 BLE 5.x) which can
    // race with GATT discovery on the BLE 4.2 OBDLink CX.  OBD responses are
    // ~10 bytes so the default 23-byte ATT MTU is sufficient.
    const bool ok = pClient_->connect(addr, !preferCachedAttributes, true, false);
    if (!ok) {
        Serial.println("[OBD] connect() returned false");
        lastBleError_.store(pClient_->getLastError(), std::memory_order_release);
        connectPending_.store(false, std::memory_order_release);
        confirmedDownGeneration_.store(generation, std::memory_order_release);
    }
    return ok;
}

bool ObdBleClient::disconnect() {
    serviceDeferredLinkState();
    if (!pClient_) {
        clearLinkState(false);
        clearCharacteristicHandles();
        confirmedDownGeneration_.store(activeLinkGeneration_.load(std::memory_order_acquire),
                                       std::memory_order_release);
        return true;
    }

    const bool connected = pClient_->isConnected();
    const bool connectPending = connectPending_.load(std::memory_order_acquire);
    const bool hasConnectionHandle = pClient_->getConnHandle() != static_cast<uint16_t>(-1);
    if (!connected && !connectPending && !hasConnectionHandle) {
        clearLinkState(false);
        clearCharacteristicHandles();
        confirmedDownGeneration_.store(activeLinkGeneration_.load(std::memory_order_acquire),
                                       std::memory_order_release);
        return true;
    }

    const bool accepted = connected || hasConnectionHandle ? pClient_->disconnect() : pClient_->cancelConnect();
    if (!accepted) {
        lastBleError_.store(pClient_->getLastError(), std::memory_order_release);
        return false;
    }

    // The transport owner has fenced all new work, so handles can be retired
    // now. Callback-visible connection state remains authoritative until the
    // actual link-down callback arrives.
    clearCharacteristicHandles();
    return true;
}

bool ObdBleClient::isConnected() const {
    return pClient_ && pClient_->isConnected();
}

bool ObdBleClient::beginSecurity() {
    if (!pClient_ || !pClient_->isConnected()) {
        lastSecurityError_.store(BLE_HS_ENOTCONN, std::memory_order_release);
        return false;
    }

    syncSecurityStateFromConnInfo();
    if (securityReady_.load(std::memory_order_acquire)) {
        return true;
    }
    if (securityPending_.load(std::memory_order_acquire)) {
        return true;
    }

    lastSecurityError_.store(0, std::memory_order_release);
    const bool ok = pClient_->secureConnection(true);
    if (!ok) {
        const int error = pClient_->getLastError();
        lastSecurityError_.store(error, std::memory_order_release);
        lastBleError_.store(error, std::memory_order_release);
        securityPending_.store(false, std::memory_order_release);
        return false;
    }

    securityPending_.store(true, std::memory_order_release);
    return true;
}

bool ObdBleClient::isSecurityReady() const {
    if (pClient_ && pClient_->isConnected()) {
        const_cast<ObdBleClient*>(this)->syncSecurityStateFromConnInfo();
    }
    return securityReady_.load(std::memory_order_acquire);
}

bool ObdBleClient::isEncrypted() const {
    if (pClient_ && pClient_->isConnected()) {
        const_cast<ObdBleClient*>(this)->syncSecurityStateFromConnInfo();
    }
    return encrypted_.load(std::memory_order_acquire);
}

bool ObdBleClient::isBonded() const {
    if (pClient_ && pClient_->isConnected()) {
        const_cast<ObdBleClient*>(this)->syncSecurityStateFromConnInfo();
    }
    return bonded_.load(std::memory_order_acquire);
}

bool ObdBleClient::isAuthenticated() const {
    if (pClient_ && pClient_->isConnected()) {
        const_cast<ObdBleClient*>(this)->syncSecurityStateFromConnInfo();
    }
    return authenticated_.load(std::memory_order_acquire);
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
    serviceDeferredLinkState();
    if (!pClient_ || !pClient_->isConnected()) {
        Serial.println("[OBD] discoverServices: not connected");
        return false;
    }

    syncSecurityStateFromConnInfo();
    connectPending_.store(false, std::memory_order_release);
    // Force a full GATT attribute refresh before accessing services.
    // Main's working flow calls discoverAttributes() explicitly — without it,
    // NimBLE may use stale/missing attribute handles and silently fail.
    if (!pClient_->discoverAttributes()) {
        Serial.println("[OBD] discoverAttributes failed (continuing anyway)");
    }

    NimBLERemoteService* svc = pClient_->getService(kCxServiceUuid);
    if (!svc) {
        Serial.println("[OBD] discoverServices: FFF0 service not found");
        lastBleError_.store(pClient_->getLastError(), std::memory_order_release);
        return false;
    }

    pTxChar_ = svc->getCharacteristic(kCxNotifyUuid);
    pRxChar_ = svc->getCharacteristic(kCxWriteUuid);
    if (!pTxChar_ || !pRxChar_) {
        Serial.printf("[OBD] discoverServices: char missing tx=%d rx=%d\n", pTxChar_ != nullptr, pRxChar_ != nullptr);
        lastBleError_.store(pClient_->getLastError(), std::memory_order_release);
        clearCharacteristicHandles();
        return false;
    }

    if (!pTxChar_->canNotify() || !(pRxChar_->canWrite() || pRxChar_->canWriteNoResponse())) {
        Serial.printf("[OBD] discoverServices: capability mismatch notify=%d write=%d writeNR=%d\n",
                      pTxChar_->canNotify(), pRxChar_->canWrite(), pRxChar_->canWriteNoResponse());
        lastBleError_.store(pClient_->getLastError(), std::memory_order_release);
        clearCharacteristicHandles();
        return false;
    }

    return !linkDownFence_.pending();
}

bool ObdBleClient::writeCommand(const char* cmd, bool withResponse) {
    serviceDeferredLinkState();
    NimBLERemoteCharacteristic* const rxChar = pRxChar_;
    if (!rxChar || !pClient_ || !pClient_->isConnected() || !cmd || linkDownFence_.pending())
        return false;
    syncSecurityStateFromConnInfo();
    const bool ok = rxChar->writeValue(reinterpret_cast<const uint8_t*>(cmd), strlen(cmd), withResponse);
    const bool linkStayedUp = !linkDownFence_.pending();
    lastBleError_.store(ok && linkStayedUp ? 0 : pClient_->getLastError(), std::memory_order_release);
    return ok && linkStayedUp;
}

bool ObdBleClient::subscribeNotify(void (*callback)(const uint8_t* data, size_t len)) {
    serviceDeferredLinkState();
    NimBLERemoteCharacteristic* const txChar = pTxChar_;
    if (!txChar || linkDownFence_.pending())
        return false;
    if (!pClient_ || !pClient_->isConnected()) {
        Serial.println("[OBD] subscribeNotify: connection lost before subscribe");
        return false;
    }
    syncSecurityStateFromConnInfo();

    // Main's working flow: simple subscribe(true, callback) — two args,
    // defaults to CCCD write-no-response.  The DA14531 rejects
    // write-with-response for CCCD, and trying it first can leave the GATT
    // state machine confused, so match main exactly.
    const bool ok = txChar->subscribe(
        true, [callback](NimBLERemoteCharacteristic* /*chr*/, uint8_t* data, size_t length, bool /*isNotify*/) {
            if (callback && data && length > 0) {
                callback(data, length);
            }
        });
    const bool linkStayedUp = !linkDownFence_.pending();
    const int error = ok && linkStayedUp ? 0 : pClient_->getLastError();
    lastBleError_.store(error, std::memory_order_release);
    if (!ok || !linkStayedUp) {
        Serial.printf("[OBD] subscribeNotify: failed rc=%d\n", error);
    }
    return ok && linkStayedUp;
}

int8_t ObdBleClient::getRssi(uint32_t nowMs) {
    if (!pClient_ || !pClient_->isConnected())
        return 0;

    if (nowMs - lastRssiQueryMs_ >= RSSI_QUERY_INTERVAL_MS) {
        lastRssiQueryMs_ = nowMs;
        cachedRssi_ = static_cast<int8_t>(pClient_->getRssi());
    }
    return cachedRssi_;
}

#endif // !UNIT_TEST || V1_LINKED_TEST_OBD_BLE_CLIENT

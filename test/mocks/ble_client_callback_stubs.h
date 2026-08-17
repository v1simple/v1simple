#pragma once

// Partial V1BLEClient tests do not link ble_connection.cpp or ble_proxy.cpp.
// GCC's vptr sanitizer still needs the nested callback classes' RTTI when it
// instantiates the client's unique_ptr deleters, so complete their virtual
// method definitions in those test translation units.
inline void V1BLEClient::ClientCallbacks::onConnect(NimBLEClient*) {}
inline void V1BLEClient::ClientCallbacks::onConnectFail(NimBLEClient*, int) {}
inline void V1BLEClient::ClientCallbacks::onDisconnect(NimBLEClient*, int) {}
inline void V1BLEClient::ClientCallbacks::onPhyUpdate(NimBLEClient*, uint8_t, uint8_t) {}

inline void V1BLEClient::ScanCallbacks::onResult(const NimBLEAdvertisedDevice*) {}
inline void V1BLEClient::ScanCallbacks::onScanEnd(const NimBLEScanResults&, int) {}

inline void V1BLEClient::ProxyServerCallbacks::onConnect(NimBLEServer*, NimBLEConnInfo&) {}
inline void V1BLEClient::ProxyServerCallbacks::onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) {}

inline void V1BLEClient::ProxyWriteCallbacks::onWrite(NimBLECharacteristic*, NimBLEConnInfo&) {}

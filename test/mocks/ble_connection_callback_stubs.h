#pragma once

// Tests that omit ble_connection.cpp still need complete callback definitions:
// GCC's vptr sanitizer references their RTTI from the client's unique_ptr deleters.
inline void V1BLEClient::ClientCallbacks::onConnect(NimBLEClient*) {}
inline void V1BLEClient::ClientCallbacks::onConnectFail(NimBLEClient*, int) {}
inline void V1BLEClient::ClientCallbacks::onDisconnect(NimBLEClient*, int) {}
inline void V1BLEClient::ClientCallbacks::onPhyUpdate(NimBLEClient*, uint8_t, uint8_t) {}

inline void V1BLEClient::ScanCallbacks::onResult(const NimBLEAdvertisedDevice*) {}
inline void V1BLEClient::ScanCallbacks::onScanEnd(const NimBLEScanResults&, int) {}

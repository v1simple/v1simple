#pragma once

#include "ble_connection_callback_stubs.h"

// Partial tests that also omit ble_proxy.cpp need its callback definitions too.
inline void V1BLEClient::ProxyServerCallbacks::onConnect(NimBLEServer*, NimBLEConnInfo&) {}
inline void V1BLEClient::ProxyServerCallbacks::onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) {}

inline void V1BLEClient::ProxyWriteCallbacks::onWrite(NimBLECharacteristic*, NimBLEConnInfo&) {}

#pragma once

#include <WiFi.h>
#include <ArduinoJson.h>

#include "wifi_manager.h"
#include "ble_client.h"
#include "packet_parser.h"
#include "storage_manager.h"
#include "modules/auto_push/auto_push_module.h"

class WifiOrchestrator {
public:
    WifiOrchestrator(WiFiManager& wifiManager,
                     V1BLEClient& bleClient,
                     PacketParser& parser,
                     StorageManager& storageManager,
                     AutoPushModule& autoPushModule);

    void ensureCallbacksConfigured();

private:
    void configureCallbacks();

    WiFiManager& wifiManager;
    V1BLEClient& bleClient;
    PacketParser& parser;
    StorageManager& storageManager;
    AutoPushModule& autoPushModule;
    bool callbacksConfigured_ = false;
};

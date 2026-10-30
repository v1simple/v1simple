#pragma once

#include <WiFi.h>
#include <ArduinoJson.h>

#include "wifi_manager.h"
#include "ble_client.h"
#include "packet_parser.h"

class WifiOrchestrator {
  public:
    WifiOrchestrator(WiFiManager& wifiManager, V1BLEClient& bleClient, PacketParser& parser);

    void ensureCallbacksConfigured();

  private:
    void configureCallbacks();

    WiFiManager& wifiManager;
    V1BLEClient& bleClient;
    PacketParser& parser;
    bool callbacksConfigured_ = false;
};

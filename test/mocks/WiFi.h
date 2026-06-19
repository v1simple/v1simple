#pragma once

#ifndef WIFI_H
#define WIFI_H

#include "Arduino.h"

// WiFi status codes
#ifndef WL_CONNECTED
#define WL_CONNECTED    3
#endif
#ifndef WL_DISCONNECTED
#define WL_DISCONNECTED 6
#endif
#ifndef WL_IDLE_STATUS
#define WL_IDLE_STATUS  0
#endif

// WiFi auth types
#ifndef WIFI_AUTH_OPEN
#define WIFI_AUTH_OPEN 0
#endif
#ifndef WIFI_AUTH_WEP
#define WIFI_AUTH_WEP 1
#endif
#ifndef WIFI_AUTH_WPA_PSK
#define WIFI_AUTH_WPA_PSK 2
#endif
#ifndef WIFI_AUTH_WPA2_PSK
#define WIFI_AUTH_WPA2_PSK 3
#endif
#ifndef WIFI_AUTH_WPA_WPA2_PSK
#define WIFI_AUTH_WPA_WPA2_PSK 4
#endif
#ifndef WIFI_AUTH_WPA2_ENTERPRISE
#define WIFI_AUTH_WPA2_ENTERPRISE 5
#endif
#ifndef WIFI_AUTH_WPA3_PSK
#define WIFI_AUTH_WPA3_PSK 6
#endif
#ifndef WIFI_AUTH_WPA2_WPA3_PSK
#define WIFI_AUTH_WPA2_WPA3_PSK 7
#endif

// WiFi mode type
#ifndef WIFI_MODE_T_DEFINED
#define WIFI_MODE_T_DEFINED
typedef enum {
    WIFI_MODE_NULL  = 0,
    WIFI_MODE_STA   = 1,
    WIFI_MODE_AP    = 2,
    WIFI_MODE_APSTA = 3,
} wifi_mode_t;
#endif
#ifndef WIFI_STA
#define WIFI_STA    WIFI_MODE_STA
#endif
#ifndef WIFI_AP_STA
#define WIFI_AP_STA WIFI_MODE_APSTA
#endif

// Minimal WiFi stub for native tests
class WiFiClass {
public:
    uint16_t softAPgetStationNum() const { return apStationCount_; }
    void setApStationCount(uint16_t n) { apStationCount_ = n; }
    int status() const { return WL_DISCONNECTED; }
    wifi_mode_t getMode() const { return WIFI_MODE_NULL; }
private:
    uint16_t apStationCount_ = 0;
};

extern WiFiClass WiFi;

#endif  // WIFI_H

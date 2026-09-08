#pragma once
#include "Arduino.h"
#include <cassert>
#include <functional>

using wl_status_t = int;
enum wifi_mode_t { WIFI_OFF = 0, WIFI_STA = 1, WIFI_AP = 2, WIFI_AP_STA = 3 };
constexpr int WL_IDLE_STATUS = 0, WL_NO_SSID_AVAIL = 1, WL_CONNECTED = 3, WL_CONNECT_FAILED = 4, WL_DISCONNECTED = 6;
constexpr int WIFI_SCAN_RUNNING = -1, WIFI_SCAN_FAILED = -2, WIFI_AUTH_OPEN = 0;

struct FakeIP {
    uint32_t value;
    operator uint32_t() const { return value; }
};

// Arduino publishes connection events asynchronously. Keep the cached status
// separate from station availability so either mode-change notification timing
// is covered. Scans start only disconnected or already connected, never during
// an in-progress connection (which the target driver rejects).
class WiFiClass {
  public:
    wifi_mode_t radioMode = WIFI_AP_STA;
    bool physicalConnected = true;
    bool autoReconnect = true;
    bool publishStopImmediately = true;
    wl_status_t cachedStatus = WL_CONNECTED;
    int scanState = WIFI_SCAN_FAILED;
    int scans = 0;
    int scanStatusReads = 0;
    int scanAborts = 0;
    int apOnlyTransitions = 0;
    int disconnects = 0;
    int begins = 0;
    bool startedScanConnected = false;
    std::function<void()> beforeScan;

    wl_status_t status() const { return cachedStatus; }
    wifi_mode_t getMode() const { return radioMode; }
    bool mode(wifi_mode_t mode) {
        if (mode == WIFI_AP && radioMode != WIFI_AP) {
            ++apOnlyTransitions;
            physicalConnected = false;
            if (publishStopImmediately) cachedStatus = WL_DISCONNECTED;
        }
        radioMode = mode;
        return true;
    }
    bool setSleep(bool) { return true; }
    bool setAutoReconnect(bool enabled) { autoReconnect = enabled; return true; }
    bool disconnect(bool = false, bool = false) {
        ++disconnects;
        physicalConnected = false;
        cachedStatus = WL_DISCONNECTED;
        return true;
    }
    void scanDelete() { scanState = WIFI_SCAN_FAILED; }
    int16_t scanNetworks(bool, bool, bool, unsigned) {
        if (beforeScan) {
            auto callback = beforeScan;
            beforeScan = {};
            callback();
        }
        assert(cachedStatus != WL_IDLE_STATUS);
        radioMode = WIFI_AP_STA;
        ++scans;
        startedScanConnected = physicalConnected;
        scanState = WIFI_SCAN_RUNNING;
        return scanState;
    }
    int16_t scanComplete() { ++scanStatusReads; return scanState; }
    String SSID() const { return physicalConnected ? "Saved" : ""; }
    String SSID(int16_t) const { return "Saved"; }
    int32_t RSSI(int16_t) const { return -45; }
    uint8_t encryptionType(int16_t) const { return WIFI_AUTH_OPEN; }
    FakeIP softAPIP() const { return {0x0104a8c0}; }
    FakeIP localIP() const { return {physicalConnected ? 0x6401a8c0U : 0U}; }
    void begin(const char*, const char*) { ++begins; cachedStatus = WL_IDLE_STATUS; }
    void restored() {
        assert(radioMode == WIFI_AP_STA);
        physicalConnected = true;
        cachedStatus = WL_CONNECTED;
    }
    void lost() { physicalConnected = false; cachedStatus = WL_DISCONNECTED; }
};

extern WiFiClass WiFi;
inline int esp_wifi_scan_stop() {
    ++WiFi.scanAborts;
    WiFi.scanState = WIFI_SCAN_FAILED;
    return 0;
}

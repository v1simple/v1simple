#pragma once
#include "Arduino.h"
#include <cassert>
#include <functional>
#include <cstring>
#include <vector>

using wl_status_t = int;
enum wifi_mode_t { WIFI_OFF = 0, WIFI_STA = 1, WIFI_AP = 2, WIFI_AP_STA = 3 };
constexpr int WL_IDLE_STATUS = 0, WL_NO_SSID_AVAIL = 1, WL_CONNECTED = 3, WL_CONNECT_FAILED = 4, WL_DISCONNECTED = 6;
constexpr int WIFI_SCAN_RUNNING = -1, WIFI_SCAN_FAILED = -2, WIFI_AUTH_OPEN = 0;
using arduino_event_id_t = int;
constexpr arduino_event_id_t ARDUINO_EVENT_WIFI_STA_DISCONNECTED = 1;
constexpr uint8_t WIFI_REASON_AUTH_EXPIRE = 2, WIFI_REASON_ASSOC_LEAVE = 8,
                  WIFI_REASON_NO_AP_FOUND = 201, WIFI_REASON_AUTH_FAIL = 202, WIFI_REASON_ASSOC_FAIL = 203;
struct arduino_event_info_t {
    struct {
        uint8_t ssid[32] = {};
        uint8_t ssid_len = 0;
        uint8_t reason = 0;
    } wifi_sta_disconnected;
};
inline std::function<void(arduino_event_id_t, arduino_event_info_t)> staEventListener;

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
    String lastBeginSsid;
    std::vector<String> scanSsids = {"Saved"};
    bool staBeginSucceeds = true;
    bool staConnectSucceeds = true;
    std::function<void()> duringBegin;
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
    String SSID(int16_t index) const { return scanSsids.at(static_cast<size_t>(index)); }
    int32_t RSSI(int16_t) const { return -45; }
    uint8_t encryptionType(int16_t) const { return WIFI_AUTH_OPEN; }
    FakeIP softAPIP() const { return {0x0104a8c0}; }
    FakeIP localIP() const { return {physicalConnected ? 0x6401a8c0U : 0U}; }
    // STAClass::connect() does not clear _status; asynchronous events do.
    void begin(const char* ssid, const char*) {
        ++begins;
        lastBeginSsid = ssid;
        if (duringBegin) duringBegin();
    }
    struct StaInterface {
        bool begin();
        bool connect(const char* ssid, const char* password);
    } STA;
    int onEvent(std::function<void(arduino_event_id_t, arduino_event_info_t)> listener, arduino_event_id_t event) {
        assert(event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
        staEventListener = listener;
        return 1;
    }
    void failure(const String& ssid, wl_status_t status) {
        assert(status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL);
        physicalConnected = false;
        cachedStatus = status;
        emitDisconnected(ssid, status == WL_NO_SSID_AVAIL ? WIFI_REASON_NO_AP_FOUND : WIFI_REASON_AUTH_FAIL);
    }
    // Keep status explicit: Arduino leaves it unchanged for AUTH_EXPIRE, but
    // clears it for initial-auth retries and voluntary disconnects.
    void emitDisconnected(const String& ssid, uint8_t reason) {
        arduino_event_info_t info{};
        info.wifi_sta_disconnected.reason = reason;
        info.wifi_sta_disconnected.ssid_len = static_cast<uint8_t>(ssid.length());
        assert(ssid.length() <= sizeof(info.wifi_sta_disconnected.ssid));
        memcpy(info.wifi_sta_disconnected.ssid, ssid.c_str(), ssid.length());
        if (staEventListener) staEventListener(ARDUINO_EVENT_WIFI_STA_DISCONNECTED, info);
    }
    void restored() {
        assert(radioMode == WIFI_AP_STA);
        physicalConnected = true;
        cachedStatus = WL_CONNECTED;
    }
    void lost() { physicalConnected = false; cachedStatus = WL_DISCONNECTED; }
};

extern WiFiClass WiFi;
inline bool WiFiClass::StaInterface::begin() { return WiFi.staBeginSucceeds; }
inline bool WiFiClass::StaInterface::connect(const char* ssid, const char* password) {
    WiFi.begin(ssid, password);
    return WiFi.staConnectSucceeds;
}
inline int esp_wifi_scan_stop() {
    ++WiFi.scanAborts;
    WiFi.scanState = WIFI_SCAN_FAILED;
    return 0;
}

#pragma once

#include <Arduino.h>

#include "touch_handler.h"
#include "display.h"
#include "display_mode.h"
#include "settings.h"
#include "ble_client.h"
#include "packet_parser.h"

class AutoPushModule;
class AlertPersistenceModule;
class QuietCoordinatorModule;

class TapGestureModule {
public:
    struct WifiCallbacks {
        bool (*isWifiActive)(void* ctx);
        void* isWifiActiveCtx;
        void (*stopWifi)(void* ctx);
        void* stopWifiCtx;
        void (*requestMaintenanceBoot)(void* ctx);
        void* requestMaintenanceBootCtx;
    };

    void begin(TouchHandler* touchHandler,
               SettingsManager* settings,
               V1Display* display,
               V1BLEClient* bleClient,
               PacketParser* parser,
               AutoPushModule* autoPushModule,
               AlertPersistenceModule* alertPersistenceModule,
               DisplayMode* displayModePtr,
               QuietCoordinatorModule* quietCoordinator,
               const WifiCallbacks& wifiCbs = {});

    void process(unsigned long nowMs);

private:
    TouchHandler* touch_ = nullptr;
    SettingsManager* settings_ = nullptr;
    V1Display* display_ = nullptr;
    V1BLEClient* ble_ = nullptr;
    PacketParser* parser_ = nullptr;
    AutoPushModule* autoPush_ = nullptr;
    AlertPersistenceModule* alertPersistence_ = nullptr;
    DisplayMode* displayMode_ = nullptr;
    QuietCoordinatorModule* quiet_ = nullptr;

    unsigned long lastTapTime_ = 0;
    int tapCount_ = 0;
    static constexpr int PROFILE_CHANGE_TAP_COUNT = 3;
    static constexpr unsigned long TAP_WINDOW_MS = 600;
    static constexpr unsigned long TAP_DEBOUNCE_MS = 150;

    // Long-press maintenance entry / WiFi stop
    WifiCallbacks wifiCbs_ = {};
    unsigned long touchStartMs_ = 0;
    unsigned long nextTouchPollMs_ = 0;
    bool touching_ = false;
    bool longPressFired_ = false;
    static constexpr unsigned long LONG_PRESS_WIFI_MS = 4000;
    static constexpr unsigned long TOUCH_POLL_INTERVAL_MS = 25;
};

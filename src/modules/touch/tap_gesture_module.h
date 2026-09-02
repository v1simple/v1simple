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
    void begin(TouchHandler* touchHandler, SettingsManager* settings, V1Display* display, V1BLEClient* bleClient,
               PacketParser* parser, AutoPushModule* autoPushModule, AlertPersistenceModule* alertPersistenceModule,
               DisplayMode* displayModePtr, QuietCoordinatorModule* quietCoordinator);

    void process(unsigned long nowMs);

    // Drop pending taps and mute retries across a higher-priority presentation
    // interval so they cannot fire when normal input resumes.
    void suspendForPresentationOwner();

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

    unsigned long nextTouchPollMs_ = 0;
    bool pendingMuteCommand_ = false;
    bool pendingMuteValue_ = false;
    unsigned long pendingMuteLastAttemptMs_ = 0;
    static constexpr unsigned long MUTE_RETRY_INTERVAL_MS = 25;
    static constexpr unsigned long TOUCH_POLL_INTERVAL_MS = 25;
};

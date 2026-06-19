#include "tap_gesture_module.h"

#include "../perf/debug_macros.h"
#include "../quiet/quiet_coordinator_module.h"

#ifndef UNIT_TEST
#include "modules/alert_persistence/alert_persistence_module.h"
#include "modules/auto_push/auto_push_module.h"
#endif

void TapGestureModule::begin(TouchHandler* touchHandler,
                             SettingsManager* settings,
                             V1Display* displayPtr,
                             V1BLEClient* bleClient,
                             PacketParser* parserPtr,
                             AutoPushModule* autoPushModule,
                             AlertPersistenceModule* alertPersistenceModule,
                             DisplayMode* displayModePtr,
                             QuietCoordinatorModule* quietCoordinator,
                             const WifiCallbacks& wifiCbs) {
    touch_ = touchHandler;
    settings_ = settings;
    display_ = displayPtr;
    ble_ = bleClient;
    parser_ = parserPtr;
    autoPush_ = autoPushModule;
    alertPersistence_ = alertPersistenceModule;
    displayMode_ = displayModePtr;
    quiet_ = quietCoordinator;
    wifiCbs_ = wifiCbs;
}

void TapGestureModule::process(unsigned long nowMs) {
    if (!touch_ || !settings_ || !display_ || !ble_ || !parser_ || !autoPush_ || !alertPersistence_ || !displayMode_) {
        return;
    }

    if (static_cast<int32_t>(nowMs - nextTouchPollMs_) < 0) {
        return;
    }
    nextTouchPollMs_ = nowMs + TOUCH_POLL_INTERVAL_MS;

    int16_t touchX, touchY;

    auto performMuteToggle = [&](const char* reason) {
        const bool hasActiveAlert = parser_->hasAlerts();
        if (!hasActiveAlert) {
            DBG_PRINTLN("MUTE BLOCKED: No active alert to mute");
            return;
        }

        DisplayState state = parser_->getDisplayState();
        bool currentMuted = state.muted;
        bool newMuted = !currentMuted;

        DBG_PRINTF("Mute: %s -> Sending: %s (%s)\n",
                      currentMuted ? "MUTED" : "UNMUTED",
                      newMuted ? "MUTE_ON" : "MUTE_OFF",
                      reason);

        const bool cmdSent = quiet_ && quiet_->sendMute(QuietOwner::TapGesture, newMuted);
        DBG_PRINTF("Mute command sent: %s\n", cmdSent ? "OK" : "FAIL");
    };

    auto performProfileCycle = [&]() {
        const V1Settings& s = settings_->get();
        int newSlot = (s.activeSlot + 1) % 3;
        settings_->setActiveSlot(newSlot);
        *displayMode_ = DisplayMode::IDLE;

        alertPersistence_->clearPersistence();

        const char* slotNames[] = {"Default", "Highway", "Comfort"};
        DBG_PRINTF("PROFILE CHANGE: Switched to '%s' (slot %d)\n", slotNames[newSlot], newSlot);

        display_->drawProfileIndicator(newSlot);

        if (ble_->isConnected() && s.autoPushEnabled) {
            DBG_PRINTLN("Pushing new profile to V1...");
            const auto queueResult = autoPush_->queueSlotPush(newSlot);
            if (queueResult != AutoPushModule::QueueResult::QUEUED) {
                DBG_PRINTF("Profile push skipped: %d\n", static_cast<int>(queueResult));
            }
        }
    };

    if (touch_->getTouchPoint(touchX, touchY)) {
        const bool hasActiveAlert = parser_->hasAlerts();
        // Track long-press for maintenance entry. If WiFi is somehow already
        // active, stop it; otherwise reboot into maintenance instead of
        // starting WiFi late in the normal drive runtime.
        if (!touching_) {
            touching_ = true;
            touchStartMs_ = nowMs;
            longPressFired_ = false;
        }
        if (!longPressFired_ && wifiCbs_.isWifiActive &&
            (nowMs - touchStartMs_) >= LONG_PRESS_WIFI_MS) {
            longPressFired_ = true;
            tapCount_ = 0;
            if (wifiCbs_.isWifiActive(wifiCbs_.isWifiActiveCtx)) {
                if (wifiCbs_.stopWifi) wifiCbs_.stopWifi(wifiCbs_.stopWifiCtx);
                DBG_PRINTLN("Long-press: WiFi stopped");
            } else {
                if (wifiCbs_.requestMaintenanceBoot) {
                    wifiCbs_.requestMaintenanceBoot(wifiCbs_.requestMaintenanceBootCtx);
                }
                DBG_PRINTLN("Long-press: maintenance boot requested");
            }
            return;
        }
        if (longPressFired_) return;  // Suppress taps while held after long-press

        if (nowMs - lastTapTime_ >= TAP_DEBOUNCE_MS) {
            if (nowMs - lastTapTime_ <= TAP_WINDOW_MS) {
                tapCount_++;
            } else {
                tapCount_ = 1;
            }
            lastTapTime_ = nowMs;

            DBG_PRINTF("Tap detected: count=%d, x=%d, y=%d, hasAlert=%d\n", tapCount_, touchX, touchY, hasActiveAlert);

            if (hasActiveAlert && tapCount_ == 1) {
                performMuteToggle("immediate tap");
                tapCount_ = 0;
                return;
            }

            if (!hasActiveAlert && tapCount_ >= PROFILE_CHANGE_TAP_COUNT) {
                performProfileCycle();
                tapCount_ = 0;
            } else if (hasActiveAlert) {
                DBG_PRINTF("Processing %d tap(s) as mute toggle\n", tapCount_);
                performMuteToggle("deferred tap");
                tapCount_ = 0;
            }
        }
    } else {
        touching_ = false;
    }
}

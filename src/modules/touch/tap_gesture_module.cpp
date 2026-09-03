#include "tap_gesture_module.h"

#include "../quiet/quiet_coordinator_module.h"
#ifndef UNIT_TEST
#include "modules/alert_persistence/alert_persistence_module.h"
#include "modules/auto_push/auto_push_module.h"
#endif

void TapGestureModule::begin(TouchHandler* touchHandler, SettingsManager* settings, V1Display* displayPtr,
                             V1BLEClient* bleClient, PacketParser* parserPtr, AutoPushModule* autoPushModule,
                             AlertPersistenceModule* alertPersistenceModule, DisplayMode* displayModePtr,
                             QuietCoordinatorModule* quietCoordinator) {
    touch_ = touchHandler;
    settings_ = settings;
    display_ = displayPtr;
    ble_ = bleClient;
    parser_ = parserPtr;
    autoPush_ = autoPushModule;
    alertPersistence_ = alertPersistenceModule;
    displayMode_ = displayModePtr;
    quiet_ = quietCoordinator;
    observedAlertLifetime_ = parser_ ? parser_->alertLifetime() : 0;
}

void TapGestureModule::process(unsigned long nowMs, bool profileCycleAllowed) {
    if (!touch_ || !settings_ || !display_ || !ble_ || !parser_ || !autoPush_ || !alertPersistence_ || !displayMode_) {
        return;
    }

    const uint32_t lifetime = parser_->alertLifetime();
    if (lifetime != observedAlertLifetime_) {
        // One captured lifetime owns both pending mute and partial idle taps.
        // Check before poll throttling so even a brief episode cancels them.
        observedAlertLifetime_ = lifetime;
        tapCount_ = 0;
        pendingMuteCommand_ = false;
    }
    if (!profileCycleAllowed) {
        tapCount_ = 0;
    }

    if (static_cast<int32_t>(nowMs - nextTouchPollMs_) < 0) {
        return;
    }
    nextTouchPollMs_ = nowMs + TOUCH_POLL_INTERVAL_MS;

    if (pendingMuteCommand_) {
        // A tap-mute belongs only to the alert that caused it. Do not deliver
        // the old command after that alert clears, and stop retrying once the
        // parser already reflects the desired state.
        const bool desiredStateAlreadyCommitted = parser_->getDisplayState().muted == pendingMuteValue_;
        if (!parser_->hasAlerts() || desiredStateAlreadyCommitted || !quiet_) {
            pendingMuteCommand_ = false;
        } else if ((nowMs - pendingMuteLastAttemptMs_) >= MUTE_RETRY_INTERVAL_MS) {
            pendingMuteLastAttemptMs_ = nowMs;
            const SendResult result = quiet_->sendMuteResult(QuietOwner::TapGesture, pendingMuteValue_);
            if (result != SendResult::NOT_YET) {
                pendingMuteCommand_ = false;
            }
        }
        // Continue reading taps while a mute retry remains pending.
    }

    int16_t touchX, touchY;

    auto performMuteToggle = [&](const char* reason) {
        const bool hasActiveAlert = parser_->hasAlerts();
        if (!hasActiveAlert) {
            Serial.println("MUTE BLOCKED: No active alert to mute");
            return;
        }

        DisplayState state = parser_->getDisplayState();
        bool currentMuted = state.muted;
        bool newMuted = !currentMuted;

        Serial.printf("Mute: %s -> Sending: %s (%s)\n", currentMuted ? "MUTED" : "UNMUTED",
                   newMuted ? "MUTE_ON" : "MUTE_OFF", reason);

        const SendResult result =
            quiet_ ? quiet_->sendMuteResult(QuietOwner::TapGesture, newMuted) : SendResult::FAILED;
        if (result == SendResult::NOT_YET) {
            pendingMuteCommand_ = true;
            pendingMuteValue_ = newMuted;
            pendingMuteLastAttemptMs_ = nowMs;
        }
        Serial.printf("Mute command result: %d\n", static_cast<int>(result));
    };

    auto performProfileCycle = [&]() {
        const V1Settings& s = settings_->get();
        if (ble_->isConnected() && s.autoPushEnabled && autoPush_->isActive()) {
            return;
        }
        int newSlot = (s.activeSlot + 1) % 3;
        if (!settings_->setActiveSlot(newSlot, SettingsPersistMode::ImmediateNvsDeferredBackup).success) {
            return;
        }
        *displayMode_ = DisplayMode::IDLE;

        alertPersistence_->clearPersistence();

        const char* slotNames[] = {"Default", "Highway", "Comfort"};
        Serial.printf("PROFILE CHANGE: Switched to '%s' (slot %d)\n", slotNames[newSlot], newSlot);

        display_->drawProfileIndicator(newSlot);

        if (ble_->isConnected() && s.autoPushEnabled) {
            Serial.println("Pushing new profile to V1...");
            const auto queueResult = autoPush_->queueSlotPush(newSlot);
            if (queueResult != AutoPushModule::QueueResult::QUEUED) {
                Serial.printf("Profile push skipped: %d\n", static_cast<int>(queueResult));
            }
        }
    };

    const bool newTapEdge = touch_->getTouchPoint(touchX, touchY);

    if (newTapEdge) {
        const bool hasActiveAlert = parser_->hasAlerts();
        if (!hasActiveAlert && !profileCycleAllowed) {
            return;
        }

        if (nowMs - lastTapTime_ >= TAP_DEBOUNCE_MS) {
            if (nowMs - lastTapTime_ <= TAP_WINDOW_MS) {
                tapCount_++;
            } else {
                tapCount_ = 1;
            }
            lastTapTime_ = nowMs;

            Serial.printf("Tap detected: count=%d, x=%d, y=%d, hasAlert=%d\n", tapCount_, touchX, touchY, hasActiveAlert);

            if (hasActiveAlert && tapCount_ == 1) {
                performMuteToggle("immediate tap");
                tapCount_ = 0;
                return;
            }

            if (!hasActiveAlert && tapCount_ >= PROFILE_CHANGE_TAP_COUNT) {
                performProfileCycle();
                tapCount_ = 0;
            } else if (hasActiveAlert) {
                Serial.printf("Processing %d tap(s) as mute toggle\n", tapCount_);
                performMuteToggle("deferred tap");
                tapCount_ = 0;
            }
        }
    }
}

void TapGestureModule::suspendForPresentationOwner() {
    lastTapTime_ = 0;
    tapCount_ = 0;
    nextTouchPollMs_ = 0;
    pendingMuteCommand_ = false;
    pendingMuteValue_ = false;
    pendingMuteLastAttemptMs_ = 0;
}

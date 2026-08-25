#include "power_module.h"

void PowerModule::performShutdownRequest() {
    performShutdown();
}

void PowerModule::performShutdown() {
#ifdef CAR_MODE_PWR_SHORT
    Serial.println("[Power] Shutdown request ignored (CAR_MODE_PWR_SHORT)");
    return;
#else
    if (shutdownPreparationCallback_) {
        shutdownPreparationCallback_(shutdownPreparationContext_);
    }

    if (display_) {
        display_->showShutdown();
        delay(1000);
        // Leave panel GRAM black before the battery manager drops the latch or
        // enters deep sleep. The LCD controller can otherwise retain the
        // GOODBYE frame while its backlight is off and briefly reveal it when
        // a wake press restores power/backlight.
        display_->clear();
    }

    if (battery_) {
        Serial.printf("[Power] Shutdown handoff: source=%s\n", battery_->isOnBattery() ? "battery" : "external");
#ifdef UNIT_TEST
        if (shutdownHandoffObserverForTest_) {
            shutdownHandoffObserverForTest_(shutdownHandoffObserverContextForTest_);
        }
#endif
        const bool shutdownCompleted = battery_->powerOff();
        if (!shutdownCompleted) {
            Serial.println("[Power] ERROR: shutdown hardware tail returned; device remains awake");
            if (shutdownAbortCallback_) {
                shutdownAbortCallback_(shutdownAbortContext_);
            }
            if (display_) {
                // BatteryManager deliberately returns with the inverted
                // backlight pin HIGH/off. Flush a safe frame while it remains
                // dark. The main loop restores the authoritative owner before
                // it consumes the pending brightness restore, so retained
                // GOODBYE pixels can never be exposed.
                display_->showDisconnected();
                display_->flush();
                if (settings_) {
                    displayBrightnessRestorePending_ = true;
                }
            }

            // A failed hardware tail returned after clearing the normal owner
            // and painting GOODBYE. Release any critical-warning owner and ask
            // the main loop to restore the authoritative current presentation.
            releaseCriticalBatteryPresentation();
            requestDisplayRestore();

            // Auto-power expiry clears its timer immediately before entering
            // this handoff. If the device remains awake, retry only after a
            // complete configured interval rather than on the next loop.
            if (settings_) {
                autoPowerOffTimerStart_ = 0;
                reevaluateAutoPowerOffTimer(settings_->get(), millis());
            }
        }
    }
#endif
}

void PowerModule::begin(BatteryManager* batteryMgr, V1Display* disp, SettingsManager* settings) {
    battery_ = batteryMgr;
    display_ = disp;
    settings_ = settings;
    criticalBatteryPresentationActive_ = false;
    displayRestorePending_ = false;
    displayBrightnessRestorePending_ = false;
    criticalBatteryTime_ = 0;
}

bool PowerModule::consumeDisplayRestoreRequest() {
    const bool pending = displayRestorePending_;
    displayRestorePending_ = false;
    return pending;
}

bool PowerModule::consumeDisplayBrightnessRestoreRequest() {
    const bool pending = displayBrightnessRestorePending_;
    displayBrightnessRestorePending_ = false;
    return pending;
}

void PowerModule::requestDisplayRestore() {
    displayRestorePending_ = true;
}

void PowerModule::releaseCriticalBatteryPresentation() {
    if (!criticalBatteryPresentationActive_) {
        return;
    }
    criticalBatteryPresentationActive_ = false;
    criticalBatteryTime_ = 0;
    requestDisplayRestore();
}

void PowerModule::setShutdownPreparationCallback(ShutdownPreparationCallback callback, void* context) {
    shutdownPreparationCallback_ = callback;
    shutdownPreparationContext_ = context;
}

void PowerModule::setShutdownAbortCallback(ShutdownAbortCallback callback, void* context) {
    shutdownAbortCallback_ = callback;
    shutdownAbortContext_ = context;
}

void PowerModule::logStartupStatus() {
    if (!battery_)
        return;
    Serial.printf("[Battery] Power source: %s\n", battery_->isOnBattery() ? "BATTERY" : "USB");
    Serial.printf("[Battery] Icon display: %s\n", battery_->hasBattery() ? "YES" : "NO");
    if (battery_->hasBattery()) {
        Serial.printf("[Battery] Voltage: %dmV (%d%%)\n", battery_->getVoltageMillivolts(), battery_->getPercentage());
    }
}

void PowerModule::onV1DataReceived() {
#ifndef CAR_MODE_PWR_SHORT
    if (!autoPowerOffArmed_) {
        autoPowerOffArmed_ = true;
        Serial.println("[AutoPowerOff] Armed - V1 data received");
    }
#endif
}

void PowerModule::onV1ConnectionChange(bool connected) {
#ifdef CAR_MODE_PWR_SHORT
    (void)connected;
#else
    if (!battery_ || !settings_)
        return;

    const V1Settings& s = settings_->get();
    v1SignalPresent_ = connected;

    reevaluateAutoPowerOffTimer(s, millis());
#endif
}

void PowerModule::onAlpSignalChange(bool active) {
#ifdef CAR_MODE_PWR_SHORT
    (void)active;
#else
    if (!battery_ || !settings_)
        return;

    const V1Settings& s = settings_->get();

    if (active && !alpSignalPresent_ && !autoPowerOffArmed_) {
        autoPowerOffArmed_ = true;
        Serial.println("[AutoPowerOff] Armed - ALP heartbeat received");
    }

    alpSignalPresent_ = active;

    reevaluateAutoPowerOffTimer(s, millis());
#endif
}

void PowerModule::reevaluateAutoPowerOffTimer(const V1Settings& s, unsigned long nowMs) {
#ifdef CAR_MODE_PWR_SHORT
    (void)s;
    (void)nowMs;
#else
    if (v1SignalPresent_ || alpSignalPresent_) {
        if (autoPowerOffTimerStart_ != 0) {
            Serial.println("[AutoPowerOff] Timer cancelled - activity resumed");
            autoPowerOffTimerStart_ = 0;
        }
        return;
    }

    if (autoPowerOffArmed_ && s.autoPowerOffMinutes > 0 && autoPowerOffTimerStart_ == 0) {
        // Zero is the disabled sentinel. Preserve a full retry interval even
        // when this transition happens on the first millisecond of a host test
        // or immediately around boot.
        autoPowerOffTimerStart_ = nowMs == 0 ? 1 : nowMs;
        Serial.printf("[AutoPowerOff] Timer started: %d minutes\n", s.autoPowerOffMinutes);
    }
#endif
}

void PowerModule::process(unsigned long nowMs) {
    if (!battery_ || !display_ || !settings_)
        return;

#ifdef CAR_MODE_PWR_SHORT
    (void)nowMs;
#endif

    battery_->update();
#ifndef CAR_MODE_PWR_SHORT
    if (battery_->processPowerButton()) {
        performShutdown();
        return;
    }
#endif

    // Critical battery handling (warning + shutdown)
#ifndef CAR_MODE_PWR_SHORT
    if (battery_->criticalProtectionRequired()) {
        if (!criticalBatteryPresentationActive_) {
            Serial.println("[Battery] CRITICAL - showing low battery warning");
            display_->showLowBattery();
            criticalBatteryPresentationActive_ = true;
            criticalBatteryTime_ = nowMs;
        } else if (nowMs - criticalBatteryTime_ > 5000) {
            // Any successful sample from the warning window is fresh enough to
            // confirm the cached decision. Only force one immediate acquisition
            // when periodic polling supplied none; never wait for a battery or
            // a nonzero reading to appear.
            const bool hasFreshReading =
                battery_->hasVoltageReadingAfter(static_cast<uint32_t>(criticalBatteryTime_)) ||
                battery_->refreshVoltage();
            if (!hasFreshReading || !battery_->criticalProtectionRequired()) {
                Serial.println("[Battery] CRITICAL - fresh reading did not confirm shutdown");
                releaseCriticalBatteryPresentation();
            } else {
                Serial.println("[Battery] CRITICAL - auto shutdown to protect battery");
                performShutdownRequest();
                return;
            }
        }
    } else {
        releaseCriticalBatteryPresentation();
    }
#endif

#ifndef CAR_MODE_PWR_SHORT
    // Auto power-off is disabled in car installs. Ignition power controls the
    // device lifetime, so no signal transition may tear down a still-running unit.
    const V1Settings& s = settings_->get();
    if (autoPowerOffTimerStart_ != 0) {
        unsigned long elapsedMs = nowMs - autoPowerOffTimerStart_;
        unsigned long timeoutMs = (unsigned long)s.autoPowerOffMinutes * 60UL * 1000UL;
        if (elapsedMs >= timeoutMs) {
            Serial.printf("[AutoPowerOff] Timer expired after %d minutes - powering off\n", s.autoPowerOffMinutes);
            autoPowerOffTimerStart_ = 0;
            performShutdownRequest();
            return;
        }
    }
#endif
}

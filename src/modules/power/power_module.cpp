#include "power_module.h"

void PowerModule::performShutdownRequest() {
    performShutdown();
}

void PowerModule::performShutdown() {
#ifdef CAR_MODE_PWR_SHORT
    Serial.println("[Power] Shutdown request ignored (CAR_MODE_PWR_SHORT)");
    return;
#else
    if (lifecycle_) {
        lifecycle_->prepareForShutdown();
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
        Serial.printf("[Power] Shutdown handoff: source=%s\n", battery_source_policy::sourceName(battery_->powerSource()));
#ifdef UNIT_TEST
        if (shutdownHandoffObserverForTest_) {
            shutdownHandoffObserverForTest_(shutdownHandoffObserverContextForTest_);
        }
#endif
        const bool shutdownCompleted = battery_->powerOff();
        if (!shutdownCompleted) {
            Serial.println("[Power] ERROR: shutdown hardware tail returned; device remains awake");
            if (lifecycle_) {
                lifecycle_->resumeAfterAbortedShutdown();
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
                autoPowerOffTimerRunning_ = false;
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
    criticalBatteryWarningVisible_ = false;
    criticalBatteryWarningPreempted_ = false;
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
    criticalBatteryWarningVisible_ = false;
    criticalBatteryWarningPreempted_ = false;
    criticalBatteryTime_ = 0;
    requestDisplayRestore();
}

void PowerModule::noteCriticalBatteryWarningPreempted() {
    if (criticalBatteryPresentationActive_) {
        criticalBatteryWarningVisible_ = false;
        criticalBatteryWarningPreempted_ = true;
    }
}

void PowerModule::restoreCriticalBatteryWarning() {
    if (!criticalBatteryPresentationActive_ || criticalBatteryWarningVisible_ || !display_) {
        return;
    }
    display_->showLowBattery();
    criticalBatteryWarningVisible_ = true;
    criticalBatteryWarningPreempted_ = false;
}

void PowerModule::logStartupStatus() {
    if (!battery_)
        return;
    Serial.printf("[Battery] Power source: %s\n", battery_source_policy::sourceName(battery_->powerSource()));
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

void PowerModule::reevaluateAutoPowerOffTimer(const V1Settings& s, uint32_t nowMs) {
#ifdef CAR_MODE_PWR_SHORT
    (void)s;
    (void)nowMs;
#else
    if (s.autoPowerOffMinutes == 0 || v1SignalPresent_ || alpSignalPresent_) {
        if (autoPowerOffTimerRunning_) {
            Serial.println("[AutoPowerOff] Timer cancelled - disabled or activity resumed");
            autoPowerOffTimerRunning_ = false;
            autoPowerOffTimerStart_ = 0;
        }
        return;
    }

    if (autoPowerOffArmed_ && s.autoPowerOffMinutes > 0 && !autoPowerOffTimerRunning_) {
        autoPowerOffTimerStart_ = nowMs;
        autoPowerOffTimerRunning_ = true;
        Serial.printf("[AutoPowerOff] Timer started: %d minutes\n", s.autoPowerOffMinutes);
    }
#endif
}

void PowerModule::process() {
    if (!battery_ || !display_ || !settings_)
        return;

    battery_->update();
#ifndef CAR_MODE_PWR_SHORT
    if (battery_->processPowerButton()) {
        performShutdown();
        return;
    }
#endif

    // Critical battery handling (warning + shutdown)
#ifndef CAR_MODE_PWR_SHORT
    // Earlier runtime phases may have started the inactivity timer, and ADC
    // acquisition may advance millis(). Evaluate both timers after that work:
    // the trigger reading must not count as a later warning-window sample.
    const uint32_t nowMs = static_cast<uint32_t>(millis());
    if (battery_->criticalProtectionRequired()) {
        if (!criticalBatteryPresentationActive_) {
            Serial.println("[Battery] CRITICAL - arming low battery warning");
            criticalBatteryPresentationActive_ = true;
            criticalBatteryWarningVisible_ = false;
            criticalBatteryWarningPreempted_ = false;
            criticalBatteryTime_ = nowMs;
        } else if (static_cast<uint32_t>(nowMs - criticalBatteryTime_) > 5000) {
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
    // Deferred recovery can enable auto-off after the last presence edge.
    // Reconcile without renewing an already-running interval on every tick.
    reevaluateAutoPowerOffTimer(s, nowMs);
    if (autoPowerOffTimerRunning_) {
        const uint32_t elapsedMs = nowMs - autoPowerOffTimerStart_;
        const uint32_t timeoutMs = static_cast<uint32_t>(s.autoPowerOffMinutes) * 60000u;
        if (elapsedMs >= timeoutMs) {
            Serial.printf("[AutoPowerOff] Timer expired after %d minutes - powering off\n", s.autoPowerOffMinutes);
            autoPowerOffTimerRunning_ = false;
            autoPowerOffTimerStart_ = 0;
            performShutdownRequest();
            return;
        }
    }
#endif
}

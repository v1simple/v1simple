#include "power_module.h"

#ifndef UNIT_TEST
#include "perf_metrics.h"
#define POWER_PERF_INC(counter) PERF_INC(counter)
#define POWER_PERF_FLUSH_NOW() do { (void)perfMetricsEnqueueSnapshotNow(); } while (0)
#else
#define POWER_PERF_INC(counter) do { } while (0)
#define POWER_PERF_FLUSH_NOW() do { } while (0)
#endif

void PowerModule::performShutdownRequest() {
    performShutdown();
}

void PowerModule::performShutdown() {
    if (shutdownPreparationCallback_) {
        shutdownPreparationCallback_(shutdownPreparationContext_);
    }

    if (display_) {
        display_->showShutdown();
        delay(1000);
    }

    POWER_PERF_FLUSH_NOW();
    if (battery_) {
        const bool sdLog = settings_ && settings_->get().powerOffSdLog;
        battery_->powerOff(sdLog);
    }
}

void PowerModule::begin(BatteryManager* batteryMgr,
                        V1Display* disp,
                        SettingsManager* settings) {
    battery_ = batteryMgr;
    display_ = disp;
    settings_ = settings;
}

void PowerModule::setShutdownPreparationCallback(ShutdownPreparationCallback callback, void* context) {
    shutdownPreparationCallback_ = callback;
    shutdownPreparationContext_ = context;
}

void PowerModule::logStartupStatus() {
    if (!battery_) return;
    Serial.printf("[Battery] Power source: %s\n",
                  battery_->isOnBattery() ? "BATTERY" : "USB");
    Serial.printf("[Battery] Icon display: %s\n",
                  battery_->hasBattery() ? "YES" : "NO");
    if (battery_->hasBattery()) {
        Serial.printf("[Battery] Voltage: %dmV (%d%%)\n",
                      battery_->getVoltageMillivolts(),
                      battery_->getPercentage());
    }
}

void PowerModule::onV1DataReceived() {
    if (!autoPowerOffArmed_) {
        autoPowerOffArmed_ = true;
        POWER_PERF_INC(powerAutoPowerArmed);
        Serial.println("[AutoPowerOff] Armed - V1 data received");
    }
}

void PowerModule::onV1ConnectionChange(bool connected) {
    if (!battery_ || !settings_) return;

    const V1Settings& s = settings_->get();
    v1SignalPresent_ = connected;

    reevaluateAutoPowerOffTimer(s, millis());
}

void PowerModule::onAlpSignalChange(bool active) {
    if (!battery_ || !settings_) return;

    const V1Settings& s = settings_->get();

    if (active && !alpSignalPresent_ && !autoPowerOffArmed_) {
        autoPowerOffArmed_ = true;
        POWER_PERF_INC(powerAutoPowerArmed);
        Serial.println("[AutoPowerOff] Armed - ALP heartbeat received");
    }

    alpSignalPresent_ = active;

    reevaluateAutoPowerOffTimer(s, millis());
}

void PowerModule::reevaluateAutoPowerOffTimer(const V1Settings& s, unsigned long nowMs) {
    if (v1SignalPresent_ || alpSignalPresent_) {
        if (autoPowerOffTimerStart_ != 0) {
            Serial.println("[AutoPowerOff] Timer cancelled - activity resumed");
            autoPowerOffTimerStart_ = 0;
            POWER_PERF_INC(powerAutoPowerTimerCancel);
        }
        return;
    }

    if (autoPowerOffArmed_ && s.autoPowerOffMinutes > 0 && autoPowerOffTimerStart_ == 0) {
        autoPowerOffTimerStart_ = nowMs;
        POWER_PERF_INC(powerAutoPowerTimerStart);
        Serial.printf("[AutoPowerOff] Timer started: %d minutes\n", s.autoPowerOffMinutes);
    }
}

void PowerModule::process(unsigned long nowMs) {
    if (!battery_ || !display_ || !settings_) return;

    const V1Settings& s = settings_->get();

    battery_->update();
#ifndef CAR_MODE_PWR_SHORT
    if (battery_->processPowerButton()) {
        performShutdown();
        return;
    }
#endif

    // Critical battery handling (warning + shutdown)
#ifndef CAR_MODE_PWR_SHORT
    if (battery_->isOnBattery() && battery_->hasBattery()) {
        if (battery_->isCritical()) {
            if (!lowBatteryWarningShown_) {
                Serial.println("[Battery] CRITICAL - showing low battery warning");
                display_->showLowBattery();
                lowBatteryWarningShown_ = true;
                criticalBatteryTime_ = nowMs;
                POWER_PERF_INC(powerCriticalWarn);
            } else if (nowMs - criticalBatteryTime_ > 5000) {
                Serial.println("[Battery] CRITICAL - auto shutdown to protect battery");
                POWER_PERF_INC(powerCriticalShutdown);
                POWER_PERF_FLUSH_NOW();
                performShutdownRequest();
                return;
            }
        } else {
            lowBatteryWarningShown_ = false;
        }
    }
#endif

    // Auto power-off timer check
    if (autoPowerOffTimerStart_ != 0) {
        unsigned long elapsedMs = nowMs - autoPowerOffTimerStart_;
        unsigned long timeoutMs = (unsigned long)s.autoPowerOffMinutes * 60UL * 1000UL;
        if (elapsedMs >= timeoutMs) {
            Serial.printf("[AutoPowerOff] Timer expired after %d minutes - powering off\n", s.autoPowerOffMinutes);
            autoPowerOffTimerStart_ = 0;
            POWER_PERF_INC(powerAutoPowerTimerExpire);
            POWER_PERF_FLUSH_NOW();
            performShutdownRequest();
            return;
        }
    }
}

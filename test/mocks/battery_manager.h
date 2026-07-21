/**
 * Mock BatteryManager for native unit tests.
 * Provides test helpers to control battery state.
 */
#pragma once
#ifndef BATTERY_MANAGER_H
#define BATTERY_MANAGER_H

#include <cstdint>

#ifndef BOOT_BUTTON_GPIO
#define BOOT_BUTTON_GPIO 0
#endif

#ifndef PWR_BUTTON_GPIO
#define PWR_BUTTON_GPIO 16
#endif

class BatteryManager {
public:
    struct PwrButtonState {
        bool buttonWasPressed = false;
        uint32_t buttonPressStart = 0;
    };

    bool isOnBattery() const { return onBattery_; }
    bool hasBattery() const { return hasBattery_; }
    int getBatteryPercent() const { return percent_; }
    uint8_t getPercentage() const { return static_cast<uint8_t>(percent_); }
    uint16_t getVoltageMillivolts() const { return static_cast<uint16_t>(voltage_ * 1000.0f); }
    float getBatteryVoltage() const { return voltage_; }
    bool isCharging() const { return charging_; }
    bool isLow() const { return percent_ < 20; }
    bool isCritical() const { return criticalOverrideEnabled_ ? criticalOverrideValue_ : percent_ < 5; }
    void update() { ++updateCalls; }
    bool processPowerButton() {
        ++processPowerButtonCalls;
        return processPowerButtonResult;
    }
    bool powerOff(bool sdLogEnabled = false) {
        ++powerOffCalls;
        powerOffCalled = true;
        lastPowerOffSdLog = sdLogEnabled;
        return powerOffResult;
    }
    void enterDeepSleep(uint64_t wakeMask, bool sdLogEnabled = false, uint64_t pullupMask = 0,
                        const char* outcome = nullptr) {
        (void)outcome;
        ++deepSleepCalls;
        deepSleepCalled = true;
        lastDeepSleepWakeMask = wakeMask;
        lastDeepSleepPullupMask = pullupMask;
        lastDeepSleepSdLog = sdLogEnabled;
    }

    // Test helpers
    void setOnBattery(bool b) { onBattery_ = b; }
    void setHasBattery(bool b) { hasBattery_ = b; }
    void setBatteryPercent(int p) {
        percent_ = p;
        criticalOverrideEnabled_ = false;
    }
    void setVoltage(float v) { voltage_ = v; }
    void setCharging(bool c) { charging_ = c; }
    void setCritical(bool critical) {
        criticalOverrideEnabled_ = true;
        criticalOverrideValue_ = critical;
    }
    void reset() {
        onBattery_ = false;
        hasBattery_ = false;
        percent_ = 100;
        voltage_ = 4.2f;
        charging_ = false;
        criticalOverrideEnabled_ = false;
        criticalOverrideValue_ = false;
        updateCalls = 0;
        processPowerButtonCalls = 0;
        processPowerButtonResult = false;
        powerOffCalls = 0;
        powerOffCalled = false;
        powerOffResult = true;
        lastPowerOffSdLog = false;
        deepSleepCalls = 0;
        deepSleepCalled = false;
        lastDeepSleepWakeMask = 0;
        lastDeepSleepSdLog = false;
        lastDeepSleepPullupMask = 0;
    }

    int updateCalls = 0;
    int processPowerButtonCalls = 0;
    bool processPowerButtonResult = false;
    int powerOffCalls = 0;
    bool powerOffCalled = false;
    bool powerOffResult = true;
    bool lastPowerOffSdLog = false;
    int deepSleepCalls = 0;
    bool deepSleepCalled = false;
    uint64_t lastDeepSleepWakeMask = 0;
    bool lastDeepSleepSdLog = false;
    uint64_t lastDeepSleepPullupMask = 0;

private:
    bool onBattery_ = false;
    bool hasBattery_ = false;
    int percent_ = 100;
    float voltage_ = 4.2f;
    bool charging_ = false;
    bool criticalOverrideEnabled_ = false;
    bool criticalOverrideValue_ = false;
};

inline BatteryManager batteryManager;

#endif  // BATTERY_MANAGER_H

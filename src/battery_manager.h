#pragma once
#ifndef BATTERY_MANAGER_H
#define BATTERY_MANAGER_H

#include <Arduino.h>
#include "battery_math.h"
#include "battery_source_policy.h"
#include <Wire.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#define BATTERY_ADC_CHANNEL ADC1_CHANNEL_3 // GPIO4
#define BATTERY_ADC_GPIO 4
#define BOOT_BUTTON_GPIO 0 // BOOT button for brightness adjustment
#define PWR_BUTTON_GPIO 16 // Physical PWR switch; not a supply-presence signal
#define TCA9554_SDA_GPIO 47
#define TCA9554_SCL_GPIO 48
#define TCA9554_I2C_ADDR 0x20   // ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000
#define TCA9554_PWR_LATCH_PIN 6 // Pin 6 controls battery power latch

#define TCA9554_OUTPUT_PORT 0x01
#define TCA9554_CONFIG_PORT 0x03

// Battery voltage thresholds (mV)
#define BATTERY_FULL_MV battery_math::kFullMv
#define BATTERY_EMPTY_MV battery_math::kEmptyMv
#define BATTERY_CRITICAL_MV battery_math::kCriticalMv

class BatteryManager {
  public:
    BatteryManager();

    bool begin();

    // Documented V1/V2 boards provide no qualified supply-source input.
    battery_source_policy::Source powerSource() const { return battery_source_policy::Source::Unknown; }

    // Legacy conservative policy: Unknown also returns true. For source
    // reporting use powerSource(), never interpret this as a measurement.
    bool isOnBattery() const;

    // Voltage-based presence heuristic, including a cell attached during USB power.
    bool hasBattery() const;

    // Update cached battery readings (call in loop, voltage updates every 10s)
    void update();

    // Attempts one physical ADC read without waiting for a battery. False means
    // no fresh reading (uninitialized/failed ADC or diagnostic simulation).
    bool refreshVoltage();

    // True when at least one successful physical ADC acquisition occurred
    // after sinceMs. Comparison is rollover-safe for normal runtime windows.
    bool hasVoltageReadingAfter(uint32_t sinceMs) const;

    // Cached readings, refreshed every ten seconds by update().
    uint16_t getVoltageMillivolts() const;

    uint8_t getPercentage() const;

    bool isCritical() const;

    // Safety decision for the shutdown path. Unlike hasBattery(), this does
    // not reject a deeply discharged nonzero reading below the icon's cell
    // floor while supply is unknown or confirmed battery.
    bool criticalProtectionRequired() const;

    // Keep system powered on (call early in setup when on battery)
    bool latchPowerOn();

    // Execute the final hardware-only shutdown tail after prep completes.
    // Unknown supply uses a verified latch cut, then sleep if the rail stays
    // alive. A future qualified external/USB source
    // isolates the battery latch and enters BOOT/GPIO0-wake deep sleep because
    // firmware cannot remove the external rail. A battery fallback waits for
    // PWR/GPIO16 release and uses BOOT if PWR remains asserted. A successful
    // portable cut/sleep never returns; false means the shutdown tail aborted
    // and the CPU is still awake.
    bool powerOff();

    // Enter deep sleep with an EXT1 wake mask.
    bool enterDeepSleep(uint64_t wakeMask, uint64_t pullupMask = 0, const char* outcome = nullptr);

    bool isPowerButtonPressed();

    // Process power button (call in loop) - returns true if should power off
    bool processPowerButton();

    struct PwrButtonState {
        bool buttonWasPressed = false;
        uint32_t buttonPressStart = 0;
    };

  private:
    bool initialized_;
    uint16_t lastVoltage_;
    uint32_t buttonPressStart_;
    bool buttonWasPressed_;
    // True once the button has been seen HIGH (released) since boot.
    // Prevents a button held at power-on from triggering an immediate shutdown.
    bool buttonSeenReleasedSinceBoot_;

    // Cached battery state (updated every 10s)
    uint16_t cachedVoltage_;
    uint8_t cachedPercent_;
    uint32_t lastUpdateMs_;
    bool hasVoltageReading_;

    uint16_t simulatedVoltage_;

    bool initADC();
    bool initTCA9554();
    bool setTCA9554Pin(uint8_t pin, bool high);
    bool setTCA9554PinWithBudget(uint8_t pin, bool high, TickType_t timeoutTicks, int maxRetries);
    bool readADCMillivolts(uint16_t& voltage);

};

// Pure button-state helper for hardware-independent tests. pinLow=true means
// GPIO16 is low and the button is pressed. Returns true after a two-second hold.
bool processPowerButtonState(bool pinLow, uint32_t nowMs, BatteryManager::PwrButtonState& state);

// Shared I2C bus for TCA9554 (also used by ES8311 codec)
extern TwoWire tca9554Wire;
extern SemaphoreHandle_t tca9554WireMutex;
#endif // BATTERY_MANAGER_H

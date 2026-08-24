/**
 * Battery Manager for Waveshare ESP32-S3-Touch-LCD-3.49
 *
 * Handles:
 * - Battery voltage monitoring via ADC
 * - Power control via TCA9554 I/O expander
 * - Power button handling for battery power on/off
 */

#pragma once
#ifndef BATTERY_MANAGER_H
#define BATTERY_MANAGER_H

#include <Arduino.h>
#include "battery_math.h"
#include "battery_source_policy.h"
#include <Wire.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// Hardware Pins
#define BATTERY_ADC_CHANNEL ADC1_CHANNEL_3 // GPIO4
#define BATTERY_ADC_GPIO 4
#define BOOT_BUTTON_GPIO 0 // BOOT button for brightness adjustment
#define PWR_BUTTON_GPIO 16 // Also battery presence detection
#define TCA9554_SDA_GPIO 47
#define TCA9554_SCL_GPIO 48
#define TCA9554_I2C_ADDR 0x20   // ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000
#define TCA9554_PWR_LATCH_PIN 6 // Pin 6 controls battery power latch

// TCA9554 Registers
#define TCA9554_OUTPUT_PORT 0x01
#define TCA9554_CONFIG_PORT 0x03

// Battery voltage thresholds (mV)
#define BATTERY_FULL_MV battery_math::kFullMv
#define BATTERY_EMPTY_MV battery_math::kEmptyMv
#define BATTERY_WARNING_MV battery_math::kWarningMv
#define BATTERY_CRITICAL_MV battery_math::kCriticalMv

class BatteryManager {
  public:
    BatteryManager();

    // Initialize the battery manager (call in setup)
    bool begin();

    // Check if running on battery power
    bool isOnBattery() const;

    // Check if a battery is present (detects battery even when on USB power)
    bool hasBattery() const;

    // Update cached battery readings (call in loop, voltage updates every 10s)
    void update();

    // Force one immediate physical ADC acquisition and update the cache. This
    // never waits for a battery to appear; false means no fresh reading was
    // available (uninitialized/failed ADC or diagnostic simulation mode).
    bool refreshVoltage();

    // True when at least one successful physical ADC acquisition occurred
    // after sinceMs. Comparison is rollover-safe for normal runtime windows.
    bool hasVoltageReadingAfter(uint32_t sinceMs) const;

    // Get cached battery voltage in millivolts (updated every 10s)
    uint16_t getVoltageMillivolts() const;

    // Get cached battery percentage (0-100, updated every 10s)
    uint8_t getPercentage() const;

    // Check if battery is low (uses cached values)
    bool isLow() const;

    // Check if battery is critically low (should shutdown soon)
    bool isCritical() const;

    // Safety decision for the shutdown path. Unlike hasBattery(), this does
    // not reject a deeply discharged nonzero reading below the icon's cell
    // floor once the source classifier has resolved toward battery power.
    bool criticalProtectionRequired() const;

    // Keep system powered on (call early in setup when on battery)
    bool latchPowerOn();

    // Execute the final hardware-only shutdown tail after prep completes.
    // Battery power uses a verified latch cut; attached external/USB power
    // isolates the battery latch and enters BOOT/GPIO0-wake deep sleep because
    // firmware cannot remove the external rail. A battery fallback waits for
    // PWR/GPIO16 release and uses BOOT if PWR remains asserted. A successful
    // portable cut/sleep never returns; false means the shutdown tail aborted
    // and the CPU is still awake.
    bool powerOff();

    // Enter deep sleep with an EXT1 wake mask.
    bool enterDeepSleep(uint64_t wakeMask, uint64_t pullupMask = 0, const char* outcome = nullptr);

    // Check if power button is pressed
    bool isPowerButtonPressed();

    // Process power button (call in loop) - returns true if should power off
    bool processPowerButton();

    // State used by processPowerButtonState(). Exposed so the pure helper can
    // be tested in isolation without pulling in hardware headers.
    struct PwrButtonState {
        bool buttonWasPressed = false;
        uint32_t buttonPressStart = 0;
    };

  private:
    bool initialized_;
    // Resolved view of sourceState_.classification. Kept as a plain bool so
    // every existing caller of isOnBattery() is unaffected; the authoritative
    // three-valued classification lives in sourceState_.
    bool onBattery_;
    uint16_t lastVoltage_;
    uint32_t lastButtonPress_;
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

    // Debug simulation
    uint16_t simulatedVoltage_;

    // The hardware-independent policy owns source decisions; this class
    // supplies pin samples and the clock.
    battery_source_policy::State sourceState_;
    // Bounded serial evidence replay after a confirmed source transition.
    // This does not influence sourceState_ or any product behavior.
    battery_source_policy::EvidenceReplayState sourceEvidenceReplayState_;

    bool initADC();
    bool initTCA9554();
    bool setTCA9554Pin(uint8_t pin, bool high);
    bool setTCA9554PinWithBudget(uint8_t pin, bool high, TickType_t timeoutTicks, int maxRetries);
    bool readADCMillivolts(uint16_t& voltage);

    // Take one spaced sampling round of PWR_BUTTON_GPIO and hand it to the
    // policy. Updates onBattery_ from the policy's resolved answer.
    battery_source_policy::Result observeSourceRound(uint32_t nowMs);
};

extern BatteryManager batteryManager;

// Pure button-state helper for hardware-independent tests. pinLow=true means
// GPIO16 is low and the button is pressed. Returns true after a two-second hold.
bool processPowerButtonState(bool pinLow, uint32_t nowMs, BatteryManager::PwrButtonState& state);

// Shared I2C bus for TCA9554 (also used by ES8311 codec)
extern TwoWire tca9554Wire;
extern SemaphoreHandle_t tca9554WireMutex;
#endif // BATTERY_MANAGER_H

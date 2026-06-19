/**
 * Touch Handler for Waveshare ESP32-S3-Touch-LCD-3.49
 *
 * Hardware: AXS15231B display controller with integrated touch
 * Protocol: I2C @ 0x3B on SDA=17 / SCL=18
 *
 * Features:
 * - Single-touch support (hardware limitation)
 * - 200ms debounce to prevent rapid repeat taps
 * - Optional hardware reset support via RST pin
 * - Returns coordinates in display space
 *
 * Usage:
 *   TouchHandler touch;
 *   touch.begin(17, 18, 0x3B, -1);  // SDA, SCL, addr, RST (unused)
 *   int16_t x, y;
 *   if (touch.getTouchPoint(x, y)) {
 *     // Handle touch at (x, y)
 *   }
 */

#pragma once
#ifndef TOUCH_HANDLER_H
#define TOUCH_HANDLER_H

#include <Arduino.h>
#include <Wire.h>

// AXS15231B Touch Controller I2C address and registers
// (integrated into the display controller on Waveshare ESP32-S3-Touch-LCD-3.49)
#define AXS_TOUCH_ADDR      0x3B
#define AXS_REG_STATUS      0x01
#define AXS_REG_XPOS_HIGH   0x03
#define AXS_REG_XPOS_LOW    0x04
#define AXS_REG_YPOS_HIGH   0x05
#define AXS_REG_YPOS_LOW    0x06
#define AXS_REG_CHIP_ID     0xA3

class TouchHandler {
public:
    TouchHandler();

    // Initialize touch controller with I2C
    bool begin(int sda = 17, int scl = 18, uint8_t addr = AXS_TOUCH_ADDR, int rst = -1);

    // Check if screen is touched
    bool isTouched();

    // Get touch coordinates (returns true if valid touch detected)
    bool getTouchPoint(int16_t& x, int16_t& y);

    // Reset the touch controller
    void reset();

private:
    uint8_t i2cAddr_;
    int rstPin_;
    bool touchActive_;
    uint32_t lastTouchTime_;
    uint32_t lastReleaseTime_;      // When finger was last released
    uint32_t touchDebounceMs_;
    uint32_t releaseDebounceMs_;    // Time finger must be lifted before new tap

    // I2C stall tracking
    uint32_t i2cStallCount_ = 0;          // Transactions that returned error
    uint32_t i2cMaxUs_ = 0;               // Longest I2C transaction observed

public:
    uint32_t getI2cStallCount() const { return i2cStallCount_; }
    uint32_t getI2cMaxUs() const { return i2cMaxUs_; }
    uint32_t getI2cRecoveryCount() const { return i2cRecoveryCount_; }
    void resetI2cStats() {
        i2cStallCount_ = 0;
        i2cMaxUs_ = 0;
        i2cRecoveryCount_ = 0;
        consecutiveI2cFailures_ = 0;
    }

private:
    static constexpr uint8_t I2C_RECOVERY_THRESHOLD = 3;
    static constexpr uint32_t I2C_RECOVERY_COOLDOWN_MS = 250;
    static constexpr uint32_t I2C_RECOVERY_BACKOFF_MS = 50;
    static constexpr uint8_t I2C_RECOVERY_CLOCK_PULSES = 9;
    static constexpr unsigned int I2C_RECOVERY_PULSE_DELAY_US = 5;
    static constexpr uint32_t I2C_CLOCK_HZ = 400000;
    static constexpr uint16_t I2C_TIMEOUT_MS = 5;

    int sdaPin_ = 17;
    int sclPin_ = 18;
    // I2C communication
    void configureWireBus();
    void noteNoTouch(uint32_t now);
    void recordI2cFailure(uint32_t now, uint32_t elapsedUs);
    void recordI2cSuccess();
    void maybeRecoverI2cBus(uint32_t now);
    void recoverI2cBus(uint32_t now);
    bool isI2cPollBackoffActive(uint32_t now) const;
    uint8_t readRegister(uint8_t reg);

    uint32_t lastRecoveryMs_ = 0;
    uint32_t nextI2cPollAllowedMs_ = 0;
    uint8_t consecutiveI2cFailures_ = 0;
    uint32_t i2cRecoveryCount_ = 0;
};
#endif  // TOUCH_HANDLER_H

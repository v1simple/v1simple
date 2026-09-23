// Edge-triggered touch input for the Waveshare AXS15231B controller.

#pragma once
#ifndef TOUCH_HANDLER_H
#define TOUCH_HANDLER_H

#include <Arduino.h>
#include <Wire.h>

// AXS15231B controller integrated into the Waveshare 3.49-inch display.
#define AXS_TOUCH_ADDR 0x3B
#define AXS_REG_STATUS 0x01
#define AXS_REG_XPOS_HIGH 0x03
#define AXS_REG_XPOS_LOW 0x04
#define AXS_REG_YPOS_HIGH 0x05
#define AXS_REG_YPOS_LOW 0x06
#define AXS_REG_CHIP_ID 0xA3

class TouchHandler {
  public:
    TouchHandler();

    bool begin(int sda = 17, int scl = 18, uint8_t addr = AXS_TOUCH_ADDR, int rst = -1);

    bool isTouched();

    // Edge-triggered: returns true once per new tap; while the finger
    // stays down it returns false. Use isTouchActive() for the level state.
    bool getTouchPoint(int16_t& x, int16_t& y);

    // Only a valid latest poll can confirm a hold; read failures are not releases.
    bool isTouchActive() const { return touchReadValid_ && touchActive_; }

    // Cancel input across presentation changes until a valid release is read.
    void requireRelease() {
        releaseRequired_ = true;
        touchReadValid_ = false;
    }

    void reset();

    bool isAvailable() const { return touchAvailable_; }

  private:
    uint8_t i2cAddr_;
    int rstPin_;
    bool touchAvailable_ = false;
    bool touchActive_;
    bool touchReadValid_ = false;
    bool releaseRequired_ = false;
    uint32_t lastTouchTime_;
    uint32_t lastReleaseTime_;
    uint32_t touchDebounceMs_;
    uint32_t releaseDebounceMs_;

    static constexpr uint8_t I2C_RECOVERY_THRESHOLD = 3;
    static constexpr uint32_t I2C_RECOVERY_COOLDOWN_MS = 250;
    static constexpr uint32_t I2C_RECOVERY_BACKOFF_MS = 50;
    static constexpr uint8_t I2C_RECOVERY_CLOCK_PULSES = 9;
    static constexpr unsigned int I2C_RECOVERY_PULSE_DELAY_US = 5;
    static constexpr uint32_t I2C_CLOCK_HZ = 400000;
    static constexpr uint16_t I2C_TIMEOUT_MS = 5;

    int sdaPin_ = 17;
    int sclPin_ = 18;
    void configureWireBus();
    void noteNoTouch(uint32_t now);
    void recordI2cFailure(uint32_t now);
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
#endif // TOUCH_HANDLER_H

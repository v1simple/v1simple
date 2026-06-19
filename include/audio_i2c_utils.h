/**
 * Audio I2C helpers shared by audio runtime code and tests.
 *
 * Internal-only surface: not part of the public user/API layer.
 */

#pragma once

#include <Arduino.h>
#include <Wire.h>
#include <cstdint>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

enum class AudioI2cResult : uint8_t {
    Ok = 0,
    Busy,
    ReadFailed,
    WriteFailed,
};

const char* audioI2cResultToString(AudioI2cResult result);

class AudioI2cLockGuard {
public:
    AudioI2cLockGuard(SemaphoreHandle_t mutex, TickType_t timeoutTicks);
    ~AudioI2cLockGuard();

    bool ok() const { return locked_; }
    AudioI2cResult result() const { return locked_ ? AudioI2cResult::Ok : AudioI2cResult::Busy; }

private:
    SemaphoreHandle_t mutex_ = nullptr;
    bool locked_ = false;
};

AudioI2cResult audioI2cWriteRegister(TwoWire& wire, uint8_t deviceAddr, uint8_t reg, uint8_t value);
AudioI2cResult audioI2cReadRegister(TwoWire& wire, uint8_t deviceAddr, uint8_t reg, uint8_t& value);
AudioI2cResult audioI2cSetTca9554Pin(TwoWire& wire,
                                     uint8_t deviceAddr,
                                     uint8_t configReg,
                                     uint8_t outputReg,
                                     uint8_t pinIndex,
                                     bool high,
                                     uint8_t* nextConfig = nullptr,
                                     uint8_t* nextOutput = nullptr);


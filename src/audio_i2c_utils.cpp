#include "audio_i2c_utils.h"

namespace {

constexpr uint8_t kOneByte = 1;

}  // namespace

const char* audioI2cResultToString(AudioI2cResult result) {
    switch (result) {
        case AudioI2cResult::Ok:
            return "ok";
        case AudioI2cResult::Busy:
            return "busy";
        case AudioI2cResult::ReadFailed:
            return "read_failed";
        case AudioI2cResult::WriteFailed:
            return "write_failed";
    }
    return "unknown";
}

AudioI2cLockGuard::AudioI2cLockGuard(SemaphoreHandle_t mutex, TickType_t timeoutTicks)
    : mutex_(mutex)
    , locked_(mutex_ && xSemaphoreTake(mutex_, timeoutTicks) == pdTRUE) {}

AudioI2cLockGuard::~AudioI2cLockGuard() {
    if (locked_ && mutex_) {
        xSemaphoreGive(mutex_);
    }
}

AudioI2cResult audioI2cWriteRegister(TwoWire& wire, uint8_t deviceAddr, uint8_t reg, uint8_t value) {
    wire.beginTransmission(deviceAddr);
    wire.write(reg);
    wire.write(value);
    return wire.endTransmission() == 0 ? AudioI2cResult::Ok : AudioI2cResult::WriteFailed;
}

AudioI2cResult audioI2cReadRegister(TwoWire& wire, uint8_t deviceAddr, uint8_t reg, uint8_t& value) {
    wire.beginTransmission(deviceAddr);
    wire.write(reg);
    if (wire.endTransmission(false) != 0) {
        return AudioI2cResult::ReadFailed;
    }

    const std::size_t bytesRead = wire.requestFrom(deviceAddr, kOneByte);
    if (bytesRead < kOneByte || wire.available() < static_cast<int>(kOneByte)) {
        return AudioI2cResult::ReadFailed;
    }

    const int raw = wire.read();
    if (raw < 0) {
        return AudioI2cResult::ReadFailed;
    }

    value = static_cast<uint8_t>(raw);
    return AudioI2cResult::Ok;
}

AudioI2cResult audioI2cSetTca9554Pin(TwoWire& wire,
                                     uint8_t deviceAddr,
                                     uint8_t configReg,
                                     uint8_t outputReg,
                                     uint8_t pinIndex,
                                     bool high,
                                     uint8_t* nextConfig,
                                     uint8_t* nextOutput) {
    uint8_t config = 0;
    AudioI2cResult result = audioI2cReadRegister(wire, deviceAddr, configReg, config);
    if (result != AudioI2cResult::Ok) {
        return result;
    }

    uint8_t output = 0;
    result = audioI2cReadRegister(wire, deviceAddr, outputReg, output);
    if (result != AudioI2cResult::Ok) {
        return result;
    }

    const uint8_t pinMask = static_cast<uint8_t>(1u << pinIndex);
    if (high) {
        output |= pinMask;
    } else {
        output &= static_cast<uint8_t>(~pinMask);
    }

    result = audioI2cWriteRegister(wire, deviceAddr, outputReg, output);
    if (result != AudioI2cResult::Ok) {
        return result;
    }

    config &= static_cast<uint8_t>(~pinMask);
    result = audioI2cWriteRegister(wire, deviceAddr, configReg, config);
    if (result != AudioI2cResult::Ok) {
        return result;
    }

    if (nextConfig) {
        *nextConfig = config;
    }
    if (nextOutput) {
        *nextOutput = output;
    }
    return AudioI2cResult::Ok;
}

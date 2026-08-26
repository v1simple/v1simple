#include "waveshare_349_hardware.h"

#include <Arduino.h>
#include <Wire.h>
#include <driver/gpio.h>

#include "audio_i2c_utils.h"
#include "battery_manager.h"

namespace waveshare_349 {
namespace {

constexpr uint8_t kTca9554Address = 0x20;
constexpr uint8_t kInputPort = 0x00;
constexpr uint8_t kOutputPort = 0x01;
constexpr uint8_t kConfigPort = 0x03;

uint8_t pinMask(uint8_t pin) {
    return static_cast<uint8_t>(1u << pin);
}

void addGpioSample(LevelSamples& samples, int level) {
    if (level == HIGH) {
        samples.high++;
    } else {
        samples.low++;
    }
}

} // namespace

Hardware& hardware() {
    static Hardware instance;
    return instance;
}

void Hardware::leaveSwappedCandidatesAsInputs() {
    pinMode(kV1BacklightGpio, INPUT);
    pinMode(kV2BacklightGpio, INPUT);
    pinMode(kGpioResetOrTe, INPUT);
}

void Hardware::prepareEarlyCandidates() {
    gpio_deep_sleep_hold_dis();
    gpio_hold_dis(static_cast<gpio_num_t>(kV1BacklightGpio));
    gpio_hold_dis(static_cast<gpio_num_t>(kV2BacklightGpio));
    leaveSwappedCandidatesAsInputs();
}

bool Hardware::setExpanderOutput(uint8_t pin, bool high, bool makeOutput) {
    AudioI2cLockGuard lock(tca9554WireMutex, pdMS_TO_TICKS(50));
    if (!lock.ok()) {
        return false;
    }

    uint8_t output = 0;
    if (audioI2cReadRegister(tca9554Wire, kTca9554Address, kOutputPort, output) != AudioI2cResult::Ok) {
        return false;
    }
    output = withPinLevel(output, pin, high);
    if (audioI2cWriteRegister(tca9554Wire, kTca9554Address, kOutputPort, output) != AudioI2cResult::Ok) {
        return false;
    }

    uint8_t config = 0;
    if (audioI2cReadRegister(tca9554Wire, kTca9554Address, kConfigPort, config) != AudioI2cResult::Ok) {
        return false;
    }
    config = withPinDirection(config, pin, makeOutput);
    return audioI2cWriteRegister(tca9554Wire, kTca9554Address, kConfigPort, config) == AudioI2cResult::Ok;
}

bool Hardware::setExpanderPinLevel(uint8_t pin, bool high) {
    AudioI2cLockGuard lock(tca9554WireMutex, pdMS_TO_TICKS(50));
    if (!lock.ok()) {
        return false;
    }
    uint8_t output = 0;
    if (audioI2cReadRegister(tca9554Wire, kTca9554Address, kOutputPort, output) != AudioI2cResult::Ok) {
        return false;
    }
    output = withPinLevel(output, pin, high);
    return audioI2cWriteRegister(tca9554Wire, kTca9554Address, kOutputPort, output) == AudioI2cResult::Ok;
}

bool Hardware::detectAndConfigure() {
    const uint32_t startedAtMs = millis();
    revision_ = Revision::Unknown;
    evidence_ = RevisionEvidence{};
    expanderReady_ = false;

    // Preload the common gate LOW before making it an output. EXIO5 remains
    // input until the complementary reset/TE evidence has been accepted.
    if (!setExpanderOutput(kExioResetOrTe, true, false) ||
        !setExpanderOutput(kExioBacklightEnable, false, true)) {
        Serial.println("[HW] TCA9554 display-safe initialization failed; revision=Unknown");
        leaveSwappedCandidatesAsInputs();
        return false;
    }
    expanderReady_ = true;

    for (uint16_t i = 0; i < kDetectionSamples; ++i) {
        addGpioSample(evidence_.gpio21, digitalRead(kGpioResetOrTe));

        uint8_t input = 0;
        {
            AudioI2cLockGuard lock(tca9554WireMutex, pdMS_TO_TICKS(50));
            if (!lock.ok() ||
                audioI2cReadRegister(tca9554Wire, kTca9554Address, kInputPort, input) != AudioI2cResult::Ok) {
                evidence_.exio5.readFailures++;
            } else if ((input & pinMask(kExioResetOrTe)) != 0) {
                evidence_.exio5.high++;
            } else {
                evidence_.exio5.low++;
            }
        }
        delay(2);
    }

    revision_ = classifyRevision(evidence_, kDetectionSamples);

    if (!configureDetectedRevision()) {
        revision_ = Revision::Unknown;
        forceBacklightOff();
        leaveSwappedCandidatesAsInputs();
    }

    Serial.printf("[HW] Waveshare349 revision=%s gpio21(low=%u high=%u fail=%u) "
                  "exio5(low=%u high=%u fail=%u) backlightGpio=%d detectMs=%lu\n",
                  revisionName(revision_), evidence_.gpio21.low, evidence_.gpio21.high, evidence_.gpio21.readFailures,
                  evidence_.exio5.low, evidence_.exio5.high, evidence_.exio5.readFailures, backlightGpio(),
                  static_cast<unsigned long>(millis() - startedAtMs));
    return ready();
}

bool Hardware::configureDetectedRevision() {
    leaveSwappedCandidatesAsInputs();
    if (revision_ == Revision::V1) {
        pinMode(kV1BacklightGpio, OUTPUT);
        digitalWrite(kV1BacklightGpio, HIGH);
        return setExpanderOutput(kExioResetOrTe, true, false) &&
               setExpanderOutput(kExioBacklightEnable, false, true);
    }
    if (revision_ == Revision::V2) {
        pinMode(kV2BacklightGpio, OUTPUT);
        digitalWrite(kV2BacklightGpio, HIGH);
        return setExpanderOutput(kExioResetOrTe, true, true) &&
               setExpanderOutput(kExioBacklightEnable, false, true);
    }
    (void)setExpanderOutput(kExioResetOrTe, true, false);
    (void)setExpanderOutput(kExioBacklightEnable, false, true);
    return false;
}

int Hardware::backlightGpio() const {
    return routingFor(revision_).backlightGpio;
}

bool Hardware::resetPanel() {
    if (!ready()) {
        return false;
    }
    const ResetRoute resetRoute = routingFor(revision_).resetRoute;
    if (resetRoute == ResetRoute::Gpio21) {
        pinMode(kGpioResetOrTe, OUTPUT);
        digitalWrite(kGpioResetOrTe, HIGH);
        delay(30);
        digitalWrite(kGpioResetOrTe, LOW);
        delay(250);
        digitalWrite(kGpioResetOrTe, HIGH);
        delay(30);
        return true;
    }

    if (resetRoute != ResetRoute::Exio5 || !setExpanderPinLevel(kExioResetOrTe, true)) {
        return false;
    }
    delay(30);
    if (!setExpanderPinLevel(kExioResetOrTe, false)) {
        return false;
    }
    delay(250);
    if (!setExpanderPinLevel(kExioResetOrTe, true)) {
        return false;
    }
    delay(30);
    return true;
}

void Hardware::setBrightness(uint8_t level) {
    const int gpio = backlightGpio();
    if (!ready() || gpio < 0) {
        forceBacklightOff();
        return;
    }

    analogWrite(gpio, 255 - level);
    if (level == 0) {
        (void)setExpanderPinLevel(kExioBacklightEnable, false);
    } else {
        (void)setExpanderPinLevel(kExioBacklightEnable, true);
    }
}

void Hardware::forceBacklightOff() {
    if (expanderReady_) {
        (void)setExpanderPinLevel(kExioBacklightEnable, false);
    }
    const int gpio = backlightGpio();
    if (gpio >= 0) {
        analogWrite(gpio, 255);
        pinMode(gpio, OUTPUT);
        digitalWrite(gpio, HIGH);
    } else {
        leaveSwappedCandidatesAsInputs();
    }
}

void Hardware::holdBacklightForDeepSleep() {
    forceBacklightOff();
    const int gpio = backlightGpio();
    if (gpio >= 0) {
        gpio_hold_en(static_cast<gpio_num_t>(gpio));
        gpio_deep_sleep_hold_en();
    }
}

void Hardware::releaseBacklightSleepHold() {
    gpio_deep_sleep_hold_dis();
    gpio_hold_dis(static_cast<gpio_num_t>(kV1BacklightGpio));
    gpio_hold_dis(static_cast<gpio_num_t>(kV2BacklightGpio));
    forceBacklightOff();
}

} // namespace waveshare_349

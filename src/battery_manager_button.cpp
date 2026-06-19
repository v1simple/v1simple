/**
 * Standalone implementation of the pure button-state logic.
 * No hardware dependencies — safe to include in native unit tests.
 */
#include "battery_manager.h"
#include <Arduino.h>

bool processPowerButtonState(bool pinLow, uint32_t nowMs, BatteryManager::PwrButtonState& state) {
    if (pinLow && !state.buttonWasPressed) {
        state.buttonPressStart = nowMs;
        state.buttonWasPressed = true;
    } else if (pinLow && state.buttonWasPressed) {
        if (nowMs - state.buttonPressStart >= 2000) {
            return true;
        }
    } else if (!pinLow && state.buttonWasPressed) {
        state.buttonWasPressed = false;
    }
    return false;
}

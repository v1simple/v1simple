#pragma once

// Keep presentation suppression and the settings snapshot in one testable
// operation without restoring a stateful loop-settings module.
template <typename RunTapGesture, typename ReadEnableWifi>
bool prepareLoopSettingsForIngest(unsigned long nowMs, bool presentationSuppressed, RunTapGesture runTapGesture,
                                  ReadEnableWifi readEnableWifi) {
    if (!presentationSuppressed) {
        runTapGesture(nowMs);
    }
    return readEnableWifi();
}

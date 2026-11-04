#pragma once
#ifndef RUNTIME_PRESENTATION_SOURCE_H
#define RUNTIME_PRESENTATION_SOURCE_H
#include "settings.h"
#include "../../include/display_presentation.h"

inline DisplayStyleView makeDisplayStyleView(const SettingsManager& settings) {
    const auto& value = settings.get();
    return {value, settings.displayConfigurationRevision(), settings.getSlotAlertPersistSec(value.activeSlot),
            settings.getSlotPriorityArrowOnly(value.activeSlot)};
}
// Isolated module tests script observations at the presentation input boundary.
// The source-owner suite includes the real source before this mock can bind.
class RuntimePresentationSource {
  public:
    explicit RuntimePresentationSource(SettingsManager& settings) : settings_(settings) {}
    PresentationStatusSnapshot status{};
    mutable unsigned captures = 0;
    unsigned bleSamples = 0;
    unsigned attentionChanges = 0;
    void noteBleSample(const DisplayBleContext& value, bool receiving, uint32_t sampledAtMs) {
        ++bleSamples;
        status.ble = value;
        status.bleReceiving = receiving;
        status.bleSampledAtMs = status.bleBadgeSampledAtMs = sampledAtMs;
        status.bleProxyEnabled = value.v1Connected;
        status.bleProxyClientConnected = value.proxyConnected;
    }
    void setObdAttention(bool value) { ++attentionChanges; status.obdAttention = value; }
    PresentationStatusSnapshot capture(uint32_t) const { ++captures; return status; }
    PresentationInputs captureForPaint() const {
        const uint32_t now = static_cast<uint32_t>(millis());
        return {capture(now), makeDisplayStyleView(settings_), now};
    }
  private:
    SettingsManager& settings_;
};
#endif

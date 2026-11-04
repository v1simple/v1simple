#pragma once
#ifndef RUNTIME_PRESENTATION_SOURCE_H
#define RUNTIME_PRESENTATION_SOURCE_H

#include "display_presentation.h"

class SettingsManager;
class ObdRuntimeModule;
class AlpRuntimeModule;
class BatteryManager;
class WiFiManager;
class GpsRuntimeModule;
class QuietCoordinatorModule;

DisplayStyleView makeDisplayStyleView(const SettingsManager& settings);

// One main-task adapter shared by the exclusive drive and maintenance runtimes.
// Capture reads existing observations; it starts no producer and advances no policy.
class RuntimePresentationSource {
  public:
    explicit RuntimePresentationSource(SettingsManager& settings) : settings_(settings) {}
    void bind(const ObdRuntimeModule& obd, const AlpRuntimeModule& alp, const BatteryManager& battery,
              const WiFiManager& wifi, const GpsRuntimeModule& gps, const QuietCoordinatorModule& quiet);
    void noteBleSample(const DisplayBleContext& context, bool receiving, uint32_t sampledAtMs);
    void setObdAttention(bool attention) { obdAttention_ = attention; }
    PresentationStatusSnapshot capture(uint32_t nowMs) const;
    PresentationInputs captureForPaint() const;

  private:
    SettingsManager& settings_;
    const ObdRuntimeModule* obd_ = nullptr;
    const AlpRuntimeModule* alp_ = nullptr;
    const BatteryManager* battery_ = nullptr;
    const WiFiManager* wifi_ = nullptr;
    const GpsRuntimeModule* gps_ = nullptr;
    const QuietCoordinatorModule* quiet_ = nullptr;
    DisplayBleContext ble_{};
    uint32_t bleSampledAtMs_ = 0;
    bool bleReceiving_ = true;
    bool obdAttention_ = false;
};

#endif

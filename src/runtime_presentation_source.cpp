#include "runtime_presentation_source.h"

#include "settings.h"
#include "battery_manager.h"
#include "wifi_manager.h"
#include "modules/obd/obd_runtime_module.h"
#include "modules/alp/alp_runtime_module.h"
#include "modules/gps/gps_runtime_module.h"
#include "modules/quiet/quiet_coordinator_module.h"

DisplayStyleView makeDisplayStyleView(const SettingsManager& settings) {
    const V1Settings& effective = settings.get();
    return {effective, settings.displayConfigurationRevision(),
            settings.getSlotAlertPersistSec(effective.activeSlot),
            settings.getSlotPriorityArrowOnly(effective.activeSlot)};
}

void RuntimePresentationSource::bind(const ObdRuntimeModule& obd, const AlpRuntimeModule& alp,
                                     const BatteryManager& battery, const WiFiManager& wifi,
                                     const GpsRuntimeModule& gps, const QuietCoordinatorModule& quiet) {
    obd_ = &obd;
    alp_ = &alp;
    battery_ = &battery;
    wifi_ = &wifi;
    gps_ = &gps;
    quiet_ = &quiet;
}

void RuntimePresentationSource::noteBleSample(const DisplayBleContext& context, bool receiving,
                                              uint32_t sampledAtMs) {
    ble_ = context;
    bleReceiving_ = receiving;
    bleSampledAtMs_ = sampledAtMs;
}

PresentationStatusSnapshot RuntimePresentationSource::capture(uint32_t nowMs) const {
    PresentationStatusSnapshot result;
    result.ble = ble_;
    result.bleReceiving = bleReceiving_;
    result.bleSampledAtMs = bleSampledAtMs_;
    result.bleBadgeSampledAtMs = bleSampledAtMs_;
    result.bleProxyEnabled = ble_.v1Connected;
    result.bleProxyClientConnected = ble_.proxyConnected;
    result.obdAttention = obdAttention_;
    if (obd_) {
        const ObdRuntimeStatus obd = obd_->snapshot(nowMs);
        result.obdEnabled = obd.enabled;
        result.obdConnected = obd.connected;
        result.obdScanInProgress = obd.scanInProgress;
        result.obdManualScanPending = obd.manualScanPending;
    }
    if (alp_) {
        const AlpStatus alp = alp_->snapshot();
        result.alpState = static_cast<uint8_t>(alp.state);
        result.alpEnabled = alp.state != AlpState::OFF;
        result.alpHeartbeat = alp.lastHbByte1;
    }
    if (gps_) {
        const GpsRuntimeStatus gps = gps_->snapshot(nowMs);
        result.gpsEnabled = gps.enabled;
        result.gpsStableFix = gps.stableHasFix;
        result.gpsSatellites = gps.stableSatellites;
    }
    if (battery_) {
        result.hasBattery = battery_->hasBattery();
        result.batteryMillivolts = battery_->getVoltageMillivolts();
        result.batteryPercent = battery_->getPercentage();
    }
    if (wifi_) {
        result.wifiServiceActive = wifi_->isWifiServiceActive();
        result.wifiConnected = wifi_->isConnected();
        result.wifiGaveUp = wifi_->isReconnectGaveUp();
    }
    if (quiet_) result.speedVolZeroActive = quiet_->getPresentationState().speedVolZeroActive;
    return result;
}

PresentationInputs RuntimePresentationSource::captureForPaint() const {
    const uint32_t nowMs = static_cast<uint32_t>(millis());
    return {capture(nowMs), makeDisplayStyleView(settings_), nowMs};
}

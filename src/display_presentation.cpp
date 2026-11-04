#include "settings_types.h"
#include "display.h"
#include "display_ble_freshness.h"
#include "display_draw.h"
#include "display_palette.h"
#include "modules/alp/alp_runtime_module.h"

namespace {
uint16_t rssiColor(int rssi) {
    if (rssi == 0) return 0;
    if (rssi >= -75) return COLOR_GREEN;
    if (rssi >= -90) return COLOR_YELLOW;
    return COLOR_RED;
}
uint16_t batteryColor(uint8_t percent) {
    if (percent <= 20) return 0xF800;
    if (percent <= 40) return 0xFD20;
    return 0x07E0;
}
} // namespace

ResolvedPaintStatus V1Display::resolvePaintStatus(const DisplayPaintContext& context, const RenderFrame* frame) {
    if (frame) {
        AlpLaserEvent event = frame->alpPrimary;
        if (frame->primaryKind == RenderFramePrimaryKind::ALP_PERSISTED) event.active = false;
        setAlpLaserEvent(event);
    }
    const auto& source = context.status;
    const auto& settings = context.style.settings;
    ResolvedPaintStatus result{context};
    result.bleFresh = DisplayBleFreshness::isFresh(source.bleSampledAtMs, context.nowMs);
    result.v1Rssi = source.ble.v1Rssi;
    result.proxyRssi = source.ble.proxyRssi;
    result.v1RssiColor = rssiColor(result.v1Rssi);
    result.proxyRssiColor = rssiColor(result.proxyRssi);
    result.bleShown = source.bleProxyEnabled && !settings.hideBleIcon;
    result.bleColor = source.bleProxyClientConnected
                          ? dimColor(settings.colorBleConnected, source.bleReceiving && DisplayBleFreshness::isFresh(source.bleBadgeSampledAtMs, context.nowMs) ? 85 : 40)
                          : dimColor(settings.colorBleDisconnected, 85);
    if (lastProxyClientConnected_ && !source.bleProxyClientConnected) volZeroWarn_.reset();
    lastProxyClientConnected_ = source.bleProxyClientConnected;

    result.profileSlot = currentProfileSlot_;
    result.profileFlashActive = static_cast<uint32_t>(context.nowMs - profileChangedTime_) < HIDE_TIMEOUT_MS;
    result.profileHidden = settings.hideProfileIndicator && !result.profileFlashActive;
    switch (currentProfileSlot_ % 3) {
    case 0:
        result.profileName = settings.slot0Name.length() ? settings.slot0Name.c_str() : "DEFAULT";
        result.profileColor = settings.slot0Color;
        break;
    case 1:
        result.profileName = settings.slot1Name.length() ? settings.slot1Name.c_str() : "HIGHWAY";
        result.profileColor = settings.slot1Color;
        break;
    default:
        result.profileName = settings.slot2Name.length() ? settings.slot2Name.c_str() : "COMFORT";
        result.profileColor = settings.slot2Color;
        break;
    }

    result.obdShown = source.obdEnabled;
    result.obdConnected = result.obdShown && source.obdConnected;
    result.obdAttention = result.obdShown && !result.obdConnected &&
                          (source.obdScanInProgress || source.obdManualScanPending || source.obdAttention);
    result.obdColor = result.obdConnected ? settings.colorObd : result.obdAttention ? 0xF800 : settings.colorMuted;
    result.gpsShown = source.gpsEnabled && source.gpsStableFix;
    result.gpsSatellites = result.gpsShown ? source.gpsSatellites : 0;
    result.alpShown = source.alpEnabled;
    result.alpState = source.alpState;
    result.alpHeartbeat = source.alpHeartbeat;
    result.alpAlertActive = alpHasLaserEvent_;
    const AlpState alpState = static_cast<AlpState>(source.alpState);
    if (alpState == AlpState::OFF || alpState == AlpState::IDLE) result.alpColor = settings.colorMuted;
    else if (result.alpAlertActive) result.alpColor = settings.colorAlpAlert;
    else if (source.alpHeartbeat == 0x04) result.alpColor = settings.colorAlpLidActive;
    else if (source.alpHeartbeat == 0x03) result.alpColor = settings.colorAlpDli;
    else result.alpColor = settings.colorAlpConnected;

    result.wifiShown = !settings.hideWifiIcon && (source.wifiServiceActive || source.wifiConnected);
    result.wifiColor = source.wifiGaveUp && !source.wifiConnected ? 0xF800 : dimColor(settings.colorWiFiConnected, 85);
    result.batteryShowOnUsb = batteryShowOnUsb_;
    if (source.batteryMillivolts > 4125) result.batteryShowOnUsb = false;
    else if (source.batteryMillivolts < 4095) result.batteryShowOnUsb = true;
    result.batteryVoltageBand = source.batteryMillivolts > 4125 ? 2 : source.batteryMillivolts < 4095 ? 0 : 1;
    result.batteryPercent = source.batteryPercent;
    result.batteryShown = source.hasBattery && !settings.hideBatteryIcon && result.batteryShowOnUsb;
    result.batteryPercentMode = settings.showBatteryPercent && !settings.hideBatteryIcon && source.hasBattery;
    result.batteryFillColor = result.batteryShown ? batteryColor(source.batteryPercent) : 0x07E0;
    result.batteryTextColor = dimColor(batteryColor(source.batteryPercent));
    result.batteryOutlineColor = dimColor(PALETTE_TEXT);
    if (result.batteryShown && source.batteryPercent != 0) {
        const unsigned sections = (source.batteryPercent + 10u) / 20u;
        result.batterySections = static_cast<uint8_t>(sections > 5u ? 5u : sections);
    }

    result.hasGunLabel = source.hasGunLabel || alpFreqOverride_;
    result.gunLabel = source.hasGunLabel ? source.gunLabel : alpFreqText_;
    result.gunLabelLidActive = source.hasGunLabel ? source.gunLabelLidActive : alpLidActive_;
    return result;
}


void V1Display::setAlpLaserEvent(const AlpLaserEvent& ev) {
    const bool prevActive = alpHasLaserEvent_;
    const bool prevLidActive = alpLidActive_;
    const uint8_t prevDirection = static_cast<uint8_t>(alpLaserEvent_.direction);
    const bool prevFreqOverride = alpFreqOverride_;
    const AlpGunType prevGun = alpLaserEvent_.gun;
    // A new engagement can arrive without an inactive display frame between it
    // and the previous teardown. Retain identity only within the same session.
    const bool sameSession = ev.sessionGeneration != 0 && ev.sessionGeneration == alpLaserEvent_.sessionGeneration;
    const bool holdLiveGun = sameSession && prevActive && ev.active && ev.gun == AlpGunType::UNKNOWN &&
                             prevGun != AlpGunType::UNKNOWN;
    const AlpGunType effectiveGun = holdLiveGun ? prevGun : ev.gun;
    // Persisted tails may retain gun text without live badge/arrow colors.
    const bool nextFreqOverride = (effectiveGun != AlpGunType::UNKNOWN);
    const char* nextGunAbbr = nextFreqOverride ? alpGunAbbrev(effectiveGun) : "";
    const bool nextLidActive = nextFreqOverride ? ev.lidActive : false;
    const bool gunChanged = (prevFreqOverride != nextFreqOverride) ||
                            (nextFreqOverride && strncmp(alpFreqText_, nextGunAbbr, sizeof(alpFreqText_)) != 0);
    const bool visualChanged = (ev.active != prevActive) || (static_cast<uint8_t>(ev.direction) != prevDirection) ||
                               gunChanged || (nextLidActive != prevLidActive);

    alpLaserEvent_ = ev;
    alpLaserEvent_.gun = effectiveGun;
    alpHasLaserEvent_ = ev.active;

    if (nextFreqOverride) {
        alpFreqOverride_ = true;
        strncpy(alpFreqText_, nextGunAbbr, sizeof(alpFreqText_));
        alpFreqText_[sizeof(alpFreqText_) - 1] = '\0';
        alpLidActive_ = nextLidActive;
    } else {
        alpFreqOverride_ = false;
        alpFreqText_[0] = '\0';
        alpLidActive_ = false;
    }

    if (visualChanged) {
        elementCaches_.alp.invalidate();
        elementCaches_.arrow.invalidate();
        elementCaches_.frequency.invalidate();
    }

}

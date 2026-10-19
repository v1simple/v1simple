// Mode transitions invalidate element caches through prepareFullRedrawNoClear().
// Changed live, resting and persisted frames use full-panel transfers; cache hits skip them.

#include "display.h"
#include "display_layout.h"
#include "display_draw.h"
#include "display_dirty_flags.h"
#include "display_element_caches.h"
#include "display_palette.h"
#include "display_text.h"
#include "display_flush.h"
#include "display_flush_policy.h"
#include "display_visual_contract.h"
#include "display_vol_warn.h"
#include "modules/alp/alp_runtime_module.h"
#include "settings.h"
#include "packet_parser.h"
#if defined(DISPLAY_WAVESHARE_349)
#include "battery_manager.h"
#include "wifi_manager.h"
#include "modules/gps/gps_runtime_module.h"
#endif

#include <array>
using DisplayLayout::PRIMARY_ZONE_HEIGHT;

namespace {

#if defined(DISPLAY_WAVESHARE_349)
uint8_t batteryVoltageBand(uint16_t millivolts) {
    if (millivolts > 4125) {
        return 2;
    }
    if (millivolts < 4095) {
        return 0;
    }
    return 1;
}

bool hasTimeDrivenRestingBlink(const DisplayState& state) {
    return state.flashBits != 0 || state.bandFlashBits != 0 || state.bogeyCounterByte != state.bogeyCounterByte2 ||
           state.bogeyCounterDot != state.bogeyCounterDot2;
}

uint32_t hashProfileName(const char* name) {
    uint32_t hash = 2166136261UL;
    if (!name) {
        return hash;
    }
    while (*name) {
        hash ^= static_cast<uint8_t>(*name++);
        hash *= 16777619UL;
    }
    return hash;
}

const char* profileNameForSlot(const V1Settings& s, int slot) {
    switch (slot % 3) {
    case 0:
        return s.slot0Name.length() > 0 ? s.slot0Name.c_str() : "DEFAULT";
    case 1:
        return s.slot1Name.length() > 0 ? s.slot1Name.c_str() : "HIGHWAY";
    default:
        return s.slot2Name.length() > 0 ? s.slot2Name.c_str() : "COMFORT";
    }
}

uint16_t profileColorForSlot(const V1Settings& s, int slot) {
    switch (slot % 3) {
    case 0:
        return s.slot0Color;
    case 1:
        return s.slot1Color;
    default:
        return s.slot2Color;
    }
}
#endif

} // namespace

namespace {

AlertData alpEventToSyntheticAlert(const AlpLaserEvent& event) {
    AlertData alert;
    alert.isValid = true;
    alert.band = BAND_LASER;
    alert.frequency = 0;
    switch (event.direction) {
    case AlpLaserDirection::FRONT:
        alert.direction = DIR_FRONT;
        break;

    case AlpLaserDirection::REAR:
        alert.direction = DIR_REAR;
        break;

    case AlpLaserDirection::UNKNOWN:
    default:
        alert.direction = DIR_NONE;
        break;
    }
    alert.frontStrength = 6;
    return alert;
}

int buildV1AlertArrayFromCards(const RenderFrame& frame, std::array<AlertData, RenderFrame::MAX_CARDS>& alerts) {
    int alertCount = 0;
    for (int index = 0; index < frame.cardCount; ++index) {
        const RenderFrameCard& card = frame.cards[index];
        if (card.kind != RenderFrameCard::Kind::V1) {
            continue;
        }
        if (alertCount >= static_cast<int>(alerts.size())) {
            break;
        }
        alerts[alertCount++] = card.v1Alert;
    }
    return alertCount;
}

} // namespace

void V1Display::renderFrame(const RenderFrame& frame) {
    persistedMode_ = false;

    switch (frame.primaryKind) {
    case RenderFramePrimaryKind::NONE:
        break;

    case RenderFramePrimaryKind::IDLE:
        if (frame.stealthMode) {
            // Stealth replaces the cards without passing through their painter.
            elementCaches_.cards.invalidate();
            showStealth(frame.stealthSpeedMph, frame.stealthSpeedValid);
        } else {
            update(frame.primaryState);
        }
        break;

    case RenderFramePrimaryKind::V1_LIVE: {
        // Restore the priority row for card continuity/grace matching; omitting it
        // can turn frequency jitter into a ghost-card handoff. Keep the list non-null:
        // null means a screen transition that clears card state, not zero secondaries.
        std::array<AlertData, RenderFrame::MAX_CARDS + 1> liveAlerts{};
        int liveCount = 0;
        liveAlerts[liveCount++] = frame.v1Priority;
        for (int index = 0; index < frame.cardCount; ++index) {
            const RenderFrameCard& card = frame.cards[index];
            if (card.kind != RenderFrameCard::Kind::V1) {
                continue;
            }
            if (liveCount >= static_cast<int>(liveAlerts.size())) {
                break;
            }
            liveAlerts[liveCount++] = card.v1Alert;
        }
        update(frame.v1Priority, liveAlerts.data(), liveCount, frame.primaryState);
        break;
    }

    case RenderFramePrimaryKind::V1_PERSISTED:
        updatePersisted(frame.v1Priority, frame.primaryState);
        break;

    case RenderFramePrimaryKind::ALP_LIVE:
    case RenderFramePrimaryKind::ALP_PERSISTED: {
        // ALP frames already contain the full V1 list. Keep it non-null to preserve
        // card grace; null is the screen-transition clear signal.
        std::array<AlertData, RenderFrame::MAX_CARDS> cardAlerts{};
        const int cardCount = buildV1AlertArrayFromCards(frame, cardAlerts);
        const AlertData syntheticAlert = alpEventToSyntheticAlert(frame.alpPrimary);
        update(syntheticAlert, cardAlerts.data(), cardCount, frame.primaryState);
        break;
    }
    }
}

#if defined(DISPLAY_WAVESHARE_349)
V1Display::RestingNoOpKey V1Display::buildRestingNoOpKey(const DisplayState& state, uint32_t nowMs,
                                                         bool bleContextFresh) const {
    const V1Settings& s = settings_.get();
    const uint16_t batteryMv = battery_ ? battery_->getVoltageMillivolts() : 0;
    const GpsRuntimeStatus gpsStatus = gpsRtMod_ ? gpsRtMod_->snapshot(nowMs) : GpsRuntimeStatus{};

    RestingNoOpKey key;
    key.paletteRevision = paletteRevision_;
    key.firmwareVersion = state.v1FirmwareVersion;
    key.batteryMinuteBucket = nowMs / 60000UL;
    key.colorBogey = s.colorBogey;
    key.colorVolumeMain = s.colorVolumeMain;
    key.colorVolumeMute = s.colorVolumeMute;
    key.colorRssiV1 = s.colorRssiV1;
    key.colorRssiProxy = s.colorRssiProxy;
    key.colorFrequency = s.colorFrequency;
    key.colorArrowFront = s.colorArrowFront;
    key.colorArrowSide = s.colorArrowSide;
    key.colorArrowRear = s.colorArrowRear;
    key.colorBandL = s.colorBandL;
    key.colorBandKa = s.colorBandKa;
    key.colorBandK = s.colorBandK;
    key.colorBandX = s.colorBandX;
    key.colorBandPhoto = s.colorBandPhoto;
    for (int barIndex = 0; barIndex < SIGNAL_BAR_COLOR_COUNT; ++barIndex) {
        key.colorBars[barIndex] = s.colorBars[barIndex];
    }
    key.colorWifi = s.colorWiFiConnected;
    key.colorBleConnected = s.colorBleConnected;
    key.colorBleDisconnected = s.colorBleDisconnected;
    key.colorObd = s.colorObd;
    key.colorMuted = s.colorMuted;
    key.colorAlpConnected = s.colorAlpConnected;
    key.colorAlpDli = s.colorAlpDli;
    key.colorAlpLidActive = s.colorAlpLidActive;
    key.colorAlpAlert = s.colorAlpAlert;
    const bool rssiCanChangePixels =
        state.supportsVolume() && !s.hideVolumeIndicator && !s.hideRssiIndicator && bleContextFresh;
    key.v1Rssi = rssiCanChangePixels ? bleCtx_.v1Rssi : 0;
    key.proxyRssi = rssiCanChangePixels ? bleCtx_.proxyRssi : 0;
    key.currentProfileSlot = currentProfileSlot_;
    key.currentProfileNameHash = hashProfileName(profileNameForSlot(s, currentProfileSlot_));
    key.currentProfileColor = profileColorForSlot(s, currentProfileSlot_);
    key.activeBands = state.activeBands;
    key.signalBars = state.signalBars;
    key.flashBits = state.flashBits;
    key.bandFlashBits = state.bandFlashBits;
    key.mainVolume = state.mainVolume;
    key.muteVolume = state.muteVolume;
    key.alpStateRaw = alpStateRaw_;
    key.alpHbByte1 = alpHbByte1_;
    key.batteryPct = battery_ ? battery_->getPercentage() : 0;
    key.batteryVoltageBand = batteryVoltageBand(batteryMv);
    key.gpsShown = gpsStatus.enabled && gpsStatus.stableHasFix;
    key.gpsSats = key.gpsShown ? gpsStatus.stableSatellites : 0;
    key.arrows = state.arrows;
    key.priorityArrow = state.priorityArrow;
    key.bogeyCounterChar = state.bogeyCounterChar;
    key.bogeyCounterChar2 = state.bogeyCounterChar2;
    key.bogeyCounterDot = state.bogeyCounterDot;
    key.bogeyCounterDot2 = state.bogeyCounterDot2;
    key.hasVolumeData = state.hasVolumeData;
    key.hasV1Version = state.hasV1Version;
    key.hasKuAlert = state.hasKuAlert;
    key.bleFresh = bleContextFresh;
    key.v1Connected = bleCtx_.v1Connected;
    key.proxyConnected = bleCtx_.proxyConnected;
    key.bleProxyEnabled = bleProxyEnabled_;
    key.bleProxyClientConnected = bleProxyClientConnected_;
    key.bleReceivingData = bleReceivingData_;
    key.wifiServiceActive = wifi_ && wifi_->isWifiServiceActive();
    key.wifiConnected = wifi_ && wifi_->isConnected();
    key.wifiGaveUp = wifi_ && wifi_->isReconnectGaveUp();
    key.hasBattery = battery_ && battery_->hasBattery();
    key.showBatteryPercent = s.showBatteryPercent;
    key.hideBatteryIcon = s.hideBatteryIcon;
    key.hideVolumeIndicator = s.hideVolumeIndicator;
    key.hideRssiIndicator = s.hideRssiIndicator;
    key.hideWifiIcon = s.hideWifiIcon;
    key.hideBleIcon = s.hideBleIcon;
    key.hideProfileIndicator = s.hideProfileIndicator;
    key.freqUseBandColor = s.freqUseBandColor;
    key.profileFlashActive = (nowMs - profileChangedTime_) < HIDE_TIMEOUT_MS;
    key.obdEnabled = obdEnabled_;
    key.obdConnected = obdConnected_;
    key.obdAttention = obdAttention_;
    key.obdScanAttention = obdScanAttention_;
    key.alpEnabled = alpEnabled_;
    key.alpHasLaserEvent = alpHasLaserEvent_;
    return key;
}

bool V1Display::canSkipRestingNoOp(const RestingNoOpKey& key) const {
    if (!lastRestingNoOpKeyValid_) {
        return false;
    }
    const RestingNoOpKey& last = lastRestingNoOpKey_;
    for (int barIndex = 0; barIndex < SIGNAL_BAR_COLOR_COUNT; ++barIndex) {
        if (key.colorBars[barIndex] != last.colorBars[barIndex]) {
            return false;
        }
    }
    return key.paletteRevision == last.paletteRevision && key.firmwareVersion == last.firmwareVersion &&
           key.batteryMinuteBucket == last.batteryMinuteBucket && key.colorBogey == last.colorBogey &&
           key.colorVolumeMain == last.colorVolumeMain && key.colorVolumeMute == last.colorVolumeMute &&
           key.colorRssiV1 == last.colorRssiV1 && key.colorRssiProxy == last.colorRssiProxy &&
           key.colorFrequency == last.colorFrequency && key.colorArrowFront == last.colorArrowFront &&
           key.colorArrowSide == last.colorArrowSide && key.colorArrowRear == last.colorArrowRear &&
           key.colorBandL == last.colorBandL && key.colorBandKa == last.colorBandKa &&
           key.colorBandK == last.colorBandK && key.colorBandX == last.colorBandX &&
           key.colorBandPhoto == last.colorBandPhoto && key.colorWifi == last.colorWifi &&
           key.colorBleConnected == last.colorBleConnected && key.colorBleDisconnected == last.colorBleDisconnected &&
           key.colorObd == last.colorObd && key.colorMuted == last.colorMuted &&
           key.colorAlpConnected == last.colorAlpConnected && key.colorAlpDli == last.colorAlpDli &&
           key.colorAlpLidActive == last.colorAlpLidActive && key.colorAlpAlert == last.colorAlpAlert &&
           key.v1Rssi == last.v1Rssi && key.proxyRssi == last.proxyRssi &&
           key.currentProfileSlot == last.currentProfileSlot &&
           key.currentProfileNameHash == last.currentProfileNameHash &&
           key.currentProfileColor == last.currentProfileColor && key.activeBands == last.activeBands &&
           key.signalBars == last.signalBars && key.flashBits == last.flashBits &&
           key.bandFlashBits == last.bandFlashBits && key.mainVolume == last.mainVolume &&
           key.muteVolume == last.muteVolume && key.alpStateRaw == last.alpStateRaw &&
           key.alpHbByte1 == last.alpHbByte1 && key.batteryPct == last.batteryPct &&
           key.batteryVoltageBand == last.batteryVoltageBand && key.gpsSats == last.gpsSats &&
           key.arrows == last.arrows && key.priorityArrow == last.priorityArrow &&
           key.bogeyCounterChar == last.bogeyCounterChar && key.bogeyCounterChar2 == last.bogeyCounterChar2 &&
           key.bogeyCounterDot == last.bogeyCounterDot && key.bogeyCounterDot2 == last.bogeyCounterDot2 &&
           key.hasVolumeData == last.hasVolumeData && key.hasV1Version == last.hasV1Version &&
           key.hasKuAlert == last.hasKuAlert && key.bleFresh == last.bleFresh && key.v1Connected == last.v1Connected &&
           key.proxyConnected == last.proxyConnected && key.bleProxyEnabled == last.bleProxyEnabled &&
           key.bleProxyClientConnected == last.bleProxyClientConnected &&
           key.bleReceivingData == last.bleReceivingData && key.wifiServiceActive == last.wifiServiceActive &&
           key.wifiConnected == last.wifiConnected && key.wifiGaveUp == last.wifiGaveUp &&
           key.hasBattery == last.hasBattery && key.showBatteryPercent == last.showBatteryPercent &&
           key.hideBatteryIcon == last.hideBatteryIcon && key.hideVolumeIndicator == last.hideVolumeIndicator &&
           key.hideRssiIndicator == last.hideRssiIndicator && key.hideWifiIcon == last.hideWifiIcon &&
           key.hideBleIcon == last.hideBleIcon && key.hideProfileIndicator == last.hideProfileIndicator &&
           key.freqUseBandColor == last.freqUseBandColor && key.profileFlashActive == last.profileFlashActive &&
           key.obdEnabled == last.obdEnabled && key.obdConnected == last.obdConnected &&
           key.obdAttention == last.obdAttention && key.obdScanAttention == last.obdScanAttention &&
           key.alpEnabled == last.alpEnabled && key.alpHasLaserEvent == last.alpHasLaserEvent &&
           key.gpsShown == last.gpsShown;
}

void V1Display::rememberRestingNoOpKey(const RestingNoOpKey& key) {
    lastRestingNoOpKey_ = key;
    lastRestingNoOpKeyValid_ = true;
}

void V1Display::invalidateRestingNoOpKey() {
    lastRestingNoOpKeyValid_ = false;
}
#endif

#ifndef DISPLAY_RENDER_FRAME_ONLY

void V1Display::drawStatusStrip(const DisplayState& state, char topChar, bool topMuted, bool topDot) {
    // image2 is the same LED's blink-off image, not a second digit. Differing
    // images (including J/P and blank) alternate on the shared 96 ms cadence.
    updateBlinkPhase_();
    char bogeyChar = topChar;
    bool bogeyDot = topDot;
    const bool bogeyBlinking =
        (state.bogeyCounterByte != state.bogeyCounterByte2) || (state.bogeyCounterDot != state.bogeyCounterDot2);
    if (bogeyBlinking && !blinkPhase_) {
        bogeyChar = state.bogeyCounterChar2;
        bogeyDot = state.bogeyCounterDot2;
    }
    drawTopCounterPair(bogeyChar, topMuted, bogeyDot);
    const V1Settings& s = settings_.get();
    const bool showVolumeAndRssi = state.supportsVolume() && !s.hideVolumeIndicator;
    if (showVolumeAndRssi) {
        drawVolumeIndicator(state.mainVolume, state.muteVolume);
        drawRssiIndicator(bleCtx_.v1Rssi);
    }
    drawWiFiIndicator();
    drawBatteryIndicator();
    drawBLEProxyIndicator();
    drawObdIndicator();
    drawGpsIndicator();
    drawAlpIndicator();
    drawMuteIcon(topMuted);
    drawProfileIndicator(currentProfileSlot_);
}

void V1Display::update(const DisplayState& state) {
    persistedMode_ = false;
    const uint32_t nowMs = static_cast<uint32_t>(millis());

    const bool needsFullRedraw = currentScreen_ != ScreenMode::Resting || dirty_.resetTracking;

    // Scanning owns the display until its mode transition completes.
    if (currentScreen_ == ScreenMode::Scanning) {
        return;
    }

    // External setters may have painted since the last frame; retain their flush
    // requirement before resetting this frame's painted bounds.
    const bool hadPendingExternalDraws = !drawnRegion_.empty();
    drawnRegion_.reset();

    // Hide mute while resting: apps commonly change idle volume, which would
    // flicker the badge. A lingering mute becomes visible with the next alert.
    const bool effectiveMuted = false;

    const bool bleContextFresh = hasFreshBleContext(nowMs);
    syncTopIndicators(nowMs);

    bool showVolumeWarning = false;
    if (!bleContextFresh) {
        volZeroWarn_.reset();
    } else {
        const bool volZero = (state.mainVolume == 0 && state.hasVolumeData);
        const bool proxyConnected = bleCtx_.proxyConnected;
        showVolumeWarning = volZeroWarn_.evaluate(volZero, proxyConnected, speedVolZeroActive_);
    }

#if defined(DISPLAY_WAVESHARE_349)
    const bool volumeWarningTimeDriven = bleContextFresh && state.hasVolumeData && state.mainVolume == 0 &&
                                         !bleCtx_.proxyConnected && !speedVolZeroActive_;
    const bool allowRestingNoOpSkip = !volumeWarningTimeDriven && !hasTimeDrivenRestingBlink(state);
    RestingNoOpKey restingNoOpKey;
    if (allowRestingNoOpSkip) {
        restingNoOpKey = buildRestingNoOpKey(state, nowMs, bleContextFresh);
        if (!needsFullRedraw && !hadPendingExternalDraws && canSkipRestingNoOp(restingNoOpKey)) {
            lastState_ = state;
            return;
        }
    } else {
        invalidateRestingNoOpKey();
    }
#endif

    // Resting mode shares the multi-alert layout geometry.
    dirty_.multiAlert = true;
    multiAlertMode_ = false;

    if (needsFullRedraw) {
        drawBaseFrame();
    }

    char topChar = state.bogeyCounterChar;
    drawStatusStrip(state, topChar, effectiveMuted, state.bogeyCounterDot);

    // Resting has no alert owner. Keep the shared physical cell labelled K;
    // Ku is a live-alert identity, not a persistent replacement for that cell.
    const uint8_t restingBandMask = static_cast<uint8_t>(state.activeBands & ~BAND_KU);
    const bool bandsPainted = drawBandIndicators(restingBandMask, effectiveMuted);
    if (bandsPainted || dirty_.gpsIndicator) {
        drawGpsIndicator(); // Repaint: band FILL_RECT overlaps GPS x-range when bands change
    }

    // Volume-zero warning replaces frequency display
    if (showVolumeWarning) {
        drawVolumeZeroWarning();
    } else {
        drawFrequency(0, BAND_NONE, effectiveMuted);
    }

    drawVerticalSignalBars(state.signalBars, state.signalBars, BAND_KA, effectiveMuted);

    drawDirectionArrow(DIR_NONE, effectiveMuted, 0);

    AlertData emptyPriority;
    drawSecondaryAlertCards(nullptr, 0, emptyPriority, effectiveMuted);

    const bool paintedThisFrame = !drawnRegion_.empty();
    if (needsFullRedraw || hadPendingExternalDraws || paintedThisFrame) {
        DISPLAY_FLUSH();
    }
    drawnRegion_.reset();

    dirty_.resetTracking = false;
    currentScreen_ = ScreenMode::Resting;
    lastState_ = state;
#if defined(DISPLAY_WAVESHARE_349)
    if (allowRestingNoOpSkip) {
        rememberRestingNoOpKey(restingNoOpKey);
    }
#endif
}

void V1Display::updatePersisted(const AlertData& alert, const DisplayState& state) {
    if (!alert.isValid) {
        persistedMode_ = false;
        update(state);
        return;
    }

    persistedMode_ = true;

    // Preserve external paint before starting this frame's cache-hit decision.
    const bool hadPendingExternalDraws = !drawnRegion_.empty();
    drawnRegion_.reset();

    const bool needsFullRedraw = currentScreen_ != ScreenMode::Persisted || dirty_.resetTracking;

    dirty_.multiAlert = true;
    multiAlertMode_ = false;

    if (needsFullRedraw) {
        drawBaseFrame();
    }

    // Bogey counter shows V1's decoded display — NOT greyed, always visible
    char topChar = state.bogeyCounterChar;
    syncTopIndicators(static_cast<uint32_t>(millis()));
    drawStatusStrip(state, topChar, false, state.bogeyCounterDot);

    const bool bandsPainted = drawBandIndicators(alert.band, true);
    if (bandsPainted || dirty_.gpsIndicator) {
        drawGpsIndicator(); // Repaint: band FILL_RECT overlaps GPS x-range when bands change
    }

    // Persistence owns a specific alert row, so preserve that row's Photo
    // type. The shared P counter may have belonged to a different row.
    const bool isPhotoRadar = (alert.band == BAND_K) && (alert.photoType != 0);
    drawFrequency(alert.frequency, alert.band, true, isPhotoRadar ? alert.photoType : 0);

    drawVerticalSignalBars(0, 0, alert.band, true);

    drawDirectionArrow(alert.direction, true);

    AlertData emptyPriority;
    drawSecondaryAlertCards(nullptr, 0, emptyPriority, true);

    const bool paintedThisFrame = !drawnRegion_.empty();
    if (needsFullRedraw || hadPendingExternalDraws || paintedThisFrame) {
        DISPLAY_FLUSH();
    }
    drawnRegion_.reset();

    dirty_.resetTracking = false;
    currentScreen_ = ScreenMode::Persisted;
}

void V1Display::update(const AlertData& priority, const AlertData* allAlerts, int alertCount,
                       const DisplayState& state) {
    persistedMode_ = false;

    if (!priority.isValid || priority.band == BAND_NONE) {
        // Leave external paint pending for the next valid frame to flush.
        return;
    }

    // Prior frames consume their bounds, so non-empty bounds here are external
    // paint (e.g. setBLEProxyStatus()) that still needs a full-canvas transfer.
    const bool hadPendingExternalDraws = !drawnRegion_.empty();
    drawnRegion_.reset();

    const V1Settings& s = settings_.get();
    const bool needsFullRedraw = currentScreen_ != ScreenMode::Live || dirty_.resetTracking;

    dirty_.multiAlert = true;
    multiAlertMode_ = true;

    // Priority-only hides directions the V1 is lighting; require explicit slot opt-in.
    Direction arrowsToShow;
    if (settings_.getSlotPriorityArrowOnly(s.activeSlot)) {
        arrowsToShow = static_cast<Direction>(state.priorityArrow & state.arrows);
    } else {
        arrowsToShow = state.arrows;
    }

    char liveTopCounterChar = state.bogeyCounterChar;
    bool liveTopCounterDot = state.bogeyCounterDot;

    if (needsFullRedraw) {
        drawBaseFrame();
    }

    syncTopIndicators(static_cast<uint32_t>(millis()));
    drawStatusStrip(state, liveTopCounterChar, state.muted, liveTopCounterDot);

    // Photo identity belongs to its K row, not an unrelated priority frequency.
    // The shared P-counter fallback is valid only for a lone K row.
    const bool isPhotoRadar = (priority.band == BAND_K) &&
                              ((priority.photoType != 0) ||
                               (alertCount <= 1 && liveTopCounterChar == 'P'));
    const uint8_t priorityPhotoType = priority.photoType != 0
                                          ? priority.photoType
                                          : DisplayVisualContract::PHOTO_TYPE_UNSPECIFIED;
    drawFrequency(priority.frequency, priority.band, state.muted, isPhotoRadar ? priorityPhotoType : 0);

    // Only a Ku priority may rename the shared K cell; secondary Ku stays in its
    // card. The physical K flash bit drives the Ku label's blink.
    const uint8_t physicalBandMask = static_cast<uint8_t>(state.activeBands & ~BAND_KU);
    const uint8_t primaryBandMask = static_cast<uint8_t>(
        physicalBandMask | ((priority.band == BAND_KU) ? BAND_KU : BAND_NONE));
    const bool bandsPainted = drawBandIndicators(primaryBandMask, state.muted, state.bandFlashBits);
    if (bandsPainted || dirty_.gpsIndicator) {
        drawGpsIndicator(); // Repaint: band FILL_RECT overlaps GPS x-range when bands change
    }
    drawVerticalSignalBars(state.signalBars, state.signalBars, priority.band, state.muted);

    // The parser exposes V1's image1-vs-image2 arrow blink bits directly;
    // alert count must not synthesize additional blinks.
    const uint8_t arrowFlashBits = state.flashBits;
    drawDirectionArrow(arrowsToShow, state.muted, arrowFlashBits);

    if (needsFullRedraw) {
        dirty_.cards = true;
        elementCaches_.cards.invalidate();
    }

    drawSecondaryAlertCards(allAlerts, alertCount, priority, state.muted);

    // Regional transfers can leave counter/card changes stale on AXS15231B.
    // Transfer the full changed canvas; cache hits still skip the panel push.
    const bool nothingToFlush = displayFrameHasNothingToFlush(
        needsFullRedraw, !hadPendingExternalDraws && drawnRegion_.empty());
    if (!nothingToFlush) {
        DISPLAY_FLUSH();
    }

    // Only new external paint should remain pending on entry to the next frame.
    drawnRegion_.reset();

    dirty_.resetTracking = false;
    currentScreen_ = ScreenMode::Live;
    lastState_ = state;
}

#endif // DISPLAY_RENDER_FRAME_ONLY

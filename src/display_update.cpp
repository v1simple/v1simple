/**
 * Display update methods — three render functions, one cache layer.
 *
 * Contains update(DisplayState), update(AlertData, ...), updatePersisted.
 *
 * The element caches (V1Display::elementCaches_) are the sole caching layer. Each draw
 * function checks "did my inputs change?" and skips the draw if not. Mode
 * transitions invalidate all element caches via prepareFullRedrawNoClear().
 * Resting/persisted steady-state frames consume the same drawnRegion_ signal
 * as the live path, but only choose between full-panel flush and no flush; they
 * intentionally do not introduce partial-panel pushes.
 */

#include "display.h"
#include "display_layout.h"
#include "display_draw.h"
#include "display_dirty_flags.h"
#include "display_element_caches.h"
#include "display_palette.h"
#include "display_text.h"
#include "display_flush.h"
#include "display_flush_policy.h"
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

struct DispatchRectList {
    DrawnRegion::Rect rects[DrawnRegion::MAX_RECTS]{};
    uint8_t count = 0;
};

bool clipToFramebuffer(DrawnRegion::Rect& rect) {
    int16_t x = rect.x;
    int16_t y = rect.y;
    int16_t w = rect.w;
    int16_t h = rect.h;
    if (w <= 0 || h <= 0)
        return false;
    if (x < 0) {
        w = static_cast<int16_t>(w + x);
        x = 0;
    }
    if (y < 0) {
        h = static_cast<int16_t>(h + y);
        y = 0;
    }
    if (w <= 0 || h <= 0)
        return false;
    if (x >= SCREEN_WIDTH || y >= SCREEN_HEIGHT)
        return false;
    if (x + w > SCREEN_WIDTH) {
        w = static_cast<int16_t>(SCREEN_WIDTH - x);
    }
    if (y + h > SCREEN_HEIGHT) {
        h = static_cast<int16_t>(SCREEN_HEIGHT - y);
    }
    if (w <= 0 || h <= 0)
        return false;
    rect.x = x;
    rect.y = y;
    rect.w = w;
    rect.h = h;
    return true;
}

int16_t rectRight(const DrawnRegion::Rect& rect) {
    return static_cast<int16_t>(rect.x + rect.w);
}

int16_t rectBottom(const DrawnRegion::Rect& rect) {
    return static_cast<int16_t>(rect.y + rect.h);
}

bool rectsOverlapOrTouch(const DrawnRegion::Rect& a, const DrawnRegion::Rect& b) {
    return a.x <= rectRight(b) && rectRight(a) >= b.x && a.y <= rectBottom(b) && rectBottom(a) >= b.y;
}

DrawnRegion::Rect unionRect(const DrawnRegion::Rect& a, const DrawnRegion::Rect& b) {
    const int16_t x0 = (a.x < b.x) ? a.x : b.x;
    const int16_t y0 = (a.y < b.y) ? a.y : b.y;
    const int16_t x1 = (rectRight(a) > rectRight(b)) ? rectRight(a) : rectRight(b);
    const int16_t y1 = (rectBottom(a) > rectBottom(b)) ? rectBottom(a) : rectBottom(b);
    return DrawnRegion::Rect{
        x0,
        y0,
        static_cast<int16_t>(x1 - x0),
        static_cast<int16_t>(y1 - y0),
        static_cast<uint8_t>(a.sourceMask | b.sourceMask),
    };
}

bool addMergedRect(DispatchRectList& list, DrawnRegion::Rect rect) {
    if (!clipToFramebuffer(rect)) {
        return true;
    }

    for (uint8_t i = 0; i < list.count; ++i) {
        if (!rectsOverlapOrTouch(list.rects[i], rect)) {
            continue;
        }
        rect = unionRect(list.rects[i], rect);
        list.rects[i] = list.rects[static_cast<uint8_t>(list.count - 1)];
        --list.count;
        i = 0xFF; // restart merge scan after unsigned increment wraps to 0
    }

    if (list.count >= DrawnRegion::MAX_RECTS) {
        return false;
    }
    list.rects[list.count++] = rect;
    return true;
}

bool buildDispatchRectList(const DrawnRegion& region, DispatchRectList& list) {
    if (region.overflowed()) {
        return false;
    }
    for (uint8_t i = 0; i < region.rectCount(); ++i) {
        if (!addMergedRect(list, region.rectAt(i))) {
            return false;
        }
    }
    return true;
}

uint32_t rectListAreaPx(const DispatchRectList& list) {
    uint32_t total = 0;
    for (uint8_t i = 0; i < list.count; ++i) {
        total += list.rects[i].areaPx();
    }
    return total;
}

uint32_t rectListRowCalls(const DispatchRectList& list) {
    uint32_t total = 0;
    for (uint8_t i = 0; i < list.count; ++i) {
        total += static_cast<uint32_t>(list.rects[i].w);
    }
    return total;
}

// V1Display::flushRegion() issues one draw16bitRGBBitmap per physical row, and
// under the rotation-1 canvas physical rows == logical *width* (px per call ==
// logical height). Cost is therefore driven by row-call count, not by area.
// Fitted against displayPartialFlushWorstUs* over 23 bench replay runs:
//
//     us ~= w * (kFlushRowCallUs + h * kFlushRowPixelNs / 1000)
//
//     shape        predicted   measured
//      70 x  22         3850       3858
//     145 x  54         9425       8781
//     147 x 172        15288      15517
//     300 x 133        27300      25678
//     230 x 133        20930      45977   <-- see note
//
// The 230x133 region costs ~202 us per row call against ~88 us for 300x133 at
// the same 133 px per call. That discrepancy is not explained, so the estimate
// is low for this shape by 2.2x. The routing decision is unaffected (20930
// already exceeds a full flush) but do not read the estimate as accurate here.
constexpr uint32_t kFlushRowCallUs = 48;
constexpr uint32_t kFlushRowPixelNs = 330;

// Typical full-canvas flush with the QSPI byte-swap/DMA overlap patch active
// (scripts/patch_arduino_gfx_qspi.py). This is a MEDIAN, and the full-flush
// distribution has a long tail -- over 812 windows: median 17,732, p90 19,591,
// p99 35,474, max 47,322 us. The tail is not understood; it is unrelated to
// region shape and clusters early in a run alongside signal-bar churn.
//
// The median is the right value for a routing decision (compare expected
// costs), but do not read this constant as "a full flush takes 17.7 ms" -- one
// in a hundred takes twice that. Without the QSPI patch the median is
// ~33,200 us; raise this to match if the patch is ever removed, or partial
// flushes that would still have been cheaper get sent to the full path.
constexpr uint32_t kFullFlushUs = 17700;

uint32_t estimatedFlushRegionUs(uint32_t w, uint32_t h) {
    return w * (kFlushRowCallUs + (h * kFlushRowPixelNs) / 1000u);
}

uint32_t estimatedRectListFlushUs(const DispatchRectList& list) {
    uint32_t total = 0;
    for (uint8_t i = 0; i < list.count; ++i) {
        total += estimatedFlushRegionUs(static_cast<uint32_t>(list.rects[i].w), static_cast<uint32_t>(list.rects[i].h));
    }
    return total;
}

bool shouldUseMultiRectDispatch(const DrawnRegion& region, uint32_t partialAreaCap, bool arrowPainted,
                                DispatchRectList& list) {
    if (arrowPainted || region.rectCount() < 2) {
        return false;
    }
    if (!buildDispatchRectList(region, list)) {
        return false;
    }
    if (list.count < 2 || list.count > 6) {
        return false;
    }

    const uint32_t totalArea = rectListAreaPx(list);
    const uint32_t totalRows = rectListRowCalls(list);
    const uint32_t unionArea = region.areaPx();
    const uint32_t unionRows = static_cast<uint32_t>(region.w());
    if (totalArea == 0 || totalRows == 0 || totalArea >= partialAreaCap) {
        return false;
    }

    // A split is only worth issuing if the whole set still beats one full push.
    // The area and row heuristics below compare the split against the union
    // bbox; neither bounds it against DISPLAY_FLUSH().
    if (estimatedRectListFlushUs(list) >= kFullFlushUs) {
        return false;
    }

    // The AXS15231B partial path is row-call sensitive. Only split when the
    // item-owned windows avoid enough dead air to beat the current union bbox.
    if (unionArea >= partialAreaCap) {
        return totalArea < unionArea && totalRows <= unionRows + 32u;
    }

    const uint32_t perRectPenaltyRows = 8u * static_cast<uint32_t>(list.count - 1);
    return (totalArea * 100u < unionArea * 85u) && (totalRows + perRectPenaltyRows < unionRows);
}

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

// ============================================================================
// renderFrame — display-pipeline frame dispatch
// ============================================================================

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
            showStealth(frame.stealthSpeedMph, frame.stealthSpeedValid);
        } else {
            update(frame.primaryState);
        }
        break;

    case RenderFramePrimaryKind::V1_LIVE: {
        // The card-row alert list includes the priority alert. The composer
        // strips it from frame.cards, but drawSecondaryAlertCards() relies on
        // seeing it. Its old-priority grace admission asks whether the previous
        // priority remains in the list, and its slot refresh keeps matching
        // slots alive. A priority-stripped list makes ordinary frequency jitter
        // look like a handoff and repeatedly admits a ghost card.
        // The pointer is also never null during live frames: a null list
        // is the screen-transition "clear card state" signal, not a
        // "zero secondaries" frame — null here would bypass the grace
        // window and wipe persisted cards instantly.
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
        // ALP frames carry the full unstripped V1 alert list in
        // frame.cards (the composer passes skipPriority=nullptr on this
        // path), so no priority prepend is needed. Keep the pointer
        // non-null for the same reason as V1_LIVE: null is the
        // screen-transition clear signal and would bypass card grace.
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
    // Every segment is independently configurable, so any one of them changing
    // must force the redraw.
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

// ============================================================================
// drawStatusStrip — full status strip render
// ============================================================================

void V1Display::drawStatusStrip(const DisplayState& state, char topChar, bool topMuted, bool topDot) {
    // image2 is the blink-off companion for the same physical bogey-counter
    // LED, not a second digit. Differing images alternate at the shared 96 ms
    // cadence; matching images remain steady. This preserves junk and photo
    // glyphs across both blink phases.
    //
    // The "blinking J" (junk indicator: image1='J', image2=' ') and
    // "blinking P" (Photo radar) cases are the visible payoff: V1 itself
    // blinks them, and now so do we.
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

// ============================================================================
// update(DisplayState) — Resting display (no active alerts)
// ============================================================================

void V1Display::update(const DisplayState& state) {
    // Not in persisted mode
    persistedMode_ = false;
    const uint32_t nowMs = static_cast<uint32_t>(millis());

    const bool needsFullRedraw = currentScreen_ != ScreenMode::Resting || dirty_.resetTracking;

    // Scanning owns the display until its mode transition completes.
    if (currentScreen_ == ScreenMode::Scanning) {
        return;
    }

    // Capture any indicator draws queued by lower-level modules before this
    // frame (for example connection-state refreshes that draw into the
    // framebuffer but leave flushing to the display pipeline), then reset the
    // per-frame accumulator so this update only records pixels it actually
    // repaints. A queued external draw still forces the safe full push below.
    const bool hadPendingExternalDraws = !drawnRegion_.empty();
    drawnRegion_.reset();
    arrowVisibilityForceFullFlush_ = false;
    arrowPaintedThisFrame_ = false;

    // In resting mode, never show muted visual — apps commonly set volume to 0
    // when idle, adjusting on new alerts.
    //
    // This intentionally hides the V1's mute state while resting, so a
    // lingering mute (tap
    // gesture, app housekeeping) is invisible until the next alert arrives
    // already quiet. Accepted because no live threat exists at rest — no
    // urgency is downgraded — and a mirrored resting mute icon would flicker
    // meaninglessly in proxy mode where the app owns muting. If revisited,
    // the sketched adjustment is: mirror mute at rest only when no proxy
    // client is connected (standalone mode gets instrument state while proxy
    // mode avoids app-owned mute churn).
    const bool effectiveMuted = false;

    const bool bleContextFresh = hasFreshBleContext(nowMs);
    syncTopIndicators(nowMs);

    // Volume-zero warning state machine
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

    // B1: Ku alerts have no dedicated LED on the V1 band row — they light K.
    // OR BAND_KU into the mask so drawBandIndicators relabels K -> "Ku".
    const uint8_t bandMaskWithKu1 = static_cast<uint8_t>(state.activeBands | (state.hasKuAlert ? BAND_KU : 0));
    const bool bandsPainted = drawBandIndicators(bandMaskWithKu1, effectiveMuted);
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

    // Clear any persisted card slots
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

// ============================================================================
// updatePersisted — last alert held in dark grey
// ============================================================================

void V1Display::updatePersisted(const AlertData& alert, const DisplayState& state) {
    if (!alert.isValid) {
        persistedMode_ = false;
        update(state);
        return;
    }

    persistedMode_ = true;

    // Preserve unflushed external indicator draws while keeping this
    // persisted frame's cache-hit decision independent from stale regions left
    // by prior frames. Persisted mode still uses only full-panel or no flush.
    const bool hadPendingExternalDraws = !drawnRegion_.empty();
    drawnRegion_.reset();
    arrowVisibilityForceFullFlush_ = false;
    arrowPaintedThisFrame_ = false;

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

    // Band indicator in persisted color
    const bool bandsPainted = drawBandIndicators(alert.band, true);
    if (bandsPainted || dirty_.gpsIndicator) {
        drawGpsIndicator(); // Repaint: band FILL_RECT overlaps GPS x-range when bands change
    }

    // Frequency in persisted color
    const bool isPhotoRadar = (alert.photoType != 0) || state.hasPhotoAlert || (state.bogeyCounterChar == 'P');
    drawFrequency(alert.frequency, alert.band, true, isPhotoRadar);

    // No signal bars — draw empty
    drawVerticalSignalBars(0, 0, alert.band, true);

    // Arrows in persisted grey
    drawDirectionArrow(alert.direction, true);

    // Clear card area
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

// ============================================================================
// update(priority, allAlerts, alertCount, state) — Live alert display
// ============================================================================

void V1Display::update(const AlertData& priority, const AlertData* allAlerts, int alertCount,
                       const DisplayState& state) {
    persistedMode_ = false;

    if (!priority.isValid || priority.band == BAND_NONE) {
        // Do not clear drawnRegion_ here: a lower-level external setter may
        // have painted an indicator before this invalid live packet. Leaving
        // the region queued lets the next real display frame flush it.
        return;
    }

    // Capture any indicator draws queued by lower-level modules before this
    // live frame (for example setBLEProxyStatus(), which draws into the
    // framebuffer and intentionally leaves flushing to the display pipeline).
    // Then reset the per-frame accumulator so this update only records pixels
    // it actually repaints. The queued region is merged back before dispatch.
    //
    // The end of this function resets drawnRegion_ after consuming it; that is
    // what makes a non-empty entry region mean "external pending draw" rather
    // than "the previous live frame already flushed this area."
    DrawnRegion pendingExternalDraws = drawnRegion_;
    const bool hadPendingExternalDraws = !pendingExternalDraws.empty();
    drawnRegion_.reset();
    arrowVisibilityForceFullFlush_ = false;
    arrowPaintedThisFrame_ = false;

    const V1Settings& s = settings_.get();
    const bool needsFullRedraw = currentScreen_ != ScreenMode::Live || dirty_.resetTracking;

    // Correctness override for V1 blink frames. Diag14 proved the framebuffer
    // paints Image1/Image2 arrow phases correctly, but the panel still looked
    // steady when those changes were delivered through repeated small
    // flushRegion() windows. Prior AXS15231B partial-window attempts were
    // unstable on-device, so live arrow/band blink frames that actually changed
    // pixels use the proven full canvas push.
    //
    // Cache-hit frames still skip flushing. V1 sends display packets
    // faster than the 96 ms blink cadence, so forcing a full flush merely
    // because flashBits are present would waste SPI time and risk higher-priority
    // BLE ingest/drain without changing the visible image.
    //
    // Kept separate from needsFullRedraw so EnterLive logging / screen
    // transition recording (gated by needsFullRedraw below) only fire on
    // actual mode transitions, not every blink frame.
    const bool blinkForceFullFlush = (state.flashBits != 0) || (state.bandFlashBits != 0);


    dirty_.multiAlert = true;
    multiAlertMode_ = true;

    // Arrow display: priority arrow only if setting enabled, otherwise all V1 arrows.
    //
    // Intersecting with priorityArrow hides directions the V1 is lighting.
    // This is applied only when explicitly enabled per slot; never make it the
    // default.
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

    // Photo-radar detection: image1 is the steady displayed character. If V1
    // is showing 'P' (steady or in the on-phase of a blink), liveTopCounterChar
    // == 'P'. Under blink-pair semantics image2=='P' implies image1=='P'
    // (steady-P case), so a separate byte2 check would be redundant.
    const bool isPhotoRadar = (priority.photoType != 0) || state.hasPhotoAlert || (liveTopCounterChar == 'P');
    drawFrequency(priority.frequency, priority.band, state.muted, isPhotoRadar);

    // B1: see above — re-label K cell as "Ku" when a Ku alert is active.
    const uint8_t bandMaskWithKu2 = static_cast<uint8_t>(state.activeBands | (state.hasKuAlert ? BAND_KU : 0));
    const bool bandsPainted = drawBandIndicators(bandMaskWithKu2, state.muted, state.bandFlashBits);
    if (bandsPainted || dirty_.gpsIndicator) {
        drawGpsIndicator(); // Repaint: band FILL_RECT overlaps GPS x-range when bands change
    }
    drawVerticalSignalBars(state.signalBars, state.signalBars, priority.band, state.muted);

    // Arrow blink: V1 reports the priority-arrow blink directly via image1 vs
    // image2 in the InfDisplayData packet (image1 = currently lit, image2 =
    // steady).  packet_parser.cpp computes state.flashBits = image1 & ~image2
    // & 0xE0, so any direction V1 wants to blink is already in state.flashBits.
    // Synthesizing a flash bit here whenever alertCount > 1 would misread
    // ESP Spec 3.015 §9 and force blinks during 2-alert windows where V1
    // explicitly reports "no blink" (image1 == image2), so we use the
    // packet-reported flash bits as-is.
    const uint8_t arrowFlashBits = state.flashBits;
    drawDirectionArrow(arrowsToShow, state.muted, arrowFlashBits);

    if (needsFullRedraw) {
        // Force card redraw only when a full screen clear invalidated the card area.
        dirty_.cards = true;
        elementCaches_.cards.invalidate();
    }

    drawSecondaryAlertCards(allAlerts, alertCount, priority, state.muted);

    // Region-union partial-flush dispatch (steady-state optimization).
    //
    // Bounded-drift safety (Valentine's Law — docs/VALENTINE_PHILOSOPHY.md,
    // principle #7: the display must not lie by going stale). This is the one
    // place a performance shortcut can leave the panel showing something the
    // parser no longer believes, so the drift is bounded by construction: every
    // outcome below either pushes what was painted or provably painted nothing,
    // mode transitions force a full redraw, and the elements whose small-window
    // updates are unreliable on this panel path (blink, arrow visibility, signal
    // bars — outcome 3) are excluded from the partial route entirely rather than
    // trusted. Worst case is a single stale frame; the next annotated frame
    // repaints. Do not widen the partial route to those elements to save a flush.
    //
    // Each leaf draw function annotates its paint rect via drawnRegion_.add().
    // DrawnRegion retains both a historical union bbox and the individual
    // item rects. Six outcomes:
    //
    //   1. needsFullRedraw        → DISPLAY_FLUSH() (mode transition / reset)
    //   2. drawnRegion_.empty()   → no flush at all (every leaf cache hit)
    //   3. blink, arrow visibility, or signal-bar change → DISPLAY_FLUSH()
    //                                (small-window updates for these elements
    //                                are not reliable enough on this panel path)
    //   4. safe split rects   → flushRegion(each item rect) when the union is
    //                            mostly dead space and no arrow rect repainted
    //   5. est. union flush ≥ kFullFlushUs → DISPLAY_FLUSH() (a partial push
    //                                would cost more than the whole canvas)
    //   6. otherwise              → flushRegion(union)
    //
    // Outcome 5 compares estimated cost, not area. flushRegion() costs roughly
    // w * (48us + 0.33us*h) because it issues one row call per logical width
    // unit, so a tall narrow-in-area region can be slower than pushing all
    // 110,080 px. The 230x133 alert union measured 45.8 ms against 17.7 ms for
    // a full flush while sitting at 30,590 px -- comfortably under the old
    // 55,040 px area cap, and taken by the partial path in 20 of 22 bench runs.
    //
    // kPartialFlushAreaCap (50% of canvas = 55,040 px) still gates the
    // multi-rect split below. Every region at or above it also fails the cost
    // test, so it is now a cheaper early-out rather than the deciding rule.
    constexpr uint32_t kPartialFlushAreaCap =
        static_cast<uint32_t>(SCREEN_WIDTH) * static_cast<uint32_t>(SCREEN_HEIGHT) / 2;

    if (hadPendingExternalDraws) {
        const uint8_t pendingSources =
            static_cast<uint8_t>(pendingExternalDraws.sourceMask() | DisplayDirtyRegionSource::External);
        drawnRegion_.add(pendingExternalDraws.x(), pendingExternalDraws.y(), pendingExternalDraws.w(),
                         pendingExternalDraws.h(), pendingSources);
    }

    // Bench replay showed the parser and framebuffer accepting every bar
    // transition while the physical meter remained at the previous value.
    // The same panel path already requires full pushes for changing arrows;
    // keep signal-bar transitions off the unreliable partial-window route too.
    const bool signalBarsPainted = (drawnRegion_.sourceMask() & DisplayDirtyRegionSource::SignalBars) != 0;
    const bool smallWindowForceFullFlush =
        blinkForceFullFlush || signalBarsPainted ||
        (arrowVisibilityForceFullFlush_ && drawnRegion_.areaPx() < kPartialFlushAreaCap);
    DispatchRectList multiRectDispatch;
    const bool useMultiRectDispatch =
        !needsFullRedraw && !smallWindowForceFullFlush &&
        shouldUseMultiRectDispatch(drawnRegion_, kPartialFlushAreaCap, arrowPaintedThisFrame_, multiRectDispatch);

    const bool nothingToFlush = displayFrameHasNothingToFlush(needsFullRedraw, drawnRegion_.empty());
    if (!nothingToFlush) {
        if (needsFullRedraw) {
            DISPLAY_FLUSH();
        } else if (smallWindowForceFullFlush) {
            // Bypass partial flush only when this blink-bearing or arrow
            // visibility-changing frame painted pixels. Cache-hit blink packets
            // above still skip. Direction-set changes repaint active/resting arrow
            // states; if the previous frame was a blink-off PALETTE_BG phase, a
            // missed small-window partial flush leaves the resting glyph blank.
            DISPLAY_FLUSH();
        } else if (useMultiRectDispatch) {
            for (uint8_t i = 0; i < multiRectDispatch.count; ++i) {
                const DrawnRegion::Rect& rect = multiRectDispatch.rects[i];
                flushRegion(rect.x, rect.y, rect.w, rect.h);
            }
        } else if (estimatedFlushRegionUs(static_cast<uint32_t>(drawnRegion_.w()),
                                          static_cast<uint32_t>(drawnRegion_.h())) >= kFullFlushUs) {
            DISPLAY_FLUSH();
        } else {
            flushRegion(drawnRegion_.x(), drawnRegion_.y(), drawnRegion_.w(), drawnRegion_.h());
        }
    }

    // Consume the live-frame region after dispatch. This prevents the next
    // live frame from mistaking already-flushed paint for pending external
    // work, while still allowing external setters between frames to queue a
    // region for the next pipeline-owned flush.
    drawnRegion_.reset();
    arrowVisibilityForceFullFlush_ = false;
    arrowPaintedThisFrame_ = false;

    dirty_.resetTracking = false;
    currentScreen_ = ScreenMode::Live;
    lastAlert_ = priority;
    lastState_ = state;
}

#endif // DISPLAY_RENDER_FRAME_ONLY

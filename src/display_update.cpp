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
#include "display_log.h"
#include "display_flush.h"
#include "display_vol_warn.h"
#include "modules/display/display_correctness_trace.h"
#include "modules/alp/alp_runtime_module.h"
#include "settings.h"
#include "perf_metrics.h"
#include "packet_parser.h"
#if defined(DISPLAY_WAVESHARE_349)
#include "battery_manager.h"
#include "wifi_manager.h"
#include "main_globals.h"
#include "modules/gps/gps_runtime_module.h"
#endif

#include <array>
using DisplayLayout::PRIMARY_ZONE_HEIGHT;

namespace {

PerfDisplayRenderPath liveRenderPathForScenario() {
    const PerfDisplayRenderScenario scenario = perfGetDisplayRenderScenario();
    if (scenario == PerfDisplayRenderScenario::Restore) {
        return PerfDisplayRenderPath::Restore;
    }
    if (scenario == PerfDisplayRenderScenario::PreviewFirstFrame ||
        scenario == PerfDisplayRenderScenario::PreviewSteadyFrame) {
        return PerfDisplayRenderPath::Preview;
    }
    return PerfDisplayRenderPath::Full;
}

PerfDisplayRenderPath restingRenderPathForScenario() {
    const PerfDisplayRenderScenario scenario = perfGetDisplayRenderScenario();
    if (scenario == PerfDisplayRenderScenario::Restore) {
        return PerfDisplayRenderPath::Restore;
    }
    return PerfDisplayRenderPath::RestingFull;
}

PerfDisplayRenderPath persistedRenderPathForScenario() {
    return (perfGetDisplayRenderScenario() == PerfDisplayRenderScenario::Restore)
               ? PerfDisplayRenderPath::Restore
               : PerfDisplayRenderPath::Persisted;
}

struct DispatchRectList {
    DrawnRegion::Rect rects[DrawnRegion::MAX_RECTS]{};
    uint8_t count = 0;
};

bool clipToFramebuffer(DrawnRegion::Rect& rect) {
    int16_t x = rect.x;
    int16_t y = rect.y;
    int16_t w = rect.w;
    int16_t h = rect.h;
    if (w <= 0 || h <= 0) return false;
    if (x < 0) { w = static_cast<int16_t>(w + x); x = 0; }
    if (y < 0) { h = static_cast<int16_t>(h + y); y = 0; }
    if (w <= 0 || h <= 0) return false;
    if (x >= SCREEN_WIDTH || y >= SCREEN_HEIGHT) return false;
    if (x + w > SCREEN_WIDTH) {
        w = static_cast<int16_t>(SCREEN_WIDTH - x);
    }
    if (y + h > SCREEN_HEIGHT) {
        h = static_cast<int16_t>(SCREEN_HEIGHT - y);
    }
    if (w <= 0 || h <= 0) return false;
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
    return a.x <= rectRight(b) &&
           rectRight(a) >= b.x &&
           a.y <= rectBottom(b) &&
           rectBottom(a) >= b.y;
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
        i = 0xFF;  // restart merge scan after unsigned increment wraps to 0
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

bool shouldUseMultiRectDispatch(const DrawnRegion& region,
                                uint32_t partialAreaCap,
                                bool arrowPainted,
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

    // The AXS15231B partial path is row-call sensitive. Only split when the
    // item-owned windows avoid enough dead air to beat the current union bbox.
    if (unionArea >= partialAreaCap) {
        return totalArea < unionArea && totalRows <= unionRows + 32u;
    }

    const uint32_t perRectPenaltyRows = 8u * static_cast<uint32_t>(list.count - 1);
    return (totalArea * 100u < unionArea * 85u) &&
           (totalRows + perRectPenaltyRows < unionRows);
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
    return state.flashBits != 0 ||
           state.bandFlashBits != 0 ||
           state.bogeyCounterByte != state.bogeyCounterByte2 ||
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

}  // namespace

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

int buildV1AlertArrayFromCards(const RenderFrame& frame,
                               std::array<AlertData, RenderFrame::MAX_CARDS>& alerts) {
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

}  // namespace

void V1Display::renderFrame(const RenderFrame& frame) {
    persistedMode_ = false;

#if defined(UNIT_TEST) || \
    (defined(DISPLAY_CORRECTNESS_TRACE_ENABLED) && DISPLAY_CORRECTNESS_TRACE_ENABLED)
    displayCorrectnessTracePublish(
        buildDisplayCorrectnessTraceEvent(frame, static_cast<uint32_t>(millis())));
#endif

    switch (frame.primaryKind) {
        case RenderFramePrimaryKind::NONE:
            return;

        case RenderFramePrimaryKind::IDLE:
            if (frame.stealthMode) {
                showStealth(frame.stealthSpeedMph, frame.stealthSpeedValid);
            } else {
                update(frame.primaryState);
            }
            return;

        case RenderFramePrimaryKind::V1_LIVE: {
            // Card-row input contract (matches the legacy tree): the alert
            // list passed to update() includes the priority alert itself —
            // the composer strips it from frame.cards, but
            // drawSecondaryAlertCards() relies on seeing it. Its old-priority
            // grace admission asks "is the previous priority still in the
            // list", and its slot refresh keeps priority-matching slots
            // alive. A priority-stripped list makes ordinary frequency
            // jitter look like a priority handoff and admits a ghost card
            // that re-admits forever (stuck first card).
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
            update(frame.v1Priority, liveAlerts.data(), liveCount,
                   frame.primaryState);
            return;
        }

        case RenderFramePrimaryKind::V1_PERSISTED:
            updatePersisted(frame.v1Priority, frame.primaryState);
            return;

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
            update(syntheticAlert,
                   cardAlerts.data(),
                   cardCount,
                   frame.primaryState);
            return;
        }
    }
}

#if defined(DISPLAY_WAVESHARE_349)
V1Display::RestingNoOpKey V1Display::buildRestingNoOpKey(const DisplayState& state,
                                                          uint32_t nowMs,
                                                          bool bleContextFresh) const {
    const V1Settings& s = settingsManager.get();
    const uint16_t batteryMv = batteryManager.getVoltageMillivolts();
    const GpsRuntimeStatus gpsStatus = gpsRuntimeModule.snapshot(nowMs);

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
    key.colorBar1 = s.colorBar1;
    key.colorBar2 = s.colorBar2;
    key.colorBar3 = s.colorBar3;
    key.colorBar4 = s.colorBar4;
    key.colorBar5 = s.colorBar5;
    key.colorBar6 = s.colorBar6;
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
        state.supportsVolume() &&
        !s.hideVolumeIndicator &&
        !s.hideRssiIndicator &&
        bleContextFresh;
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
    key.batteryPct = batteryManager.getPercentage();
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
    key.wifiServiceActive = wifiManager.isWifiServiceActive();
    key.wifiConnected = wifiManager.isConnected();
    key.wifiGaveUp = wifiManager.isReconnectGaveUp();
    key.hasBattery = batteryManager.hasBattery();
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
    return key.paletteRevision == last.paletteRevision &&
           key.firmwareVersion == last.firmwareVersion &&
           key.batteryMinuteBucket == last.batteryMinuteBucket &&
           key.colorBogey == last.colorBogey &&
           key.colorVolumeMain == last.colorVolumeMain &&
           key.colorVolumeMute == last.colorVolumeMute &&
           key.colorRssiV1 == last.colorRssiV1 &&
           key.colorRssiProxy == last.colorRssiProxy &&
           key.colorFrequency == last.colorFrequency &&
           key.colorArrowFront == last.colorArrowFront &&
           key.colorArrowSide == last.colorArrowSide &&
           key.colorArrowRear == last.colorArrowRear &&
           key.colorBandL == last.colorBandL &&
           key.colorBandKa == last.colorBandKa &&
           key.colorBandK == last.colorBandK &&
           key.colorBandX == last.colorBandX &&
           key.colorBandPhoto == last.colorBandPhoto &&
           key.colorBar1 == last.colorBar1 &&
           key.colorBar2 == last.colorBar2 &&
           key.colorBar3 == last.colorBar3 &&
           key.colorBar4 == last.colorBar4 &&
           key.colorBar5 == last.colorBar5 &&
           key.colorBar6 == last.colorBar6 &&
           key.colorWifi == last.colorWifi &&
           key.colorBleConnected == last.colorBleConnected &&
           key.colorBleDisconnected == last.colorBleDisconnected &&
           key.colorObd == last.colorObd &&
           key.colorMuted == last.colorMuted &&
           key.colorAlpConnected == last.colorAlpConnected &&
           key.colorAlpDli == last.colorAlpDli &&
           key.colorAlpLidActive == last.colorAlpLidActive &&
           key.colorAlpAlert == last.colorAlpAlert &&
           key.v1Rssi == last.v1Rssi &&
           key.proxyRssi == last.proxyRssi &&
           key.currentProfileSlot == last.currentProfileSlot &&
           key.currentProfileNameHash == last.currentProfileNameHash &&
           key.currentProfileColor == last.currentProfileColor &&
           key.activeBands == last.activeBands &&
           key.signalBars == last.signalBars &&
           key.flashBits == last.flashBits &&
           key.bandFlashBits == last.bandFlashBits &&
           key.mainVolume == last.mainVolume &&
           key.muteVolume == last.muteVolume &&
           key.alpStateRaw == last.alpStateRaw &&
           key.alpHbByte1 == last.alpHbByte1 &&
           key.batteryPct == last.batteryPct &&
           key.batteryVoltageBand == last.batteryVoltageBand &&
           key.gpsSats == last.gpsSats &&
           key.arrows == last.arrows &&
           key.priorityArrow == last.priorityArrow &&
           key.bogeyCounterChar == last.bogeyCounterChar &&
           key.bogeyCounterChar2 == last.bogeyCounterChar2 &&
           key.bogeyCounterDot == last.bogeyCounterDot &&
           key.bogeyCounterDot2 == last.bogeyCounterDot2 &&
           key.hasVolumeData == last.hasVolumeData &&
           key.hasV1Version == last.hasV1Version &&
           key.hasKuAlert == last.hasKuAlert &&
           key.bleFresh == last.bleFresh &&
           key.v1Connected == last.v1Connected &&
           key.proxyConnected == last.proxyConnected &&
           key.bleProxyEnabled == last.bleProxyEnabled &&
           key.bleProxyClientConnected == last.bleProxyClientConnected &&
           key.bleReceivingData == last.bleReceivingData &&
           key.wifiServiceActive == last.wifiServiceActive &&
           key.wifiConnected == last.wifiConnected &&
           key.wifiGaveUp == last.wifiGaveUp &&
           key.hasBattery == last.hasBattery &&
           key.showBatteryPercent == last.showBatteryPercent &&
           key.hideBatteryIcon == last.hideBatteryIcon &&
           key.hideVolumeIndicator == last.hideVolumeIndicator &&
           key.hideRssiIndicator == last.hideRssiIndicator &&
           key.hideWifiIcon == last.hideWifiIcon &&
           key.hideBleIcon == last.hideBleIcon &&
           key.hideProfileIndicator == last.hideProfileIndicator &&
           key.freqUseBandColor == last.freqUseBandColor &&
           key.profileFlashActive == last.profileFlashActive &&
           key.obdEnabled == last.obdEnabled &&
           key.obdConnected == last.obdConnected &&
           key.obdAttention == last.obdAttention &&
           key.obdScanAttention == last.obdScanAttention &&
           key.alpEnabled == last.alpEnabled &&
           key.alpHasLaserEvent == last.alpHasLaserEvent &&
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

void V1Display::drawStatusStrip(const DisplayState& state,
                                char topChar,
                                bool topMuted,
                                bool topDot) {
    // Single-LED bogey counter blink:
    // docs/V1_PROTOCOL_REFERENCES.md#infdisplaydata.
    // Image 2 is the steady/blink-off mask companion to Image 1; toggling the
    // two bytes at the shared blink phase matches the V1 LCD semantics.
    //
    // image2 (bogeyCounterByte2/Char2/Dot2) is the blink-off pair of image1
    // for the SAME single 7-segment LED — NOT a second physical digit.
    // FSD-002: treating image2 as a second digit reversed V1's visible
    // junk/photo verdicts whenever the single LED blinked. When
    // the two bytes differ, the LED is blinking on V1's hardware: V1
    // physically alternates the two images at 10.416 Hz. We reproduce this
    // character-side — render image1's char during the on phase, image2's
    // char during the off phase. When the two bytes match, no blinking and
    // the same char renders both phases.
    //
    // The "blinking J" (junk indicator: image1='J', image2=' ') and
    // "blinking P" (Photo radar) cases are the visible payoff: V1 itself
    // blinks them, and now so do we.
    updateBlinkPhase_();
    char bogeyChar = topChar;
    bool bogeyDot  = topDot;
    const bool bogeyBlinking =
        (state.bogeyCounterByte != state.bogeyCounterByte2) ||
        (state.bogeyCounterDot  != state.bogeyCounterDot2);
    if (bogeyBlinking && !blinkPhase_) {
        bogeyChar = state.bogeyCounterChar2;
        bogeyDot  = state.bogeyCounterDot2;
    }
    drawTopCounterPair(bogeyChar, topMuted, bogeyDot);
    const V1Settings& s = settingsManager.get();
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

    const bool needsFullRedraw =
        currentScreen_ != ScreenMode::Resting || dirty_.resetTracking;

    // Don't process resting update if we're in Scanning mode
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

    // Mode transition → full redraw via element cache invalidation
    if (needsFullRedraw) {
        if (currentScreen_ == ScreenMode::Live) {
            perfRecordDisplayRedrawReason(PerfDisplayRedrawReason::LeaveLive);
        } else if (currentScreen_ == ScreenMode::Persisted) {
            perfRecordDisplayRedrawReason(PerfDisplayRedrawReason::LeavePersisted);
        }
        perfRecordDisplayScreenTransition(
            perfScreenForMode(currentScreen_),
            PerfDisplayScreen::Resting,
            millis());
    }

    perfRecordDisplayRenderPath(restingRenderPathForScenario());

    // In resting mode, never show muted visual — apps commonly set volume to 0
    // when idle, adjusting on new alerts.
    //
    // Valentine's Law note (sanctioned mirror deviation, reviewed 2026-07-09):
    // this hides the V1's mute state while resting, so a lingering mute (tap
    // gesture, app housekeeping) is invisible until the next alert arrives
    // already quiet. Accepted because no live threat exists at rest — no
    // urgency is downgraded — and a mirrored resting mute icon would flicker
    // meaninglessly in proxy mode where the app owns muting. If revisited,
    // the sketched adjustment is: mirror mute at rest only when no proxy
    // client is connected (standalone mode gets honest instrument state; the
    // app-trust split is documented in docs/CONNECTIVITY_MODES.md).
    // See docs/VALENTINE_PHILOSOPHY.md (principle #7 vs checklist item 3).
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
        showVolumeWarning = volZeroWarn_.evaluate(
            volZero, proxyConnected, speedVolZeroActive_);
    }

#if defined(DISPLAY_WAVESHARE_349)
    const bool volumeWarningTimeDriven =
        bleContextFresh &&
        state.hasVolumeData &&
        state.mainVolume == 0 &&
        !bleCtx_.proxyConnected &&
        !speedVolZeroActive_;
    const bool allowRestingNoOpSkip =
        !volumeWarningTimeDriven && !hasTimeDrivenRestingBlink(state);
    RestingNoOpKey restingNoOpKey;
    if (allowRestingNoOpSkip) {
        restingNoOpKey = buildRestingNoOpKey(state, nowMs, bleContextFresh);
        if (!needsFullRedraw &&
            !hadPendingExternalDraws &&
            canSkipRestingNoOp(restingNoOpKey)) {
            perfRecordDisplayRedrawReason(PerfDisplayRedrawReason::CacheHitSkipFlush);
            perfRecordDisplayFlushDecision(PerfDisplayFlushDecisionPath::Resting,
                                           PerfDisplayFlushDecisionReason::CacheHit);
            lastState_ = state;
            return;
        }
    } else {
        invalidateRestingNoOpKey();
    }
#endif

    // Always use multi-alert layout positioning
    dirty_.multiAlert = true;
    multiAlertMode_ = false;

    uint32_t stageStartUs = 0;
    if (needsFullRedraw) {
        stageStartUs = micros();
        drawBaseFrame();
        perfRecordDisplayRenderSubphaseUs(PerfDisplayRenderSubphase::BaseFrame,
                                          micros() - stageStartUs);
    }

    char topChar = state.bogeyCounterChar;
    stageStartUs = micros();
    drawStatusStrip(state, topChar, effectiveMuted, state.bogeyCounterDot);
    perfRecordDisplayRenderSubphaseUs(PerfDisplayRenderSubphase::StatusStrip,
                                      micros() - stageStartUs);

    // B1: Ku alerts have no dedicated LED on the V1 band row — they light K.
    // OR BAND_KU into the mask so drawBandIndicators relabels K -> "Ku".
    const uint8_t bandMaskWithKu1 =
        static_cast<uint8_t>(state.activeBands | (state.hasKuAlert ? BAND_KU : 0));
    const bool bandsPainted = drawBandIndicators(bandMaskWithKu1, effectiveMuted);
    if (bandsPainted || dirty_.gpsIndicator) {
        drawGpsIndicator();  // Repaint: band FILL_RECT overlaps GPS x-range when bands change
    }

    // Volume-zero warning replaces frequency display
    if (showVolumeWarning) {
        drawVolumeZeroWarning();
    } else {
        stageStartUs = micros();
        drawFrequency(0, BAND_NONE, effectiveMuted);
        perfRecordDisplayRenderSubphaseUs(PerfDisplayRenderSubphase::Frequency,
                                          micros() - stageStartUs);
    }

    stageStartUs = micros();
    drawVerticalSignalBars(state.signalBars, state.signalBars, BAND_KA, effectiveMuted);
    perfRecordDisplayRenderSubphaseUs(PerfDisplayRenderSubphase::BandsBars,
                                      micros() - stageStartUs);

    stageStartUs = micros();
    drawDirectionArrow(DIR_NONE, effectiveMuted, 0);
    perfRecordDisplayRenderSubphaseUs(PerfDisplayRenderSubphase::ArrowsIcons,
                                      micros() - stageStartUs);

    // Clear any persisted card slots
    AlertData emptyPriority;
    stageStartUs = micros();
    drawSecondaryAlertCards(nullptr, 0, emptyPriority, effectiveMuted);
    perfRecordDisplayRenderSubphaseUs(PerfDisplayRenderSubphase::Cards,
                                      micros() - stageStartUs);

    stageStartUs = micros();
    const bool paintedThisFrame = !drawnRegion_.empty();
    PerfDisplayFlushDecisionReason flushDecision = PerfDisplayFlushDecisionReason::CacheHit;
    if (needsFullRedraw) {
        flushDecision = PerfDisplayFlushDecisionReason::FullRedraw;
    } else if (hadPendingExternalDraws) {
        flushDecision = PerfDisplayFlushDecisionReason::PendingExternal;
    } else if (paintedThisFrame) {
        flushDecision = PerfDisplayFlushDecisionReason::Painted;
    }
    if (flushDecision != PerfDisplayFlushDecisionReason::CacheHit) {
        DISPLAY_FLUSH();
    } else {
        perfRecordDisplayRedrawReason(PerfDisplayRedrawReason::CacheHitSkipFlush);
    }
    perfRecordDisplayFlushDecision(PerfDisplayFlushDecisionPath::Resting, flushDecision);
    drawnRegion_.reset();
    perfRecordDisplayRenderSubphaseUs(PerfDisplayRenderSubphase::Flush,
                                      micros() - stageStartUs);

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

    const bool needsFullRedraw =
        currentScreen_ != ScreenMode::Persisted || dirty_.resetTracking;

    if (needsFullRedraw) {
        if (currentScreen_ == ScreenMode::Live) {
            perfRecordDisplayRedrawReason(PerfDisplayRedrawReason::LeaveLive);
        }
        perfRecordDisplayScreenTransition(
            perfScreenForMode(currentScreen_),
            PerfDisplayScreen::Persisted,
            millis());
    }

    perfRecordDisplayRenderPath(persistedRenderPathForScenario());

    dirty_.multiAlert = true;
    multiAlertMode_ = false;

    uint32_t stageStartUs = 0;
    if (needsFullRedraw) {
        stageStartUs = micros();
        drawBaseFrame();
        perfRecordDisplayRenderSubphaseUs(PerfDisplayRenderSubphase::BaseFrame,
                                          micros() - stageStartUs);
    }

    // Bogey counter shows V1's decoded display — NOT greyed, always visible
    char topChar = state.bogeyCounterChar;
    syncTopIndicators(static_cast<uint32_t>(millis()));
    stageStartUs = micros();
    drawStatusStrip(state, topChar, false, state.bogeyCounterDot);
    perfRecordDisplayRenderSubphaseUs(PerfDisplayRenderSubphase::StatusStrip,
                                      micros() - stageStartUs);

    // Band indicator in persisted color
    stageStartUs = micros();
    const bool bandsPainted = drawBandIndicators(alert.band, true);
    if (bandsPainted || dirty_.gpsIndicator) {
        drawGpsIndicator();  // Repaint: band FILL_RECT overlaps GPS x-range when bands change
    }
    perfRecordDisplayRenderSubphaseUs(PerfDisplayRenderSubphase::BandsBars,
                                      micros() - stageStartUs);

    // Frequency in persisted color
    const bool isPhotoRadar =
        (alert.photoType != 0) ||
        state.hasPhotoAlert ||
        (state.bogeyCounterChar == 'P');
    stageStartUs = micros();
    drawFrequency(alert.frequency, alert.band, true, isPhotoRadar);
    perfRecordDisplayRenderSubphaseUs(PerfDisplayRenderSubphase::Frequency,
                                      micros() - stageStartUs);

    // No signal bars — draw empty
    stageStartUs = micros();
    drawVerticalSignalBars(0, 0, alert.band, true);
    perfRecordDisplayRenderSubphaseUs(PerfDisplayRenderSubphase::BandsBars,
                                      micros() - stageStartUs);

    // Arrows in persisted grey
    stageStartUs = micros();
    drawDirectionArrow(alert.direction, true);
    perfRecordDisplayRenderSubphaseUs(PerfDisplayRenderSubphase::ArrowsIcons,
                                      micros() - stageStartUs);

    // Clear card area
    AlertData emptyPriority;
    stageStartUs = micros();
    drawSecondaryAlertCards(nullptr, 0, emptyPriority, true);
    perfRecordDisplayRenderSubphaseUs(PerfDisplayRenderSubphase::Cards,
                                      micros() - stageStartUs);

    stageStartUs = micros();
    const bool paintedThisFrame = !drawnRegion_.empty();
    PerfDisplayFlushDecisionReason flushDecision = PerfDisplayFlushDecisionReason::CacheHit;
    if (needsFullRedraw) {
        flushDecision = PerfDisplayFlushDecisionReason::FullRedraw;
    } else if (hadPendingExternalDraws) {
        flushDecision = PerfDisplayFlushDecisionReason::PendingExternal;
    } else if (paintedThisFrame) {
        flushDecision = PerfDisplayFlushDecisionReason::Painted;
    }
    if (flushDecision != PerfDisplayFlushDecisionReason::CacheHit) {
        DISPLAY_FLUSH();
    } else {
        perfRecordDisplayRedrawReason(PerfDisplayRedrawReason::CacheHitSkipFlush);
    }
    perfRecordDisplayFlushDecision(PerfDisplayFlushDecisionPath::Persisted, flushDecision);
    drawnRegion_.reset();
    perfRecordDisplayRenderSubphaseUs(PerfDisplayRenderSubphase::Flush,
                                      micros() - stageStartUs);

    dirty_.resetTracking = false;
    currentScreen_ = ScreenMode::Persisted;
}

// ============================================================================
// update(priority, allAlerts, alertCount, state) — Live alert display
// ============================================================================

void V1Display::update(const AlertData& priority, const AlertData* allAlerts,
                       int alertCount, const DisplayState& state) {
    persistedMode_ = false;

    if (!priority.isValid || priority.band == BAND_NONE) {
        // Do not clear drawnRegion_ here: a lower-level external setter may
        // have painted an indicator before this invalid live packet. Leaving
        // the region queued lets the next real display frame flush it.
        PERF_INC(displayLiveInvalidPrioritySkips);
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

    const V1Settings& s = settingsManager.get();
    const bool needsFullRedraw =
        currentScreen_ != ScreenMode::Live || dirty_.resetTracking;

    // Correctness override for V1 blink frames. Diag14 proved the framebuffer
    // paints Image1/Image2 arrow phases correctly, but the panel still looked
    // steady when those changes were delivered through repeated small
    // flushRegion() windows. Prior AXS15231B partial-window attempts were
    // unstable on-device, so live arrow/band blink frames that actually changed
    // pixels use the proven full canvas push.
    //
    // Important: cache-hit frames still skip flushing. V1 sends display packets
    // faster than the 96 ms blink cadence, so forcing a full flush merely
    // because flashBits are present would waste SPI time and risk higher-priority
    // BLE ingest/drain without changing the visible image.
    //
    // Kept separate from needsFullRedraw so EnterLive logging / screen
    // transition recording (gated by needsFullRedraw below) only fire on
    // actual mode transitions, not every blink frame.
    const bool blinkForceFullFlush =
        (state.flashBits != 0) || (state.bandFlashBits != 0);

    if (needsFullRedraw) {
        DISPLAY_LOG("[DISP] Entering Live mode (was %d), alertCount=%d\n",
                    (int)currentScreen_, alertCount);
        perfRecordDisplayRedrawReason(PerfDisplayRedrawReason::EnterLive);
        perfRecordDisplayScreenTransition(
            perfScreenForMode(currentScreen_),
            PerfDisplayScreen::Live,
            millis());
    }

    perfRecordDisplayRenderPath(liveRenderPathForScenario());

    dirty_.multiAlert = true;
    multiAlertMode_ = true;

    // Arrow display: priority arrow only if setting enabled, otherwise all V1 arrows.
    //
    // Valentine's Law note: intersecting with priorityArrow HIDES direction
    // intelligence the V1 is lighting (principle #2).  This is acceptable only
    // because the driver explicitly opted in per slot (checklist item 4 — the
    // driver decided).  Never make this the default.
    // See docs/VALENTINE_PHILOSOPHY.md.
    Direction arrowsToShow;
    if (settingsManager.getSlotPriorityArrowOnly(s.activeSlot)) {
        arrowsToShow = static_cast<Direction>(state.priorityArrow & state.arrows);
    } else {
        arrowsToShow = state.arrows;
    }

    char liveTopCounterChar = state.bogeyCounterChar;
    bool liveTopCounterDot = state.bogeyCounterDot;

    uint32_t stageStartUs = 0;
    if (needsFullRedraw) {
        stageStartUs = micros();
        drawBaseFrame();
        perfRecordDisplayRenderSubphaseUs(PerfDisplayRenderSubphase::BaseFrame,
                                          micros() - stageStartUs);
    }

    syncTopIndicators(static_cast<uint32_t>(millis()));
    stageStartUs = micros();
    drawStatusStrip(state, liveTopCounterChar, state.muted, liveTopCounterDot);
    perfRecordDisplayRenderSubphaseUs(PerfDisplayRenderSubphase::StatusStrip,
                                      micros() - stageStartUs);

    // Photo-radar detection: image1 is the steady displayed character. If V1
    // is showing 'P' (steady or in the on-phase of a blink), liveTopCounterChar
    // == 'P'. Under blink-pair semantics image2=='P' implies image1=='P'
    // (steady-P case), so a separate byte2 check would be redundant.
    const bool isPhotoRadar =
        (priority.photoType != 0) ||
        state.hasPhotoAlert ||
        (liveTopCounterChar == 'P');
    stageStartUs = micros();
    drawFrequency(priority.frequency, priority.band, state.muted, isPhotoRadar);
    perfRecordDisplayRenderSubphaseUs(PerfDisplayRenderSubphase::Frequency,
                                      micros() - stageStartUs);

    stageStartUs = micros();
    // B1: see above — re-label K cell as "Ku" when a Ku alert is active.
    const uint8_t bandMaskWithKu2 =
        static_cast<uint8_t>(state.activeBands | (state.hasKuAlert ? BAND_KU : 0));
    const bool bandsPainted = drawBandIndicators(bandMaskWithKu2, state.muted, state.bandFlashBits);
    if (bandsPainted || dirty_.gpsIndicator) {
        drawGpsIndicator();  // Repaint: band FILL_RECT overlaps GPS x-range when bands change
    }
    drawVerticalSignalBars(state.signalBars, state.signalBars, priority.band, state.muted);
    perfRecordDisplayRenderSubphaseUs(PerfDisplayRenderSubphase::BandsBars,
                                      micros() - stageStartUs);

    // Arrow blink: V1 reports the priority-arrow blink directly via image1 vs
    // image2 in the InfDisplayData packet (image1 = currently lit, image2 =
    // steady).  packet_parser.cpp computes state.flashBits = image1 & ~image2
    // & 0xE0, so any direction V1 wants to blink is already in state.flashBits.
    // Synthesizing a flash bit here whenever alertCount > 1 would misread
    // ESP Spec 3.015 §9 and force blinks during 2-alert windows where V1
    // explicitly reports "no blink" (image1 == image2), so we use the
    // packet-reported flash bits as-is.
    const uint8_t arrowFlashBits = state.flashBits;
    stageStartUs = micros();
    drawDirectionArrow(arrowsToShow, state.muted, arrowFlashBits);
    perfRecordDisplayRenderSubphaseUs(PerfDisplayRenderSubphase::ArrowsIcons,
                                      micros() - stageStartUs);

    if (needsFullRedraw) {
        // Force card redraw only when a full screen clear invalidated the card area.
        dirty_.cards = true;
        elementCaches_.cards.invalidate();
    }

    stageStartUs = micros();
    drawSecondaryAlertCards(allAlerts, alertCount, priority, state.muted);
    perfRecordDisplayRenderSubphaseUs(PerfDisplayRenderSubphase::Cards,
                                      micros() - stageStartUs);

    // Region-union partial-flush dispatch (steady-state optimization).
    //
    // Each leaf draw function annotates its paint rect via drawnRegion_.add().
    // DrawnRegion retains both a historical union bbox and the individual
    // item rects. Six outcomes:
    //
    //   1. needsFullRedraw        → DISPLAY_FLUSH() (mode transition / reset)
    //   2. drawnRegion_.empty()   → no flush at all (every leaf cache hit)
    //   3. blink or arrow visibility change → DISPLAY_FLUSH()
    //                                (small-window arrow updates are not
    //                                reliable enough on this panel path)
    //   4. safe split rects   → flushRegion(each item rect) when the union is
    //                            mostly dead space and no arrow rect repainted
    //   5. union ≥ kPartialFlushAreaCap → DISPLAY_FLUSH() (no savings from
    //                                partial; avoids row-by-row blit overhead)
    //   6. otherwise              → flushRegion(union)
    //
    // Bounded-drift safety (Valentine's Law): if a leaf forgets to annotate
    // its paint, the worst case is one stale frame on the panel — the next
    // frame that *does* annotate any rect triggers a flush covering both, and
    // any mode transition goes through needsFullRedraw. The display never
    // stays stale across multiple frames (bounded-drift Valentine's Law
    // argument; see docs/VALENTINE_PHILOSOPHY.md Part III).
    //
    // kPartialFlushAreaCap = 50% of canvas = 640 * 172 / 2 = 55040 px.
    // Above that, flushRegion's row-by-row SPI overhead outweighs the savings
    // from skipping the rest of the canvas.
    constexpr uint32_t kPartialFlushAreaCap =
        static_cast<uint32_t>(SCREEN_WIDTH) * static_cast<uint32_t>(SCREEN_HEIGHT) / 2;

    if (hadPendingExternalDraws) {
        const uint8_t pendingSources =
            static_cast<uint8_t>(pendingExternalDraws.sourceMask() |
                                 DisplayDirtyRegionSource::External);
        drawnRegion_.add(pendingExternalDraws.x(), pendingExternalDraws.y(),
                         pendingExternalDraws.w(), pendingExternalDraws.h(),
                         pendingSources);
    }

    const bool smallWindowForceFullFlush =
        blinkForceFullFlush ||
        (arrowVisibilityForceFullFlush_ && drawnRegion_.areaPx() < kPartialFlushAreaCap);
    DispatchRectList multiRectDispatch;
    const bool useMultiRectDispatch =
        !needsFullRedraw &&
        !smallWindowForceFullFlush &&
        shouldUseMultiRectDispatch(drawnRegion_,
                                   kPartialFlushAreaCap,
                                   arrowPaintedThisFrame_,
                                   multiRectDispatch);

    stageStartUs = micros();
    if (needsFullRedraw) {
        DISPLAY_FLUSH();
        perfRecordDisplayRedrawReason(PerfDisplayRedrawReason::FullFlushForRedraw);
    } else if (drawnRegion_.empty()) {
        perfRecordDisplayRedrawReason(PerfDisplayRedrawReason::CacheHitSkipFlush);
    } else if (smallWindowForceFullFlush) {
        // Bypass partial flush only when this blink-bearing or arrow
        // visibility-changing frame painted pixels. Cache-hit blink packets
        // above still skip. Direction-set changes repaint active/resting arrow
        // states; if the previous frame was a blink-off PALETTE_BG phase, a
        // missed small-window partial flush leaves the resting glyph blank.
        DISPLAY_FLUSH();
        perfRecordDisplayRedrawReason(PerfDisplayRedrawReason::FullFlushForRedraw);
    } else if (useMultiRectDispatch) {
        for (uint8_t i = 0; i < multiRectDispatch.count; ++i) {
            const DrawnRegion::Rect& rect = multiRectDispatch.rects[i];
            flushRegion(rect.x, rect.y, rect.w, rect.h);
        }
        perfRecordDisplayRedrawReason(PerfDisplayRedrawReason::PartialRegionFlush);
    } else if (drawnRegion_.areaPx() >= kPartialFlushAreaCap) {
        perfRecordDisplayUnionExceedsCap(drawnRegion_.areaPx(),
                                         drawnRegion_.rectCount(),
                                         drawnRegion_.sourceMask());
        DISPLAY_FLUSH();
        perfRecordDisplayRedrawReason(PerfDisplayRedrawReason::UnionExceedsCap);
    } else {
        flushRegion(drawnRegion_.x(), drawnRegion_.y(),
                    drawnRegion_.w(), drawnRegion_.h());
        perfRecordDisplayRedrawReason(PerfDisplayRedrawReason::PartialRegionFlush);
    }
    perfRecordDisplayRenderSubphaseUs(PerfDisplayRenderSubphase::Flush,
                                      micros() - stageStartUs);

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

#endif  // DISPLAY_RENDER_FRAME_ONLY

/**
 * Secondary alert cards — mini V1 alert cards at screen bottom.
 *
 * Ported from the legacy v1g2_simple tree (display_cards.cpp, Phase 2L
 * extraction) onto the current display stack. Rendering, cache, grace/persist
 * and incremental-redraw behavior match the legacy implementation.
 */

#include "display.h"
#include "display_layout.h"
#include "display_draw.h"
#include "display_dirty_flags.h"
#include "display_element_caches.h"
#include "display_palette.h"
#include "display_text.h"
#include "display_font_manager.h"
#include "settings.h"
#include <algorithm>
#include <cstring>

// ============================================================================
// Card slot state lives on elementCaches_.cards (slots + lastDrawnPositions).
// Previously these were file-scoped statics here; moving them into the
// element-cache struct means prepareFullRedrawNoClear() -> invalidateAll()
// actually zeros slot state alongside the framebuffer clear, preventing
// ghost cards from surviving across a full-redraw boundary.
// See include/display_element_caches.h (CardsRenderCache).
// ============================================================================

// --- Secondary alert cards ---

void V1Display::drawSecondaryAlertCards(const AlertData* alerts, int alertCount, const AlertData& priority, bool muted) {
#if defined(DISPLAY_WAVESHARE_349)
    const DisplayLayout::DisplayRect cardsClearRect = DisplayLayout::cardsClearRect();

    // Get persistence time from profile settings (same as main alert persistence)
    const V1Settings& settings = settingsManager.get();
    uint8_t persistSec = settingsManager.getSlotAlertPersistSec(settings.activeSlot);
    unsigned long gracePeriodMs = persistSec * 1000UL;

    // If persistence is disabled (0), cards disappear immediately
    if (gracePeriodMs == 0) {
        gracePeriodMs = 1;  // Minimum 1ms so expiration logic works
    }

    unsigned long now = millis();

    // Track profile changes - clear cards when profile rotates
    if (settings.activeSlot != elementCaches_.cards.lastProfileSlot) {
        elementCaches_.cards.lastProfileSlot = settings.activeSlot;
        // Clear all card state on profile change
        for (int c = 0; c < 2; c++) {
            elementCaches_.cards.slots[c].alert = AlertData();
            elementCaches_.cards.slots[c].lastSeen = 0;
            elementCaches_.cards.lastDrawnPositions[c].band = BAND_NONE;
            elementCaches_.cards.lastDrawnPositions[c].frequency = 0;
            elementCaches_.cards.lastDrawnPositions[c].bars = 0;
        }
        elementCaches_.cards.lastDrawnCount = 0;
        elementCaches_.cards.lastPriority = AlertData();
    }

    // If called with nullptr alerts and count 0, clear V1 card state
    if (alerts == nullptr && alertCount == 0) {
        bool hadDrawnCards = elementCaches_.cards.lastDrawnCount > 0;
        for (int c = 0; c < 2; c++) {
            if (elementCaches_.cards.lastDrawnPositions[c].band != BAND_NONE) {
                hadDrawnCards = true;
                break;
            }
        }

        for (int c = 0; c < 2; c++) {
            elementCaches_.cards.slots[c].alert = AlertData();
            elementCaches_.cards.slots[c].lastSeen = 0;
            elementCaches_.cards.lastDrawnPositions[c] = CardDrawnPosition();
        }
        elementCaches_.cards.lastPriority = AlertData();

        // Clear the card area only when a previous frame actually drew cards.
        // Resting/persisted updates call this path every idle frame; treating
        // an already-empty card row as painted forces otherwise cache-hit
        // resting frames down the safe-but-expensive full-panel flush route.
        if (hadDrawnCards) {
            if (cardsClearRect.w <= 0) {
                elementCaches_.cards.lastDrawnCount = 0;
                return;
            }
            FILL_RECT(cardsClearRect.x, cardsClearRect.y,
                      cardsClearRect.w, cardsClearRect.h, PALETTE_BG);
            drawnRegion_.add(DisplayLayout::kSecondaryCardsRect.x,
                             DisplayLayout::kSecondaryCardsRect.y,
                             DisplayLayout::kSecondaryCardsRect.w,
                             DisplayLayout::kSecondaryCardsRect.h);
        }
        // Reset last drawn count so next time cards appear, change is detected.
        elementCaches_.cards.lastDrawnCount = 0;
        return;
    }

    // Helper: check if two alerts match (same band + frequency within tolerance)
    // V1 frequency can jitter by a few MHz between frames. Use a tight ±2 MHz
    // tolerance here so distinct nearby bogeys (e.g. two Ka sources 3-4 MHz apart)
    // don't collapse into one slot. The redraw-hysteresis path below uses a looser
    // ±5 MHz window to avoid flicker on jitter within the same bogey.
    auto alertsMatch = [](const AlertData& a, const AlertData& b) -> bool {
        if (a.band != b.band) return false;
        if (a.band == BAND_LASER) return true;
        // Use a small tolerance to handle V1 jitter without merging distinct nearby bogeys
        const uint32_t ALERT_IDENTITY_TOLERANCE_MHZ = 2;
        uint32_t diff = (a.frequency > b.frequency) ? (a.frequency - b.frequency) : (b.frequency - a.frequency);
        return diff <= ALERT_IDENTITY_TOLERANCE_MHZ;
    };

    // Looser same-bogey continuity match: NEW identities are admitted with the
    // tight ±2 MHz window above so distinct nearby bogeys stay separate, but a
    // bogey that already owns a slot is refreshed/deduped with the same ±5 MHz
    // jitter window the priority handoff guard uses. Without this, a >±2 MHz
    // jitter frame fails the tight match, grace-persists the stale copy, and
    // admits a duplicate card for one physical bogey (which can also evict a
    // genuine third bogey's card).
    auto alertsMatchLoose = [](const AlertData& a, const AlertData& b) -> bool {
        if (a.band != b.band) return false;
        if (a.band == BAND_LASER) return true;
        const uint32_t SLOT_CONTINUITY_JITTER_MHZ = 5;
        uint32_t diff = (a.frequency > b.frequency) ? (a.frequency - b.frequency) : (b.frequency - a.frequency);
        return diff <= SLOT_CONTINUITY_JITTER_MHZ;
    };

    // Helper: check if alert matches priority (returns false if priority is invalid)
    auto isSameAsPriority = [&priority, &alertsMatch](const AlertData& a) -> bool {
        if (!priority.isValid || priority.band == BAND_NONE) return false;
        return alertsMatch(a, priority);
    };

    // Persist the previous priority as a card when a new radar priority takes over and the
    // old signal has disappeared. Laser must remain live-owned by its current source, so a
    // cleared laser priority never grace-persists as a secondary card.
    if (elementCaches_.cards.lastPriority.isValid && elementCaches_.cards.lastPriority.band != BAND_NONE) {
        bool priorityChanged = !alertsMatch(elementCaches_.cards.lastPriority, priority);
        bool oldPriorityGone = true;

        // Check if old priority is still in current alerts
        if (alerts != nullptr) {
            for (int i = 0; i < alertCount; i++) {
                if (alertsMatch(elementCaches_.cards.lastPriority, alerts[i])) {
                    oldPriorityGone = false;
                    break;
                }
            }
        }

        // Jitter guard (v1simple addition, not in the legacy tree): a
        // "priority change" where the new priority is the same band within
        // the ±5 MHz redraw-hysteresis window is frequency jitter on one
        // bogey, not a handoff. Without this, a >±2 MHz jitter frame fails
        // alertsMatch, looks like a departed old priority, and admits a
        // ghost copy of the live priority that re-admits after every grace
        // expiry — a card that never clears.
        const uint32_t PRIORITY_JITTER_GUARD_MHZ = 5;
        bool sameBogeyJitter = false;
        if (priority.isValid && priority.band != BAND_NONE &&
            priority.band == elementCaches_.cards.lastPriority.band &&
            priority.band != BAND_LASER) {
            uint32_t priDiff =
                (priority.frequency > elementCaches_.cards.lastPriority.frequency)
                    ? (priority.frequency - elementCaches_.cards.lastPriority.frequency)
                    : (elementCaches_.cards.lastPriority.frequency - priority.frequency);
            sameBogeyJitter = priDiff <= PRIORITY_JITTER_GUARD_MHZ;
        }

        // If old priority is gone (not just demoted), add it as persisted card
        if (priorityChanged && oldPriorityGone && !sameBogeyJitter &&
            elementCaches_.cards.lastPriority.band != BAND_LASER) {
            // Check if already tracked
            bool found = false;
            for (int c = 0; c < 2; c++) {
                if (elementCaches_.cards.slots[c].lastSeen > 0 && alertsMatchLoose(elementCaches_.cards.slots[c].alert, elementCaches_.cards.lastPriority)) {
                    found = true;
                    break;
                }
            }

            // Add to empty slot if not already tracked
            if (!found) {
                for (int c = 0; c < 2; c++) {
                    if (elementCaches_.cards.slots[c].lastSeen == 0) {
                        elementCaches_.cards.slots[c].alert = elementCaches_.cards.lastPriority;
                        elementCaches_.cards.slots[c].lastSeen = now;
                        break;
                    }
                }
            }
        }
    }

    // Update last priority tracking
    elementCaches_.cards.lastPriority = priority;

    // Refresh existing slots: match by (band, frequency) and update timestamp if the alert
    // is still live. Slots that miss a match past gracePeriodMs expire.
    for (int c = 0; c < 2; c++) {
        if (elementCaches_.cards.slots[c].lastSeen == 0) continue;

        bool stillExists = false;
        if (alerts != nullptr) {
            for (int i = 0; i < alertCount; i++) {
                if (alertsMatchLoose(elementCaches_.cards.slots[c].alert, alerts[i])) {
                    stillExists = true;
                    elementCaches_.cards.slots[c].alert = alerts[i];  // Update with latest data
                    elementCaches_.cards.slots[c].lastSeen = now;
                    break;
                }
            }
        }

        // Expire if past grace period
        if (!stillExists) {
            // Pinned/timed visual-verification steps describe the exact cards
            // for that authored frame.  Runtime alert persistence is useful in
            // normal driving, but would make a dirty transition depend on the
            // active profile's grace setting and HTTP timing.
            const bool expireForVisualPreview = previewIndicatorOverridesActive_;
            unsigned long age = now - elementCaches_.cards.slots[c].lastSeen;
            if (expireForVisualPreview || age > gracePeriodMs) {
                elementCaches_.cards.slots[c].alert = AlertData();
                elementCaches_.cards.slots[c].lastSeen = 0;
            }
        }
    }

    // Admit new non-priority alerts into empty slots. The priority alert lives in the main
    // display and never claims a card slot.
    if (alerts != nullptr) {
        for (int i = 0; i < alertCount; i++) {
            if (!alerts[i].isValid || alerts[i].band == BAND_NONE) continue;
            if (isSameAsPriority(alerts[i])) continue;  // Skip priority - don't waste a card slot

            // Check if already tracked (loose: a jitter twin of a tracked
            // bogey must refresh that slot, not claim a second card)
            bool found = false;
            for (int c = 0; c < 2; c++) {
                if (elementCaches_.cards.slots[c].lastSeen > 0 && alertsMatchLoose(elementCaches_.cards.slots[c].alert, alerts[i])) {
                    found = true;
                    break;
                }
            }

            if (!found) {
                // Find empty slot
                for (int c = 0; c < 2; c++) {
                    if (elementCaches_.cards.slots[c].lastSeen == 0) {
                        elementCaches_.cards.slots[c].alert = alerts[i];
                        elementCaches_.cards.slots[c].lastSeen = now;
                        break;
                    }
                }
            }
        }
    }

    // For debug logging if needed
    [[maybe_unused]] bool doDebug = false;

    // Helper: get signal bars for an alert based on direction
    auto getAlertBars = [](const AlertData& a) -> uint8_t {
        if (a.direction & DIR_FRONT) return a.frontStrength;
        if (a.direction & DIR_REAR) return a.rearStrength;
        return (a.frontStrength > a.rearStrength) ? a.frontStrength : a.rearStrength;
    };

    // Build list of cards to draw this frame (V1 alerts only)
    struct CardToDraw {
        int slot;           // V1 card slot index
        bool isGraced;
        uint8_t bars;       // Signal strength for V1 cards
    } cardsToDraw[2];
    int cardsToDrawCount = 0;

    // Add V1 secondary alerts
    for (int c = 0; c < 2 && cardsToDrawCount < 2; c++) {
        if (elementCaches_.cards.slots[c].lastSeen == 0) continue;
        if (isSameAsPriority(elementCaches_.cards.slots[c].alert)) continue;
        cardsToDraw[cardsToDrawCount].slot = c;
        cardsToDraw[cardsToDrawCount].bars = getAlertBars(elementCaches_.cards.slots[c].alert);
        // Check if live or graced
        bool isLive = false;
        if (alerts != nullptr) {
            for (int i = 0; i < alertCount; i++) {
                if (alertsMatch(elementCaches_.cards.slots[c].alert, alerts[i])) {
                    isLive = true;
                    break;
                }
            }
        }
        cardsToDraw[cardsToDrawCount].isGraced = !isLive;
        cardsToDrawCount++;
    }

    // ============================================================================
    // INCREMENTAL UPDATE LOGIC
    // ============================================================================
    // Instead of clearing all cards and redrawing, check each position independently

    // Capture force-redraw state before resetting. Two force inputs feed the
    // card row: the element-cache invalidation (screen clears route through
    // invalidateAll()) and dirty_.cards, which display_update.cpp raises when
    // a full-screen clear invalidated the card area mid-live-session.
    bool doForceRedraw = elementCaches_.cards.forceRedraw || dirty_.cards;
    dirty_.cards = false;                       // Consumed here
    elementCaches_.cards.forceRedraw = false;   // Consumed here

    // Helper to check if position needs full redraw vs just update
    auto positionNeedsFullRedraw = [&](int pos) -> bool {
        if (pos >= cardsToDrawCount) {
            // Position now empty but had content - needs clear
            return elementCaches_.cards.lastDrawnPositions[pos].band != BAND_NONE;
        }

        auto& last = elementCaches_.cards.lastDrawnPositions[pos];
        auto& curr = cardsToDraw[pos];

        // V1 card - check if band/freq/direction changed (needs full card redraw)
        // Use a ±5 MHz hysteresis window here (looser than alertsMatch's ±2 MHz
        // identity tolerance) so same-slot same-bogey frames with typical V1 jitter
        // don't trigger full-card redraws and visible flicker.
        const uint32_t CARD_REDRAW_HYSTERESIS_MHZ = 5;
        int slot = curr.slot;
        if (elementCaches_.cards.slots[slot].alert.band != last.band) return true;
        uint32_t freqDiff = (elementCaches_.cards.slots[slot].alert.frequency > last.frequency)
            ? (elementCaches_.cards.slots[slot].alert.frequency - last.frequency)
            : (last.frequency - elementCaches_.cards.slots[slot].alert.frequency);
        if (freqDiff > CARD_REDRAW_HYSTERESIS_MHZ) return true;
        if (elementCaches_.cards.slots[slot].alert.direction != last.direction) return true;
        if (curr.isGraced != last.isGraced) return true;
        if (muted != last.wasMuted) return true;
        return false;
    };

    // Helper to check if position needs dynamic update (bars only)
    auto positionNeedsDynamicUpdate = [&](int pos) -> bool {
        if (pos >= cardsToDrawCount) return false;

        auto& last = elementCaches_.cards.lastDrawnPositions[pos];
        auto& curr = cardsToDraw[pos];

        // V1 card - check signal bars
        if (curr.bars != last.bars) return true;
        return false;
    };

    // Process each card position
    for (int i = 0; i < DisplayLayout::CARD_SLOT_COUNT; i++) {
        const DisplayLayout::DisplayRect cardRect = DisplayLayout::cardRect(i);

        bool needsFullRedraw = positionNeedsFullRedraw(i) || doForceRedraw;
        bool needsDynamicUpdate = !needsFullRedraw && positionNeedsDynamicUpdate(i);

        // Clear position if it's now empty
        if (i >= cardsToDrawCount) {
            if (elementCaches_.cards.lastDrawnPositions[i].band != BAND_NONE) {
                FILL_RECT(cardRect.x, cardRect.y, cardRect.w, cardRect.h, PALETTE_BG);
                drawnRegion_.add(cardRect.x, cardRect.y, cardRect.w, cardRect.h);
                elementCaches_.cards.lastDrawnPositions[i].band = BAND_NONE;
            }
            continue;
        }

        if (!needsFullRedraw && !needsDynamicUpdate) {
            continue;  // Skip this position - nothing changed
        }
        drawnRegion_.add(cardRect.x, cardRect.y, cardRect.w, cardRect.h);

        // ============================================================================
        // V1 ALERT CARD
        // ============================================================================
        int c = cardsToDraw[i].slot;
        const AlertData& alert = elementCaches_.cards.slots[c].alert;
        bool isGraced = cardsToDraw[i].isGraced;
        bool drawMuted = muted || isGraced;
        uint8_t bars = cardsToDraw[i].bars;

        // Card background and border colors
        uint16_t bandCol = getBandColor(alert.band);
        uint16_t bgCol, borderCol;

        if (isGraced) {
            bgCol = 0x2104;
            borderCol = PALETTE_MUTED;
        } else if (drawMuted) {
            bgCol = 0x2104;
            borderCol = PALETTE_MUTED;
        } else {
            uint8_t r = ((bandCol >> 11) & 0x1F) * 3 / 10;
            uint8_t g = ((bandCol >> 5) & 0x3F) * 3 / 10;
            uint8_t b = (bandCol & 0x1F) * 3 / 10;
            bgCol = (r << 11) | (g << 5) | b;
            borderCol = bandCol;
        }

        uint16_t contentCol = (isGraced || drawMuted) ? PALETTE_MUTED : TFT_WHITE;
        uint16_t bandLabelCol = (isGraced || drawMuted) ? PALETTE_MUTED : bandCol;

        if (needsFullRedraw) {
            // ============================================================================
            // FULL V1 CARD REDRAW
            // ============================================================================
            FILL_ROUND_RECT(cardRect.x, cardRect.y, cardRect.w, cardRect.h, 5, bgCol);
            DRAW_ROUND_RECT(cardRect.x, cardRect.y, cardRect.w, cardRect.h, 5, borderCol);

            const DisplayLayout::DisplayRect textRect = DisplayLayout::cardTextRect(i);
            const int contentCenterY = cardRect.y + 18;
            const int topRowY = DisplayLayout::cardTextCursorY(i);

            // Direction arrow
            int arrowX = cardRect.x + 18;
            int arrowCY = contentCenterY;
            if (alert.direction & DIR_FRONT) {
                tft_->fillTriangle(arrowX, arrowCY - 7, arrowX - 6, arrowCY + 5, arrowX + 6, arrowCY + 5, contentCol);
            } else if (alert.direction & DIR_REAR) {
                tft_->fillTriangle(arrowX, arrowCY + 7, arrowX - 6, arrowCY - 5, arrowX + 6, arrowCY - 5, contentCol);
            } else if (alert.direction & DIR_SIDE) {
                FILL_RECT(arrowX - 6, arrowCY - 2, 12, 4, contentCol);
            }

            // Band + frequency
            int labelX = textRect.x;
            tft_->setTextColor(bandLabelCol);
            tft_->setTextSize(2);
            if (alert.band == BAND_LASER) {
                tft_->setCursor(labelX, topRowY);
                tft_->print("LASER");
            } else {
                const char* bandStr = bandToString(alert.band);
                tft_->setCursor(labelX, topRowY);
                tft_->print(bandStr);

                tft_->setTextColor(contentCol);
                int freqX = labelX + strlen(bandStr) * 12 + 4;
                tft_->setCursor(freqX, topRowY);
                if (alert.frequency > 0) {
                    char freqStr[10];
                    snprintf(freqStr, sizeof(freqStr), "%.3f", alert.frequency / 1000.0f);
                    tft_->print(freqStr);
                } else {
                    tft_->print("---");
                }
            }

            // Draw meter background
            const DisplayLayout::DisplayRect meterRect = DisplayLayout::cardMeterRect(i);
            FILL_RECT(meterRect.x, meterRect.y, meterRect.w, meterRect.h, 0x1082);
        }

        // Draw/update signal bars (always after full redraw, or on bars change)
        if (needsFullRedraw || needsDynamicUpdate) {
            const DisplayLayout::DisplayRect meterRect = DisplayLayout::cardMeterRect(i);

            // Clear meter area for bar update (not full redraw which already did it)
            if (!needsFullRedraw) {
                FILL_RECT(meterRect.x, meterRect.y, meterRect.w, meterRect.h, 0x1082);
            }

            uint16_t barColors[DisplayLayout::CARD_METER_BAR_COUNT] = {
                settings.colorBar1, settings.colorBar2, settings.colorBar3,
                settings.colorBar4, settings.colorBar5, settings.colorBar6
            };

            for (int b = 0; b < DisplayLayout::CARD_METER_BAR_COUNT; b++) {
                const DisplayLayout::DisplayRect barRect =
                    DisplayLayout::cardMeterBarRect(i, b);

                if (b < bars) {
                    uint16_t fillColor = (isGraced || drawMuted) ? PALETTE_MUTED : barColors[b];
                    FILL_RECT(barRect.x, barRect.y, barRect.w, barRect.h, fillColor);
                } else {
                    DRAW_RECT(barRect.x, barRect.y, barRect.w, barRect.h,
                              dimColor(barColors[b], 30));
                }
            }
        }

        // Update position tracking for V1 card
        elementCaches_.cards.lastDrawnPositions[i].band = alert.band;
        elementCaches_.cards.lastDrawnPositions[i].frequency = alert.frequency;
        elementCaches_.cards.lastDrawnPositions[i].direction = alert.direction;
        elementCaches_.cards.lastDrawnPositions[i].isGraced = isGraced;
        elementCaches_.cards.lastDrawnPositions[i].wasMuted = muted;
        elementCaches_.cards.lastDrawnPositions[i].bars = bars;
    }

    // Update global tracking
    elementCaches_.cards.lastDrawnCount = cardsToDrawCount;
#endif
}

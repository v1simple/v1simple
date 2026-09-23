#include "display.h"
#include "display_visual_contract.h"
#include "display_layout.h"
#include "display_draw.h"
#include "display_dirty_flags.h"
#include "display_element_caches.h"
#include "display_palette.h"
#include "display_text.h"
#include "display_font_manager.h"
#include "settings.h"
#include <algorithm>
#include <array>
#include <cstring>

// Card state belongs to elementCaches_.cards so a full cache invalidation also
// clears alert identities that no longer exist in the framebuffer.

namespace {

constexpr uint32_t kAlertIdentityToleranceMhz = 2;
constexpr uint32_t kSlotContinuityJitterMhz = 5;

bool alertsHaveSameRowIdentity(const AlertData& a, const AlertData& b, bool& identityAvailable) {
    const bool aHasIdentity = a.v1Index != AlertData::UNKNOWN_V1_INDEX;
    const bool bHasIdentity = b.v1Index != AlertData::UNKNOWN_V1_INDEX;
    identityAvailable = aHasIdentity || bHasIdentity;
    return aHasIdentity && bHasIdentity && a.v1Index == b.v1Index;
}

bool alertsIdentityMatch(const AlertData& a, const AlertData& b) {
    if (a.band != b.band) return false;
    if (a.band == BAND_LASER) return true;
    bool rowIdentityAvailable = false;
    const bool sameRow = alertsHaveSameRowIdentity(a, b, rowIdentityAvailable);
    if (rowIdentityAvailable) return sameRow;
    const uint32_t diff = (a.frequency > b.frequency) ? (a.frequency - b.frequency) : (b.frequency - a.frequency);
    return diff <= kAlertIdentityToleranceMhz;
}

bool alertsContinuityMatch(const AlertData& a, const AlertData& b) {
    if (a.band != b.band) return false;
    if (a.band == BAND_LASER) return true;
    const uint32_t diff = (a.frequency > b.frequency) ? (a.frequency - b.frequency) : (b.frequency - a.frequency);
    return diff <= kSlotContinuityJitterMhz;
}

bool alertMatchesPriority(const AlertData& alert, const AlertData& priority) {
    return priority.isValid && priority.band != BAND_NONE && alertsIdentityMatch(alert, priority);
}

struct SecondaryCardFrameEntry {
    int slot = 0;
    bool isGraced = false;
    uint8_t bars = 0;
};

struct SecondaryCardFrame {
    SecondaryCardFrameEntry cards[2]{};
    int count = 0;
};

void resetCardTemporalState(CardsRenderCache& cache) {
    for (CardSlot& slot : cache.slots) slot = CardSlot();
    cache.lastPriority = AlertData();
}

SecondaryCardFrame evolveSecondaryCardState(CardsRenderCache& cache, const AlertData* alerts, int alertCount,
                                            const AlertData& priority, unsigned long now, unsigned long gracePeriodMs,
                                            bool expireForVisualPreview) {
    // V1 row assignments distinguish concurrent rows, but may be compacted as
    // a table shrinks. Claim each current row at most once: prefer the exact
    // assignment, then fall back to the existing frequency-jitter continuity
    // rule without allowing one row to refresh multiple card slots.
    std::array<bool, 15> alertClaimed{};
    const int trackedAlertCount = std::min(alertCount, static_cast<int>(alertClaimed.size()));
    bool slotIsLive[2]{};
    for (int c = 0; c < 2; ++c) {
        CardSlot& slot = cache.slots[c];
        if (slot.lastSeen == 0) continue;
        int matchedAlert = -1;
        for (int pass = 0; alerts && pass < 2 && matchedAlert < 0; ++pass) {
            for (int i = 0; i < trackedAlertCount; ++i) {
                if (alertClaimed[static_cast<size_t>(i)]) continue;
                const bool matches = pass == 0 ? alertsIdentityMatch(slot.alert, alerts[i])
                                               : alertsContinuityMatch(slot.alert, alerts[i]);
                if (matches) {
                    matchedAlert = i;
                    break;
                }
            }
        }
        const bool stillExists = matchedAlert >= 0;
        if (stillExists) {
            alertClaimed[static_cast<size_t>(matchedAlert)] = true;
            slot.alert = alerts[matchedAlert];
            slot.lastSeen = now;
        }
        // Refresh first: continuity jitter can move this slot onto the new priority.
        if (alertMatchesPriority(slot.alert, priority) ||
            (!stillExists && (expireForVisualPreview || gracePeriodMs == 0 ||
                             (now - slot.lastSeen) > gracePeriodMs))) {
            slot = CardSlot();
        }
        slotIsLive[c] = stillExists && slot.lastSeen > 0;
    }

    for (int i = 0; alerts && i < alertCount; ++i) {
        if (!alerts[i].isValid || alerts[i].band == BAND_NONE || alertMatchesPriority(alerts[i], priority)) continue;
        const bool alreadyClaimed = i < trackedAlertCount && alertClaimed[static_cast<size_t>(i)];
        if (!alreadyClaimed) {
            int target = -1;
            for (int c = 0; c < 2; ++c) {
                if (cache.slots[c].lastSeen == 0) {
                    target = c;
                    break;
                }
            }
            // Live beats persisted: grace may use spare capacity, never hide
            // a live arrival. Use snapshot membership, not lastSeen == now,
            // because consecutive frames can arrive in the same millisecond.
            if (target < 0) {
                for (int c = 0; c < 2; ++c) {
                    if (!slotIsLive[c]) {
                        target = c;
                        break;
                    }
                }
            }
            if (target >= 0) {
                cache.slots[target].alert = alerts[i];
                cache.slots[target].lastSeen = now;
                slotIsLive[target] = true;
                if (i < trackedAlertCount) alertClaimed[static_cast<size_t>(i)] = true;
            }
        }
    }

    // Live rows use released capacity before a vanished priority receives grace.
    if (!expireForVisualPreview && gracePeriodMs > 0 &&
        cache.lastPriority.isValid && cache.lastPriority.band != BAND_NONE) {
        const bool priorityChanged = !alertsIdentityMatch(cache.lastPriority, priority);
        bool oldPriorityGone = true;
        for (int i = 0; alerts && i < alertCount; ++i) {
            if (alertsIdentityMatch(cache.lastPriority, alerts[i])) {
                oldPriorityGone = false;
                break;
            }
        }
        const bool sameBogeyJitter = priority.isValid && priority.band != BAND_NONE &&
                                     priority.band == cache.lastPriority.band && priority.band != BAND_LASER &&
                                     alertsContinuityMatch(priority, cache.lastPriority);
        if (priorityChanged && oldPriorityGone && !sameBogeyJitter && cache.lastPriority.band != BAND_LASER) {
            bool found = false;
            for (const CardSlot& slot : cache.slots) {
                if (slot.lastSeen > 0 && alertsContinuityMatch(slot.alert, cache.lastPriority)) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                for (CardSlot& slot : cache.slots) {
                    if (slot.lastSeen == 0) {
                        slot.alert = cache.lastPriority;
                        slot.lastSeen = now;
                        break;
                    }
                }
            }
        }
    }
    cache.lastPriority = priority;

    SecondaryCardFrame frame;
    for (int c = 0; c < 2 && frame.count < 2; ++c) {
        if (cache.slots[c].lastSeen == 0 || alertMatchesPriority(cache.slots[c].alert, priority)) continue;
        SecondaryCardFrameEntry& entry = frame.cards[frame.count++];
        entry.slot = c;
        entry.bars = DisplayVisualContract::alertMeterBars(cache.slots[c].alert);
        entry.isGraced = !slotIsLive[c];
    }
    return frame;
}

} // namespace

void V1Display::drawSecondaryAlertCards(const AlertData* alerts, int alertCount, const AlertData& priority,
                                        bool muted) {
#if defined(DISPLAY_WAVESHARE_349)
    const DisplayLayout::DisplayRect cardsClearRect = DisplayLayout::cardsClearRect();

    const V1Settings& settings = settings_.get();
    uint8_t persistSec = settings_.getSlotAlertPersistSec(settings.activeSlot);
    unsigned long gracePeriodMs = persistSec * 1000UL;

    unsigned long now = millis();

    if (settings.activeSlot != elementCaches_.cards.lastProfileSlot) {
        elementCaches_.cards.lastProfileSlot = settings.activeSlot;
        resetCardTemporalState(elementCaches_.cards);
        for (int c = 0; c < 2; c++) {
            elementCaches_.cards.lastDrawnPositions[c].band = BAND_NONE;
            elementCaches_.cards.lastDrawnPositions[c].frequency = 0;
            elementCaches_.cards.lastDrawnPositions[c].bars = 0;
        }
        elementCaches_.cards.lastDrawnCount = 0;
    }

    if (alerts == nullptr && alertCount == 0) {
        bool hadDrawnCards = elementCaches_.cards.lastDrawnCount > 0;
        for (int c = 0; c < 2; c++) {
            if (elementCaches_.cards.lastDrawnPositions[c].band != BAND_NONE) {
                hadDrawnCards = true;
                break;
            }
        }

        resetCardTemporalState(elementCaches_.cards);
        for (int c = 0; c < 2; c++) {
            elementCaches_.cards.lastDrawnPositions[c] = CardDrawnPosition();
        }

        // Clear the card area only when a previous frame actually drew cards.
        // Resting/persisted updates call this path every idle frame; treating
        // an already-empty card row as painted forces otherwise cache-hit
        // resting frames down the safe-but-expensive full-panel flush route.
        if (hadDrawnCards) {
            if (cardsClearRect.w <= 0) {
                elementCaches_.cards.lastDrawnCount = 0;
                return;
            }
            FILL_RECT(cardsClearRect.x, cardsClearRect.y, cardsClearRect.w, cardsClearRect.h, PALETTE_BG);
            drawnRegion_.add(DisplayLayout::kSecondaryCardsRect.x, DisplayLayout::kSecondaryCardsRect.y,
                             DisplayLayout::kSecondaryCardsRect.w, DisplayLayout::kSecondaryCardsRect.h);
        }
        elementCaches_.cards.lastDrawnCount = 0;
        return;
    }

    const SecondaryCardFrame frame = evolveSecondaryCardState(elementCaches_.cards, alerts, alertCount, priority, now,
                                                               gracePeriodMs, previewIndicatorOverridesActive_);
    const SecondaryCardFrameEntry* cardsToDraw = frame.cards;
    const int cardsToDrawCount = frame.count;

    // Capture force-redraw state before resetting. Two force inputs feed the
    // card row: the element-cache invalidation (screen clears route through
    // invalidateAll()) and dirty_.cards, which display_update.cpp raises when
    // a full-screen clear invalidated the card area mid-live-session.
    bool doForceRedraw = elementCaches_.cards.forceRedraw || dirty_.cards;
    dirty_.cards = false;
    elementCaches_.cards.forceRedraw = false;

    auto positionNeedsFullRedraw = [&](int pos) -> bool {
        if (pos >= cardsToDrawCount) {
            return elementCaches_.cards.lastDrawnPositions[pos].band != BAND_NONE;
        }

        auto& last = elementCaches_.cards.lastDrawnPositions[pos];
        auto& curr = cardsToDraw[pos];

        // Use a ±5 MHz hysteresis window here (looser than the synthetic-alert
        // identity fallback) so same-slot same-bogey frames with typical V1
        // jitter don't trigger full-card redraws and visible flicker.
        const uint32_t CARD_REDRAW_HYSTERESIS_MHZ = 5;
        int slot = curr.slot;
        if (elementCaches_.cards.slots[slot].alert.band != last.band)
            return true;
        uint32_t freqDiff = (elementCaches_.cards.slots[slot].alert.frequency > last.frequency)
                                ? (elementCaches_.cards.slots[slot].alert.frequency - last.frequency)
                                : (last.frequency - elementCaches_.cards.slots[slot].alert.frequency);
        if (freqDiff > CARD_REDRAW_HYSTERESIS_MHZ)
            return true;
        if (elementCaches_.cards.slots[slot].alert.direction != last.direction)
            return true;
        if (elementCaches_.cards.slots[slot].alert.photoType != last.photoType)
            return true;
        if (curr.isGraced != last.isGraced)
            return true;
        if (muted != last.wasMuted)
            return true;
        return false;
    };

    auto positionNeedsDynamicUpdate = [&](int pos) -> bool {
        if (pos >= cardsToDrawCount)
            return false;

        auto& last = elementCaches_.cards.lastDrawnPositions[pos];
        auto& curr = cardsToDraw[pos];

        if (curr.bars != last.bars)
            return true;
        return false;
    };

    for (int i = 0; i < DisplayLayout::CARD_SLOT_COUNT; i++) {
        const DisplayLayout::DisplayRect cardRect = DisplayLayout::cardRect(i);

        bool needsFullRedraw = positionNeedsFullRedraw(i) || doForceRedraw;
        bool needsDynamicUpdate = !needsFullRedraw && positionNeedsDynamicUpdate(i);

        if (i >= cardsToDrawCount) {
            if (elementCaches_.cards.lastDrawnPositions[i].band != BAND_NONE) {
                FILL_RECT(cardRect.x, cardRect.y, cardRect.w, cardRect.h, PALETTE_BG);
                drawnRegion_.add(cardRect.x, cardRect.y, cardRect.w, cardRect.h);
                elementCaches_.cards.lastDrawnPositions[i].band = BAND_NONE;
            }
            continue;
        }

        if (!needsFullRedraw && !needsDynamicUpdate) {
            continue;
        }
        drawnRegion_.add(cardRect.x, cardRect.y, cardRect.w, cardRect.h);

        int c = cardsToDraw[i].slot;
        const AlertData& alert = elementCaches_.cards.slots[c].alert;
        bool isGraced = cardsToDraw[i].isGraced;
        bool drawMuted = muted || isGraced;
        uint8_t bars = cardsToDraw[i].bars;

        const bool isPhoto = alert.band == BAND_K && alert.photoType != 0;
        uint16_t bandCol = isPhoto ? settings.colorBandPhoto : getBandColor(alert.band);
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
            FILL_ROUND_RECT(cardRect.x, cardRect.y, cardRect.w, cardRect.h, 5, bgCol);
            DRAW_ROUND_RECT(cardRect.x, cardRect.y, cardRect.w, cardRect.h, 5, borderCol);

            const DisplayLayout::DisplayRect textRect = DisplayLayout::cardTextRect(i);
            const int contentCenterY = cardRect.y + 18;
            const int topRowY = DisplayLayout::cardTextCursorY(i);

            int arrowX = cardRect.x + 18;
            int arrowCY = contentCenterY;
            if (alert.direction & DIR_FRONT) {
                tft_->fillTriangle(arrowX, arrowCY - 7, arrowX - 6, arrowCY + 5, arrowX + 6, arrowCY + 5, contentCol);
            } else if (alert.direction & DIR_REAR) {
                tft_->fillTriangle(arrowX, arrowCY + 7, arrowX - 6, arrowCY - 5, arrowX + 6, arrowCY - 5, contentCol);
            } else if (alert.direction & DIR_SIDE) {
                FILL_RECT(arrowX - 6, arrowCY - 2, 12, 4, contentCol);
            }

            int labelX = textRect.x;
            tft_->setTextColor(bandLabelCol);
            tft_->setTextSize(2);
            if (alert.band == BAND_LASER) {
                tft_->setCursor(labelX, topRowY);
                tft_->print("LASER");
            } else {
                // Valentine reports Photo as a K-band row with a nonzero
                // photo type. Keep that row's identity visible in its card
                // without inventing a separate RF band or crowding the card.
                const char* bandStr = isPhoto ? "P" : bandToString(alert.band);
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

            const DisplayLayout::DisplayRect meterRect = DisplayLayout::cardMeterRect(i);
            FILL_RECT(meterRect.x, meterRect.y, meterRect.w, meterRect.h, 0x1082);
        }

        if (needsFullRedraw || needsDynamicUpdate) {
            const DisplayLayout::DisplayRect meterRect = DisplayLayout::cardMeterRect(i);

            if (!needsFullRedraw) {
                FILL_RECT(meterRect.x, meterRect.y, meterRect.w, meterRect.h, 0x1082);
            }

            // Both meters share the per-segment colour array; the card's six
            // segments map one-to-one onto it with no draw-time interpolation.
            const uint16_t* barColors = settings.colorBars;

            for (int b = 0; b < DisplayLayout::CARD_METER_BAR_COUNT; b++) {
                const DisplayLayout::DisplayRect barRect = DisplayLayout::cardMeterBarRect(i, b);

                if (b < bars) {
                    uint16_t fillColor = (isGraced || drawMuted) ? PALETTE_MUTED : barColors[b];
                    FILL_RECT(barRect.x, barRect.y, barRect.w, barRect.h, fillColor);
                } else {
                    DRAW_RECT(barRect.x, barRect.y, barRect.w, barRect.h, dimColor(barColors[b], 30));
                }
            }
        }

        elementCaches_.cards.lastDrawnPositions[i].band = alert.band;
        // Meter-only updates must not move the frequency text's redraw baseline.
        if (needsFullRedraw) {
            elementCaches_.cards.lastDrawnPositions[i].frequency = alert.frequency;
        }
        elementCaches_.cards.lastDrawnPositions[i].direction = alert.direction;
        elementCaches_.cards.lastDrawnPositions[i].photoType = alert.photoType;
        elementCaches_.cards.lastDrawnPositions[i].isGraced = isGraced;
        elementCaches_.cards.lastDrawnPositions[i].wasMuted = muted;
        elementCaches_.cards.lastDrawnPositions[i].bars = bars;
    }

    elementCaches_.cards.lastDrawnCount = cardsToDrawCount;
#endif
}

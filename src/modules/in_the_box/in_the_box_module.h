#pragma once

#include "../../packet_parser_types.h"
#include "../../v1_in_the_box.h"
#include <array>
#include <stddef.h>
#include <stdint.h>

enum class InTheBoxClassification : uint8_t { Unknown, Outside, Inside };

struct InTheBoxDecision {
    bool allOutsideMuteEligible = false;
    bool anyInside = false;
    bool newInsideUnmute = false;
};

// V1Simple audio policy, not detector programming or a claim about VR app
// arbitration. The caller supplies only complete, fresh tables and owns mute
// commands, manual overrides, freshness, and resets on session/profile changes.
// Classification never removes or changes an alert's visual presentation.
class InTheBoxModule {
  public:
    static constexpr size_t MAX_ALERTS = 15;
    static constexpr uint32_t ENCOUNTER_TOLERANCE_MHZ = 5;

    static InTheBoxClassification classify(const V1InTheBoxSettings& settings, const AlertData& alert) {
        const int band = bandIndex(alert.band);
        if (!isValidV1InTheBoxSettings(settings) || !alert.isValid || band < 0 ||
            !validFrequency(static_cast<uint8_t>(band), alert.frequency)) {
            return InTheBoxClassification::Unknown;
        }
        return contains(settings, static_cast<uint8_t>(band), alert.frequency)
                   ? InTheBoxClassification::Inside
                   : InTheBoxClassification::Outside;
    }

    InTheBoxDecision process(const V1InTheBoxSettings& settings, const AlertData* alerts,
                             size_t count, bool laserActive) {
        InTheBoxDecision decision;
        if (!isValidV1InTheBoxSettings(settings) || count > MAX_ALERTS || (count != 0 && !alerts)) {
            return decision;
        }

        std::array<Encounter, MAX_ALERTS> current{};
        size_t currentCount = 0;
        bool uncertainTable = false;
        decision.allOutsideMuteEligible = count != 0 && !laserActive;
        for (size_t i = 0; i < count; ++i) {
            const auto classification = classify(settings, alerts[i]);
            if (classification == InTheBoxClassification::Unknown) {
                decision.allOutsideMuteEligible = false;
                uncertainTable = true;
                continue;
            }
            const auto band = static_cast<uint8_t>(bandIndex(alerts[i].band));
            const bool inside = classification == InTheBoxClassification::Inside;
            decision.anyInside |= inside;
            decision.allOutsideMuteEligible &= !inside && settings.bands[band].muteOutside;
            current[currentCount++] = {alerts[i].frequency, band, false};
        }
        // Unknown rows cannot prove an encounter ended or that another is new.
        // Retain history through uncertainty, so a temporarily undecodable row
        // cannot undo a later manual mute when its frequency becomes usable.
        if (uncertainTable) return decision;

        // Canonical order makes matching independent of packet-table assignment,
        // priority, direction, strength, and row order. No protocol encounter ID
        // exists here: same-band sources within 5 MHz can be indistinguishable.
        for (size_t i = 1; i < currentCount; ++i) {
            const Encounter item = current[i];
            size_t j = i;
            while (j > 0 && comesBefore(item, current[j - 1])) {
                current[j] = current[j - 1];
                --j;
            }
            current[j] = item;
        }

        std::array<int8_t, MAX_ALERTS> previousToCurrent{};
        previousToCurrent.fill(-1);
        for (size_t i = 0; i < currentCount; ++i) {
            uint16_t visited = 0;
            matchEncounter(i, current, previousToCurrent, visited);
        }
        for (size_t i = 0; i < encounterCount_; ++i) {
            if (previousToCurrent[i] >= 0) {
                current[static_cast<size_t>(previousToCurrent[i])].insideSeen = encounters_[i].insideSeen;
            }
        }
        for (size_t i = 0; i < currentCount; ++i) {
            auto& encounter = current[i];
            if (contains(settings, encounter.band, encounter.frequencyMHz)) {
                decision.newInsideUnmute |= !encounter.insideSeen && settings.bands[encounter.band].unmuteInside;
                encounter.insideSeen = true;
            }
        }
        // Keep the latest MHz, so gradual jitter follows the encounter. Once
        // inside, leaving/re-entering a box does not generate another unmute.
        // An absent row ends its encounter; a later appearance is new. Extra
        // simultaneous rows remain distinct even when their frequencies match.
        encounters_ = current;
        encounterCount_ = currentCount;
        return decision;
    }

    void reset() {
        encounters_ = {};
        encounterCount_ = 0;
    }

  private:
    struct Encounter {
        uint32_t frequencyMHz = 0;
        uint8_t band = 0;
        bool insideSeen = false;
    };

    std::array<Encounter, MAX_ALERTS> encounters_{};
    size_t encounterCount_ = 0;

    static int bandIndex(Band band) {
        switch (band) {
        case BAND_X: return 0;
        case BAND_KU: return 1;
        case BAND_K: return 2;
        case BAND_KA: return 3;
        default: return -1;
        }
    }

    static bool contains(const V1InTheBoxSettings& settings, uint8_t band, uint32_t frequencyMHz) {
        const size_t end = band == 3 ? settings.boxes.size() : static_cast<size_t>(band + 1);
        for (size_t i = band; i < end; ++i) {
            const auto& box = settings.boxes[i];
            if (box.enabled && frequencyMHz >= box.lowerMHz && frequencyMHz <= box.upperMHz) return true;
        }
        return false;
    }

    static bool validFrequency(uint8_t band, uint32_t frequencyMHz) {
        // Gen2 band edges from VR's V1FrequencyInfo; values outside the
        // physical band are unknown data, not a reason to request muting.
        constexpr uint32_t lowerMHz[] = {10500, 13400, 23900, 33400};
        constexpr uint32_t upperMHz[] = {10550, 13500, 24250, 36000};
        return frequencyMHz >= lowerMHz[band] && frequencyMHz <= upperMHz[band];
    }

    static bool comesBefore(const Encounter& a, const Encounter& b) {
        return a.band < b.band || (a.band == b.band && a.frequencyMHz < b.frequencyMHz);
    }

    // Nearest-first, one-to-one matching. An augmenting path preserves an old
    // encounter when a greedy closest choice would unnecessarily leave another
    // row unmatched. At most 15 old rows are visited per attempt; no allocation.
    bool matchEncounter(size_t currentIndex, const std::array<Encounter, MAX_ALERTS>& current,
                        std::array<int8_t, MAX_ALERTS>& previousToCurrent, uint16_t& visited) const {
        for (uint32_t distance = 0; distance <= ENCOUNTER_TOLERANCE_MHZ; ++distance) {
            for (size_t i = 0; i < encounterCount_; ++i) {
                const uint16_t bit = static_cast<uint16_t>(1U << i);
                if ((visited & bit) != 0 || encounters_[i].band != current[currentIndex].band) continue;
                const uint32_t oldMHz = encounters_[i].frequencyMHz;
                const uint32_t newMHz = current[currentIndex].frequencyMHz;
                const uint32_t delta = oldMHz > newMHz ? oldMHz - newMHz : newMHz - oldMHz;
                if (delta != distance) continue;
                visited |= bit;
                if (previousToCurrent[i] < 0 ||
                    matchEncounter(static_cast<size_t>(previousToCurrent[i]), current, previousToCurrent, visited)) {
                    previousToCurrent[i] = static_cast<int8_t>(currentIndex);
                    return true;
                }
            }
        }
        return false;
    }
};

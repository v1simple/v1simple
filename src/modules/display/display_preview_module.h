#pragma once

#include <Arduino.h>
#include "packet_parser.h" // For AlertData, Band, Direction
#include "display.h"       // For V1Display, DisplayState

/**
 * DisplayPreviewModule - comprehensive display test / color preview
 *
 * Replaces the original 5-step color swatch with a realistic display test
 * that exercises every visual element on screen:
 *
 * Phase 1: Band + direction sweep
 *   Each band cycles front → side → rear with realistic frequencies
 *   and ramping signal strength. Covers X, K, Ka (3 freqs), Laser.
 *
 * Phase 2: Multi-alert combos with priority-only visuals
 *   Realistic multi-bogey scenarios showing the priority alert only.
 *   Includes Photo radar display.
 *
 * Phase 3: ALP state cycling
 *   Simulates all ALP badge states (OFF → IDLE → LISTENING with PDC/DLI/LID
 *   heartbeat sub-states → ALERT_ACTIVE → NOISE_WINDOW → TEARDOWN) with gun
 *   abbreviation frequency override.
 *
 * Phase 4: Status indicator cycling
 *   Bogey counter chars, mode chars, mute, OBD states, BLE proxy,
 *   volume levels, profile slots.
 *
 * Responsibilities:
 * - Run the timed preview sequence
 * - Expose simple control (start/cancel) and completion flag
 *
 * Does NOT:
 * - Own display connection (expects V1Display pointer)
 * - Touch real runtime modules (uses preview setters for ALP/OBD)
 */
class DisplayPreviewModule {
  public:
    DisplayPreviewModule();

    void begin(V1Display* display);

    // Start/stop preview
    void requestHold(uint32_t durationMs);
    void cancel();

    // State queries
    bool isRunning() const { return previewActive_ || visualPinned_; }
    // Preview continues to own presentation until DisplayRestoreModule
    // consumes the ended edge and restores the authoritative runtime owner.
    bool ownsPresentation() const { return isRunning() || previewEnded_; }
    // Returns true once when a preview has ended (cancel or elapsed), then resets the flag
    bool consumeEnded();

    // Drive the preview; call from loop()
    void update();

    // ── Step table definition ────────────────────────────────────────

    // Flags for per-step indicator overrides
    static constexpr uint8_t FLAG_NONE = 0;
    static constexpr uint8_t FLAG_MUTED = (1 << 0);
    static constexpr uint8_t FLAG_PHOTO = (1 << 1);
    static constexpr uint8_t FLAG_FLASH_ARROW = (1 << 2);
    static constexpr uint8_t FLAG_FLASH_BAND = (1 << 3);

    struct PreviewStep {
        // Primary alert
        Band band;
        Direction dir;
        uint32_t freqMHz;
        uint8_t frontBars;
        uint8_t rearBars;
        uint8_t flags; // FLAG_* bitfield

        // Secondary alert context — band BAND_NONE means no secondary
        Band secBand;
        Direction secDir;
        uint32_t secFreqMHz;
        uint8_t secFrontBars;
        uint8_t secRearBars;

        // Third alert context — band BAND_NONE means none
        Band thirdBand;
        Direction thirdDir;
        uint32_t thirdFreqMHz;
        uint8_t thirdFrontBars;
        uint8_t thirdRearBars;

        // Status overrides (-1 = don't change, use previous)
        int8_t bogeyChar;   // Character for bogey counter (-1 = auto from alert count)
        int8_t modeChar;    // 'A', 'l', 'L', 0=none, -1=don't change
        int8_t profileSlot; // 0-2 or -1=don't change

        // ALP simulation: alpState < 0 means don't change
        int8_t alpState;   // AlpState enum cast, -1=don't change
        int8_t alpHbByte1; // Heartbeat byte1 for LISTENING sub-state

        // OBD simulation: obdState < 0 means don't change
        // 0=off, 1=connected, 2=scanning, -1=don't change
        int8_t obdState;

        // BLE proxy: bleState < 0 means don't change
        // 0=off, 1=advertising, 2=connected, -1=don't change
        int8_t bleState;

        // Volume override: < 0 means don't change
        int8_t mainVolume;
        int8_t muteVolume;

        // ALP gun abbreviation (nullptr = no override / clear)
        const char* alpGunAbbrev;
    };

    struct PreviewCarryState {
        int8_t modeChar = 0;
        int8_t profileSlot = 0;
        int8_t alpState = 0;
        int8_t alpHbByte1 = 0;
        int8_t obdState = 0;
        int8_t bleState = 0;
        int8_t mainVolume = 5;
        int8_t muteVolume = 0;
    };

    struct ResolvedAlert {
        bool present = false;
        Band band = BAND_NONE;
        Direction dir = DIR_NONE;
        uint32_t freqMHz = 0;
        uint8_t frontBars = 0;
        uint8_t rearBars = 0;
        uint8_t cardBarCount = 0;
        char frequencyText[16] = "";
    };

    struct ResolvedStatus {
        char bogeyChar = '0';
        char modeChar = 0;
        bool hasMode = false;
        int8_t profileSlot = 0;
        int8_t alpState = 0;
        uint8_t alpHbByte1 = 0;
        int8_t obdState = 0;
        int8_t bleState = 0;
        uint8_t mainVolume = 5;
        uint8_t muteVolume = 0;
    };

    struct ResolvedStep {
        int index = -1;
        PreviewStep raw{};
        ResolvedAlert primary{};
        ResolvedAlert secondary{};
        ResolvedAlert third{};
        ResolvedStatus status{};
        uint8_t flags = 0;
        uint8_t alertCount = 0;
        uint8_t activeBandMask = 0;
        uint8_t activeDirectionMask = 0;
        uint8_t flashMask = 0;
        uint8_t bandFlashMask = 0;
        uint8_t mainMeterCount = 0;
        bool muted = false;
        bool photo = false;
    };

    static int stepCount();
    static bool rawStep(int index, PreviewStep& out);
    static bool resolveStep(int index, ResolvedStep& out);
    static void resetCarryState(PreviewCarryState& state);
    static void applyCarryState(PreviewCarryState& state, const PreviewStep& step);

    bool pinStep(int index, bool clear, uint32_t* renderSeqOut = nullptr);
    void clearVisualPin();
    bool isVisualPinned() const { return visualPinned_; }
    int pinnedStep() const { return visualPinned_ ? visualPinnedStep_ : -1; }
    uint32_t pinnedRenderSeq() const { return visualPinnedRenderSeq_; }

  private:
    static const PreviewStep STEPS[];
    static const int STEP_COUNT;

    static constexpr uint32_t STEP_DURATION_MS = 2000; // 2 seconds per step
    static constexpr uint32_t PREVIEW_TAIL_MS = 600;   // Extra time after last step

    V1Display* display_ = nullptr;

    bool previewActive_ = false;
    bool previewEnded_ = false;
    unsigned long previewStartMs_ = 0;
    unsigned long previewDurationMs_ = 0;
    bool loopSequence_ = false;
    int previewStep_ = 0;
    bool visualPinned_ = false;
    int visualPinnedStep_ = -1;
    uint32_t visualPinnedRenderSeq_ = 0;

    // Carry-forward state for indicators that persist across steps
    int8_t currentModeChar_ = 0;
    int8_t currentProfileSlot_ = 0;
    int8_t currentAlpState_ = 0; // AlpState::OFF
    int8_t currentAlpHb_ = 0;
    int8_t currentObdState_ = 0;
    int8_t currentBleState_ = 0;
    int8_t currentMainVol_ = 5;
    int8_t currentMuteVol_ = 0;

    PreviewCarryState currentCarryState() const;
    void applyCarryState(const PreviewStep& step);
    void renderStep(int stepIndex, bool firstFrame);
    void renderResolvedStep(const ResolvedStep& resolved, bool firstFrame);
    void resetCarryState();
    void cleanupPreviewOverrides();
};

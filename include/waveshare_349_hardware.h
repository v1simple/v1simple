#pragma once

#include <cstdint>

#include "waveshare_349_revision.h"

namespace waveshare_349 {

class Hardware {
  public:
    static constexpr uint8_t kV1BacklightGpio = 8;
    static constexpr uint8_t kV2BacklightGpio = 42;
    static constexpr uint8_t kGpioResetOrTe = 21;
    static constexpr uint8_t kExioBacklightEnable = 1;
    static constexpr uint8_t kExioResetOrTe = 5;

    // Called before serial or any other board setup. All swapped candidates
    // remain electrically passive until complementary detection completes.
    void prepareEarlyCandidates();

    // Called after the shared TCA9554 bus and power latch are initialized.
    // EXIO1 is forced low before the full complementary detector runs.
    bool detectAndConfigure();

    Revision revision() const { return revision_; }
    const RevisionEvidence& evidence() const { return evidence_; }
    bool ready() const { return revision_ != Revision::Unknown && expanderReady_; }
    int backlightGpio() const;

    bool resetPanel();
    void setBrightness(uint8_t level);
    void forceBacklightOff();
    void holdBacklightForDeepSleep();
    void releaseBacklightSleepHold();

  private:
    // Sixteen samples span multiple observed TE periods on V2 while avoiding a
    // multi-second boot penalty from this board's relatively slow input-port
    // reads.
    static constexpr uint16_t kDetectionSamples = 16;

    void leaveSwappedCandidatesAsInputs();
    bool setExpanderOutput(uint8_t pin, bool high, bool makeOutput);
    bool setExpanderPinLevel(uint8_t pin, bool high);
    bool configureDetectedRevision();

    Revision revision_ = Revision::Unknown;
    RevisionEvidence evidence_{};
    bool expanderReady_ = false;
};

Hardware& hardware();

} // namespace waveshare_349

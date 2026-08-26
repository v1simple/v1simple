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

    // Called before serial or any other board setup. All swapped candidates are
    // first released to inputs; a stable GPIO21 idle level may then darken only
    // the corresponding probable PWM pin while the expander comes online.
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
    // The early GPIO-only sample window must be longer than the observed V2 TE
    // HIGH pulse so that it can never look like V1's continuously HIGH reset.
    static constexpr uint16_t kEarlyDetectionSamples = 128;
    // Sixteen samples span multiple observed TE periods on V2 while avoiding a
    // multi-second boot penalty from this board's relatively slow input-port
    // reads.
    static constexpr uint16_t kDetectionSamples = 16;

    void leaveSwappedCandidatesAsInputs();
    bool setExpanderOutput(uint8_t pin, bool high, bool makeOutput);
    bool setExpanderPinLevel(uint8_t pin, bool high);
    bool configureDetectedRevision();

    Revision provisionalRevision_ = Revision::Unknown;
    Revision revision_ = Revision::Unknown;
    RevisionEvidence evidence_{};
    bool expanderReady_ = false;
};

Hardware& hardware();

} // namespace waveshare_349

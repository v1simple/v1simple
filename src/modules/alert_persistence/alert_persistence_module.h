// Alert Persistence Module - Header
// Handles V1 radar alert display persistence

#pragma once

#include "packet_parser.h" // For AlertData type

// Holds cleared V1 alerts for display persistence and clears combined state.
class AlertPersistenceModule {
  public:
    // Alert persistence - shows last alert briefly after V1 clears it
    void setPersistedAlert(const AlertData& alert);
    void startPersistence(unsigned long now);
    void clearPersistence();
    bool shouldShowPersisted(unsigned long now, unsigned long persistMs) const;
    const AlertData& getPersistedAlert() const { return persistedAlert_; }
    bool isPersistenceActive() const { return alertPersistenceActive_; }

  private:
    // Alert persistence state
    AlertData persistedAlert_;
    unsigned long alertClearedTime_ = 0;
    bool alertPersistenceActive_ = false;
};

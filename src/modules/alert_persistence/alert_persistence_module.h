// Alert Persistence Module - Header
// Handles V1 radar alert display persistence

#pragma once

#include <Arduino.h>
#include "packet_parser.h"  // For AlertData type

// Forward declarations
class V1BLEClient;
class V1Display;
class SettingsManager;
class PacketParser;

/**
 * AlertPersistenceModule - V1 alert persistence and cleanup
 *
 * Responsibilities:
 * - Alert persistence (shows faded alert after V1 clears it)
 * - Combined state clearing
 */
class AlertPersistenceModule {
public:
    AlertPersistenceModule();

    // Initialize with dependencies (call from setup())
    void begin(V1BLEClient* ble, PacketParser* parser, V1Display* display, SettingsManager* settings);

    // Alert persistence - shows last alert briefly after V1 clears it
    void setPersistedAlert(const AlertData& alert);
    void startPersistence(unsigned long now);
    void clearPersistence();
    bool shouldShowPersisted(unsigned long now, unsigned long persistMs) const;
    const AlertData& getPersistedAlert() const { return persistedAlert_; }
    bool isPersistenceActive() const { return alertPersistenceActive_; }

private:
    // Dependencies
    V1BLEClient* bleClient_ = nullptr;
    PacketParser* parser_ = nullptr;
    V1Display* display_ = nullptr;
    SettingsManager* settings_ = nullptr;

    // Alert persistence state
    AlertData persistedAlert_;
    unsigned long alertClearedTime_ = 0;
    bool alertPersistenceActive_ = false;
};

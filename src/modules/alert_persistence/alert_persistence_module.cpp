// Alert Persistence Module - Implementation
// Handles V1 radar alert display state and persistence

#include "alert_persistence_module.h"

#include "ble_client.h"
#include "display.h"
#include "packet_parser.h"
#include "settings.h"

AlertPersistenceModule::AlertPersistenceModule() {
    // Dependencies set in begin()
}

void AlertPersistenceModule::begin(V1BLEClient* ble, PacketParser* pParser, V1Display* disp,
                                   SettingsManager* settings) {
    bleClient_ = ble;
    parser_ = pParser;
    display_ = disp;
    settings_ = settings;

    Serial.println("[AlertPersistenceModule] Initialized");
}

// ============================================================================
// Alert Persistence - shows last alert briefly after V1 clears it
// ============================================================================

void AlertPersistenceModule::setPersistedAlert(const AlertData& alert) {
    persistedAlert_ = alert;
    alertPersistenceActive_ = false;
    alertClearedTime_ = 0;
}

void AlertPersistenceModule::startPersistence(unsigned long now) {
    if (!persistedAlert_.isValid) {
        // Caller invoked startPersistence() with no valid alert latched.
        // Display pipeline already guards this; any nonzero count here is
        // a call-site regression.
        return;
    }
    if (alertClearedTime_ != 0) {
        // Persistence window already in flight. Expected high-frequency
        // no-op — renderIdleOwner() calls us every idle tick during a
        // window and we idempotently ignore repeats.
        return;
    }
    alertClearedTime_ = now;
    alertPersistenceActive_ = true;
}

void AlertPersistenceModule::clearPersistence() {
    persistedAlert_ = AlertData();
    alertPersistenceActive_ = false;
    alertClearedTime_ = 0;
}

bool AlertPersistenceModule::shouldShowPersisted(unsigned long now, unsigned long persistMs) const {
    return alertPersistenceActive_ && (now - alertClearedTime_) < persistMs;
}

// Mock alert_persistence_module.h for native unit testing
#pragma once

#include "packet_parser.h"

class AlertPersistenceModule {
public:
    AlertData persistedAlert;
    unsigned long persistenceStartMs = 0;
    bool persistenceActive = false;
    
    // Call tracking
    int setPersistedAlertCalls = 0;
    int clearAllAlertStateCalls = 0;
    int clearPersistenceCalls = 0;
    int startPersistenceCalls = 0;
    int shouldShowPersistedCalls = 0;
    
    void reset() {
        persistedAlert = AlertData{};
        persistenceStartMs = 0;
        persistenceActive = false;
        setPersistedAlertCalls = 0;
        clearAllAlertStateCalls = 0;
        clearPersistenceCalls = 0;
        startPersistenceCalls = 0;
        shouldShowPersistedCalls = 0;
    }
    
    // Template method accepts any AlertData-like struct
    template<typename T>
    void setPersistedAlert(const T& alert) {
        setPersistedAlertCalls++;
        persistedAlert.band = alert.band;
        persistedAlert.direction = alert.direction;
        persistedAlert.frontStrength = alert.frontStrength;
        persistedAlert.rearStrength = alert.rearStrength;
        persistedAlert.frequency = alert.frequency;
        persistedAlert.isValid = alert.isValid;
        persistedAlert.isPriority = alert.isPriority;
    }
    
    const AlertData& getPersistedAlert() const {
        return persistedAlert;
    }
    
    void clearAllAlertState() {
        clearAllAlertStateCalls++;
    }
    
    void clearPersistence() {
        clearPersistenceCalls++;
        persistenceActive = false;
    }
    
    void startPersistence(unsigned long nowMs) {
        startPersistenceCalls++;
        if (!persistenceActive) {
            persistenceStartMs = nowMs;
            persistenceActive = true;
        }
    }
    
    bool shouldShowPersisted(unsigned long nowMs, unsigned long durationMs) {
        shouldShowPersistedCalls++;
        if (!persistenceActive || !persistedAlert.isValid) return false;
        return (nowMs - persistenceStartMs) < durationMs;
    }
};

// Mock ble_queue_module.h for native unit testing
#pragma once

#include <cstdint>

class BleQueueModule {
public:
    unsigned long lastRxMillis = 0;
    uint32_t lastParsedTimestamp = 0;
    bool backpressured = false;
    bool parsedFlag = false;
    bool sessionOpen = true;
    uint32_t sessionGeneration = 0;
    int openSessionCalls = 0;
    int closeSessionCalls = 0;
    
    void reset() {
        lastRxMillis = 0;
        lastParsedTimestamp = 0;
        backpressured = false;
        parsedFlag = false;
        sessionOpen = true;
        sessionGeneration = 0;
        openSessionCalls = 0;
        closeSessionCalls = 0;
    }
    
    void setLastRxMillis(unsigned long ms) {
        lastRxMillis = ms;
    }
    
    unsigned long getLastRxMillis() const {
        return lastRxMillis;
    }

    void setLastParsedTimestamp(uint32_t tsMs) {
        lastParsedTimestamp = tsMs;
    }

    uint32_t getLastParsedTimestamp() const {
        return lastParsedTimestamp;
    }

    void setBackpressured(bool isBackpressured) {
        backpressured = isBackpressured;
    }

    bool isBackpressured() const {
        return backpressured;
    }

    void setParsedFlag(bool value) {
        parsedFlag = value;
    }

    bool consumeParsedFlag() {
        bool had = parsedFlag;
        parsedFlag = false;
        return had;
    }

    void openSession(uint32_t generation) {
        openSessionCalls++;
        sessionOpen = true;
        sessionGeneration = generation;
    }

    void closeSession() {
        closeSessionCalls++;
        sessionOpen = false;
        sessionGeneration = 0;
        lastRxMillis = 0;
        lastParsedTimestamp = 0;
        parsedFlag = false;
        backpressured = false;
    }
};

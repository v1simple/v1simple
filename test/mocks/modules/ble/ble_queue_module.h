// Mock ble_queue_module.h for native unit testing
#pragma once

#include <cstdint>

class BleQueueModule {
public:
    unsigned long lastRxMillis = 0;
    uint32_t lastParsedTimestamp = 0;
    bool backpressured = false;
    bool parsedFlag = false;
    
    void reset() {
        lastRxMillis = 0;
        lastParsedTimestamp = 0;
        backpressured = false;
        parsedFlag = false;
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
};

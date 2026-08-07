// Mock power_module.h for native unit testing
#pragma once

class PowerModule {
public:
    int onV1ConnectionChangeCalls = 0;
    bool lastConnectionState = false;
    int onV1DataReceivedCalls = 0;
    
    void reset() {
        onV1ConnectionChangeCalls = 0;
        lastConnectionState = false;
        onV1DataReceivedCalls = 0;
    }
    
    void onV1ConnectionChange(bool connected) {
        onV1ConnectionChangeCalls++;
        lastConnectionState = connected;
    }

    void onV1DataReceived() {
        onV1DataReceivedCalls++;
    }
};

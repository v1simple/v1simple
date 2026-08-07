#pragma once

class DisplayRestoreModule {
public:
    int processCalls = 0;

    void process() { processCalls++; }
};

#pragma once

class DisplayPreviewModule {
public:
    bool running = false;
    bool ended = false;
    int updateCalls = 0;
    int cancelCalls = 0;

    bool isRunning() const { return running; }
    void setRunning(bool value) { running = value; }
    void setEnded(bool value) { ended = value; }

    void update() { updateCalls++; }
    bool consumeEnded() {
        const bool value = ended;
        ended = false;
        return value;
    }
    void cancel() {
        cancelCalls++;
        running = false;
    }
};

#pragma once
#ifndef TOUCH_HANDLER_H
#define TOUCH_HANDLER_H

#include <cstdint>
#include <deque>

class TouchHandler {
public:
    struct TouchPoint {
        int16_t x = 0;
        int16_t y = 0;
        bool active = false;
    };

    bool begin(int = 17, int = 18, uint8_t = 0x3B, int = -1) { return true; }
    bool isTouched() const { return !touches_.empty() && touches_.front().active; }

    bool getTouchPoint(int16_t& x, int16_t& y) {
        ++getTouchPointCalls;
        if (touches_.empty()) {
            return false;
        }

        const TouchPoint point = touches_.front();
        touches_.pop_front();
        if (!point.active) {
            return false;
        }

        x = point.x;
        y = point.y;
        return true;
    }

    void queueTouch(int16_t x, int16_t y) {
        touches_.push_back(TouchPoint{x, y, true});
    }

    void queueNoTouch() {
        touches_.push_back(TouchPoint{0, 0, false});
    }

    void reset() {
        touches_.clear();
        getTouchPointCalls = 0;
    }

    int getTouchPointCalls = 0;

private:
    std::deque<TouchPoint> touches_;
};

#endif  // TOUCH_HANDLER_H

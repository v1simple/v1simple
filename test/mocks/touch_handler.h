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

    // Mirrors the real driver's contract: edge-triggered. Returns true once
    // per NEW tap; while the finger stays down (consecutive active points) it
    // returns false and only the level state (isTouchActive) stays true.
    bool getTouchPoint(int16_t& x, int16_t& y) {
        ++getTouchPointCalls;
        if (touches_.empty()) {
            // No touch data this poll: finger is up (real driver noteNoTouch).
            touchActive_ = false;
            return false;
        }

        const TouchPoint point = touches_.front();
        touches_.pop_front();
        if (!point.active) {
            touchActive_ = false;
            return false;
        }

        const bool newTap = !touchActive_;
        touchActive_ = true;
        if (!newTap) {
            return false;  // Touch held, not a new tap (matches real driver)
        }

        x = point.x;
        y = point.y;
        return true;
    }

    // Level state, matching src/touch_handler.h.
    bool isTouchActive() const { return touchActive_; }

    void queueTouch(int16_t x, int16_t y) {
        touches_.push_back(TouchPoint{x, y, true});
    }

    void queueNoTouch() {
        touches_.push_back(TouchPoint{0, 0, false});
    }

    void reset() {
        touches_.clear();
        getTouchPointCalls = 0;
        touchActive_ = false;
    }

    int getTouchPointCalls = 0;

private:
    std::deque<TouchPoint> touches_;
    bool touchActive_ = false;
};

#endif  // TOUCH_HANDLER_H

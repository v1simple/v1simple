#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
constexpr int SERIAL_8N1 = 0;
inline unsigned long mockMillis = 1;
inline unsigned long millis() { return mockMillis; }
class FakeUart {
    std::deque<char> queue_;

  public:
    bool enabled = false;
    void setRxBufferSize(size_t) {}
    void begin(uint32_t, int, int, int) { enabled = true; }
    void end() {
        enabled = false;
        queue_.clear();
    }
    int available() const { return static_cast<int>(queue_.size()); }
    int read() {
        if (queue_.empty())
            return -1;
        char c = queue_.front();
        queue_.pop_front();
        return static_cast<unsigned char>(c);
    }
    void receive(const std::string &data) {
        for (char c : data)
            queue_.push_back(c);
    }
    void reset() { queue_.clear(); }
};
inline FakeUart Serial1;

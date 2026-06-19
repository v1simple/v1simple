// Mock Arduino.h for native unit testing
// Provides minimal type definitions needed by application code
#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cmath>
#include <string>
#include <algorithm>
#include <cctype>
#include "pgmspace.h"

// Basic Arduino types
typedef uint8_t byte;
typedef bool boolean;

// String class stub (minimal implementation)
class String {
public:
    String() : data_("") {}
    String(const char* s) : data_(s ? s : "") {}
    String(const std::string& s) : data_(s) {}
    String(int val) : data_(std::to_string(val)) {}
    String(unsigned int val) : data_(std::to_string(val)) {}
    String(long val) : data_(std::to_string(val)) {}
    String(unsigned long val) : data_(std::to_string(val)) {}
    String(long long val) : data_(std::to_string(val)) {}
    String(unsigned long long val) : data_(std::to_string(val)) {}
    String(float val, int decimals = 2) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.*f", decimals, val);
        data_ = buf;
    }
    
    const char* c_str() const { return data_.c_str(); }
    size_t length() const { return data_.length(); }
    bool isEmpty() const { return data_.empty(); }
    void reserve(size_t capacity) { data_.reserve(capacity); }

    char operator[](size_t index) const { return data_[index]; }
    char& operator[](size_t index) { return data_[index]; }
    char charAt(size_t index) const { return index < data_.size() ? data_[index] : '\0'; }

    String& operator+=(const String& other) { data_ += other.data_; return *this; }
    String& operator+=(const char* s) { if(s) data_ += s; return *this; }
    String& operator+=(char c) { data_ += c; return *this; }
    String operator+(const String& other) const { return String(data_ + other.data_); }
    friend String operator+(const char* lhs, const String& rhs) {
        return String(std::string(lhs ? lhs : "") + rhs.data_);
    }
    bool operator==(const String& other) const { return data_ == other.data_; }
    bool operator==(const char* s) const { return data_ == (s ? s : ""); }
    bool operator!=(const String& other) const { return data_ != other.data_; }
    bool operator!=(const char* s) const { return !(*this == s); }
    bool operator<(const String& other) const { return data_ < other.data_; }
    bool operator>(const String& other) const { return data_ > other.data_; }
    bool operator<=(const String& other) const { return data_ <= other.data_; }
    bool operator>=(const String& other) const { return data_ >= other.data_; }

    void toLowerCase() {
        std::transform(data_.begin(),
                       data_.end(),
                       data_.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    }

    void toUpperCase() {
        std::transform(data_.begin(),
                       data_.end(),
                       data_.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    }

    String substring(size_t from, size_t to) const {
        if (from >= data_.size() || to <= from) {
            return String("");
        }
        size_t count = std::min(to, data_.size()) - from;
        return String(data_.substr(from, count));
    }

    String substring(size_t from) const {
        if (from >= data_.size()) {
            return String("");
        }
        return String(data_.substr(from));
    }

    int lastIndexOf(char needle) const {
        const std::size_t pos = data_.find_last_of(needle);
        return pos == std::string::npos ? -1 : static_cast<int>(pos);
    }

    int indexOf(char needle) const {
        const std::size_t pos = data_.find(needle);
        return pos == std::string::npos ? -1 : static_cast<int>(pos);
    }

    int indexOf(const char* needle) const {
        const std::string query = needle ? needle : "";
        const std::size_t pos = data_.find(query);
        return pos == std::string::npos ? -1 : static_cast<int>(pos);
    }

    bool startsWith(const char* prefix) const {
        const std::string needle = prefix ? prefix : "";
        return data_.rfind(needle, 0) == 0;
    }

    bool endsWith(const char* suffix) const {
        const std::string needle = suffix ? suffix : "";
        if (needle.size() > data_.size()) {
            return false;
        }
        return data_.compare(data_.size() - needle.size(), needle.size(), needle) == 0;
    }

    void replace(const char* find, const char* replaceWith) {
        const std::string needle = find ? find : "";
        const std::string replacement = replaceWith ? replaceWith : "";
        if (needle.empty()) {
            return;
        }
        std::size_t pos = 0;
        while ((pos = data_.find(needle, pos)) != std::string::npos) {
            data_.replace(pos, needle.size(), replacement);
            pos += replacement.size();
        }
    }

    bool equalsIgnoreCase(const String& other) const {
        if (data_.size() != other.data_.size()) {
            return false;
        }
        for (size_t i = 0; i < data_.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(data_[i])) !=
                std::tolower(static_cast<unsigned char>(other.data_[i]))) {
                return false;
            }
        }
        return true;
    }

    void trim() {
        const auto first = std::find_if_not(
            data_.begin(), data_.end(), [](unsigned char c) { return std::isspace(c) != 0; });
        const auto last = std::find_if_not(
            data_.rbegin(), data_.rend(), [](unsigned char c) { return std::isspace(c) != 0; }).base();
        if (first >= last) {
            data_.clear();
            return;
        }
        data_ = std::string(first, last);
    }
    
    int toInt() const { return std::stoi(data_); }
    float toFloat() const { return std::stof(data_); }

    size_t write(const uint8_t* data, size_t length) {
        if (!data || length == 0) {
            return 0;
        }
        data_.append(reinterpret_cast<const char*>(data), length);
        return length;
    }

    size_t write(uint8_t byte) {
        data_.push_back(static_cast<char>(byte));
        return 1;
    }

    int read() const {
        if (read_pos_ >= data_.size()) {
            return -1;
        }
        return static_cast<unsigned char>(data_[read_pos_++]);
    }

    int peek() const {
        if (read_pos_ >= data_.size()) {
            return -1;
        }
        return static_cast<unsigned char>(data_[read_pos_]);
    }
    
private:
    std::string data_;
    mutable size_t read_pos_ = 0;
};

// Serial stub
class SerialClass {
public:
    void begin(unsigned long) {}
    void print(const char*) {}
    void print(int) {}
    void print(float, int = 2) {}
    void println(const char* = "") {}
    void println(int) {}
    void println(float, int = 2) {}
    void printf(const char*, ...) {}
};
extern SerialClass Serial;

// Math functions
#ifndef PI
#define PI 3.14159265358979323846
#endif

#ifndef DEG_TO_RAD
#define DEG_TO_RAD 0.017453292519943295
#endif

#ifndef RAD_TO_DEG
#define RAD_TO_DEG 57.29577951308232
#endif

inline long map(long x, long in_min, long in_max, long out_min, long out_max) {
    return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

template<typename T>
T constrain(T x, T a, T b) {
    return (x < a) ? a : ((x > b) ? b : x);
}

inline long random(long max) { return rand() % max; }
inline long random(long min, long max) { return min + rand() % (max - min); }

// Time functions - use extern variables for test control
extern unsigned long mockMillis;
extern unsigned long mockMicros;
inline unsigned long millis() { return mockMillis; }
inline unsigned long micros() { return mockMicros; }
inline void delay(unsigned long) {}
inline void delayMicroseconds(unsigned int) {}
inline void yield() {}

// GPIO stubs
#define INPUT 0
#define OUTPUT 1
#define OUTPUT_OPEN_DRAIN 3
#define INPUT_PULLUP 2
#define HIGH 1
#define LOW 0

inline void pinMode(int, int) {}
inline void digitalWrite(int, int) {}
inline int digitalRead(int) { return 0; }
inline int analogRead(int) { return 0; }
inline void analogWrite(int, int) {}

// ESP-IDF timer stub — only defined here if a test hasn't already provided it
#ifndef ESP_TIMER_GET_TIME_DEFINED
#define ESP_TIMER_GET_TIME_DEFINED
extern "C" int64_t esp_timer_get_time() { return 0; }
#endif

// ESP object stub
class EspClass {
public:
    void restart() {}
    uint32_t getFreeHeap() { return 320000u; }
    uint32_t getHeapSize() { return 520000u; }
    uint32_t getPsramSize() { return 0u; }
};
inline EspClass ESP;

// Arduino min/max — use namespace injection rather than macros to avoid
// breaking std::min/std::max qualified calls in other headers.
using std::min;
using std::max;

// PSRAM detection helpers
inline bool psramFound() { return false; }

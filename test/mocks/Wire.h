#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

class TwoWire {
public:
    TwoWire() = default;
    explicit TwoWire(int /*port*/) {}
    struct TransmissionRecord {
        uint8_t address = 0;
        std::vector<uint8_t> data;
        bool sendStop = true;
    };

    struct RequestResult {
        std::size_t returnedSize = 0;
        std::vector<uint8_t> data;
    };

    bool begin(int sda, int scl, uint32_t frequency = 0) {
        ++beginCalls;
        lastBeginSda = sda;
        lastBeginScl = scl;
        lastBeginFrequency = frequency;
        started = true;
        return true;
    }

    void setClock(uint32_t clock) {
        lastClock = clock;
    }

    void setTimeOut(uint16_t timeout) {
        lastTimeout = timeout;
    }

    void beginTransmission(uint8_t address) {
        ++beginTransmissionCalls;
        lastTxAddress = address;
        txBuffer.clear();
    }

    std::size_t write(const uint8_t* data, std::size_t length) {
        if (!data) {
            return 0;
        }
        txBuffer.insert(txBuffer.end(), data, data + length);
        return length;
    }

    std::size_t write(uint8_t data) {
        txBuffer.push_back(data);
        return 1;
    }

    uint8_t endTransmission(bool sendStop = true) {
        ++endTransmissionCalls;
        lastSendStop = sendStop;
        transmissionHistory.push_back(TransmissionRecord{lastTxAddress, txBuffer, sendStop});
        if (endTransmissionResults.empty()) {
            return 0;
        }
        const uint8_t result = endTransmissionResults.front();
        endTransmissionResults.pop_front();
        return result;
    }

    std::size_t requestFrom(uint8_t address, uint8_t size) {
        ++requestFromCalls;
        lastRequestAddress = address;
        lastRequestSize = size;
        rxBuffer.clear();
        rxIndex = 0;

        if (requestResults.empty()) {
            rxBuffer.assign(size, 0);
            return size;
        }

        RequestResult result = requestResults.front();
        requestResults.pop_front();
        if (result.data.size() < result.returnedSize) {
            result.data.resize(result.returnedSize, 0);
        }
        rxBuffer = result.data;
        if (rxBuffer.size() > result.returnedSize) {
            rxBuffer.resize(result.returnedSize);
        }
        return result.returnedSize;
    }

    int available() const {
        return rxIndex < rxBuffer.size()
                   ? static_cast<int>(rxBuffer.size() - rxIndex)
                   : 0;
    }

    int read() {
        if (rxIndex >= rxBuffer.size()) {
            return -1;
        }
        return rxBuffer[rxIndex++];
    }

    bool end() {
        ++endCalls;
        started = false;
        return true;
    }

    void queueEndTransmission(uint8_t result) {
        endTransmissionResults.push_back(result);
    }

    void queueRequestFrom(std::size_t returnedSize, const std::vector<uint8_t>& data) {
        requestResults.push_back(RequestResult{returnedSize, data});
    }

    void resetMock() {
        beginCalls = 0;
        beginTransmissionCalls = 0;
        endTransmissionCalls = 0;
        requestFromCalls = 0;
        endCalls = 0;
        lastBeginSda = -1;
        lastBeginScl = -1;
        lastBeginFrequency = 0;
        lastClock = 0;
        lastTimeout = 0;
        lastTxAddress = 0;
        lastRequestAddress = 0;
        lastRequestSize = 0;
        lastSendStop = true;
        txBuffer.clear();
        rxBuffer.clear();
        rxIndex = 0;
        transmissionHistory.clear();
        endTransmissionResults.clear();
        requestResults.clear();
        started = false;
    }

    int beginCalls = 0;
    int beginTransmissionCalls = 0;
    int endTransmissionCalls = 0;
    int requestFromCalls = 0;
    int endCalls = 0;
    int lastBeginSda = -1;
    int lastBeginScl = -1;
    uint32_t lastBeginFrequency = 0;
    uint32_t lastClock = 0;
    uint16_t lastTimeout = 0;
    uint8_t lastTxAddress = 0;
    uint8_t lastRequestAddress = 0;
    uint8_t lastRequestSize = 0;
    bool lastSendStop = true;
    bool started = false;
    std::vector<uint8_t> txBuffer;
    std::vector<TransmissionRecord> transmissionHistory;

private:
    std::deque<uint8_t> endTransmissionResults;
    std::deque<RequestResult> requestResults;
    std::vector<uint8_t> rxBuffer;
    std::size_t rxIndex = 0;
};

inline TwoWire Wire;

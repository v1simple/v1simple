#include "wifi_scan_result_owner.h"

#include <algorithm>
#include <map>

namespace {

void abortActiveScan(const WifiScanResultOwner::Driver& driver) {
    if (driver.abort) {
        driver.abort(driver.ctx);
    } else if (driver.release) {
        driver.release(driver.ctx);
    }
}

} // namespace

size_t WifiScanResultOwner::consumerIndex(WifiScanConsumer consumer) {
    return static_cast<size_t>(consumer);
}

bool WifiScanResultOwner::hasPendingConsumer() const {
    for (size_t index = 0; index < CONSUMER_COUNT; ++index) {
        if (pending_[index] && pendingGeneration_[index] == activeGeneration_) {
            return true;
        }
    }
    return false;
}

void WifiScanResultOwner::clearPendingGeneration(uint32_t generation) {
    for (size_t index = 0; index < CONSUMER_COUNT; ++index) {
        if (pending_[index] && pendingGeneration_[index] == generation) {
            pending_[index] = false;
            pendingGeneration_[index] = 0;
        }
    }
}

WifiScanResultOwner::RequestResult WifiScanResultOwner::request(WifiScanConsumer consumer, const Driver& driver) {
    const size_t index = consumerIndex(consumer);
    clearSnapshot(consumer);

    if (running_) {
        pending_[index] = true;
        pendingGeneration_[index] = activeGeneration_;
        return RequestResult::JOINED;
    }

    if (!driver.start) {
        return RequestResult::FAILED;
    }

    ++generation_;
    if (generation_ == 0) {
        ++generation_;
    }
    activeGeneration_ = generation_;
    pending_[index] = true;
    pendingGeneration_[index] = activeGeneration_;

    if (driver.start(driver.ctx) != driver.runningStatus) {
        abortActiveScan(driver);
        pending_[index] = false;
        pendingGeneration_[index] = 0;
        activeGeneration_ = 0;
        return RequestResult::FAILED;
    }

    running_ = true;
    return RequestResult::STARTED;
}

WifiScanResultOwner::HarvestResult WifiScanResultOwner::harvest(const Driver& driver) {
    if (!running_) {
        return HarvestResult::IDLE;
    }

    if (!driver.status) {
        const uint32_t failedGeneration = activeGeneration_;
        abortActiveScan(driver);
        running_ = false;
        activeGeneration_ = 0;
        clearPendingGeneration(failedGeneration);
        return HarvestResult::FAILED;
    }

    const int16_t resultCount = driver.status(driver.ctx);
    if (resultCount == driver.runningStatus) {
        return HarvestResult::RUNNING;
    }

    const uint32_t completedGeneration = activeGeneration_;
    running_ = false;
    activeGeneration_ = 0;

    if (resultCount < 0 || !driver.ssidAt || !driver.rssiAt || !driver.encryptionAt) {
        abortActiveScan(driver);
        clearPendingGeneration(completedGeneration);
        return HarvestResult::FAILED;
    }

    std::map<String, ScannedNetwork> uniqueNetworks;
    for (int16_t scanIndex = 0; scanIndex < resultCount; ++scanIndex) {
        const String ssid = driver.ssidAt(scanIndex, driver.ctx);
        if (ssid.length() == 0) {
            continue;
        }

        const int32_t rssi = driver.rssiAt(scanIndex, driver.ctx);
        const uint8_t encryptionType = driver.encryptionAt(scanIndex, driver.ctx);
        auto existing = uniqueNetworks.find(ssid);
        if (existing == uniqueNetworks.end() || rssi > existing->second.rssi) {
            uniqueNetworks[ssid] = {ssid, rssi, encryptionType};
        }
    }

    std::vector<ScannedNetwork> networks;
    networks.reserve(uniqueNetworks.size());
    for (const auto& entry : uniqueNetworks) {
        networks.push_back(entry.second);
    }
    std::sort(networks.begin(), networks.end(),
              [](const ScannedNetwork& lhs, const ScannedNetwork& rhs) { return lhs.rssi > rhs.rssi; });

    for (size_t index = 0; index < CONSUMER_COUNT; ++index) {
        if (!pending_[index] || pendingGeneration_[index] != completedGeneration) {
            continue;
        }
        snapshots_[index] = networks;
        snapshotValid_[index] = true;
        snapshotGeneration_[index] = completedGeneration;
        pending_[index] = false;
        pendingGeneration_[index] = 0;
    }

    if (driver.release) {
        driver.release(driver.ctx);
    }
    return HarvestResult::COMPLETED;
}

bool WifiScanResultOwner::cancel(WifiScanConsumer consumer, const Driver& driver) {
    const size_t index = consumerIndex(consumer);
    const bool affected = pending_[index] || snapshotValid_[index];
    pending_[index] = false;
    pendingGeneration_[index] = 0;
    clearSnapshot(consumer);

    if (running_ && !hasPendingConsumer()) {
        abortActiveScan(driver);
        running_ = false;
        activeGeneration_ = 0;
    }
    return affected;
}

void WifiScanResultOwner::reset(const Driver& driver) {
    if (running_) {
        abortActiveScan(driver);
    }

    running_ = false;
    generation_ = 0;
    activeGeneration_ = 0;
    for (size_t index = 0; index < CONSUMER_COUNT; ++index) {
        pending_[index] = false;
        pendingGeneration_[index] = 0;
        snapshotValid_[index] = false;
        snapshotGeneration_[index] = 0;
        snapshots_[index].clear();
    }
}

bool WifiScanResultOwner::isPending(WifiScanConsumer consumer) const {
    return pending_[consumerIndex(consumer)];
}

bool WifiScanResultOwner::hasSnapshot(WifiScanConsumer consumer) const {
    return snapshotValid_[consumerIndex(consumer)];
}

uint32_t WifiScanResultOwner::snapshotGeneration(WifiScanConsumer consumer) const {
    return snapshotGeneration_[consumerIndex(consumer)];
}

std::vector<ScannedNetwork> WifiScanResultOwner::copySnapshot(WifiScanConsumer consumer) const {
    const size_t index = consumerIndex(consumer);
    if (!snapshotValid_[index]) {
        return {};
    }
    return snapshots_[index];
}

void WifiScanResultOwner::clearSnapshot(WifiScanConsumer consumer) {
    const size_t index = consumerIndex(consumer);
    snapshots_[index].clear();
    snapshotValid_[index] = false;
    snapshotGeneration_[index] = 0;
}

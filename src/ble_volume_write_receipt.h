#pragma once

#include <cstdint>

// A rejection identifies a request ID, not a user intent. Only a rejection
// received after the latest successful volume write can authorize its retry.
// New writers and detector sessions retire that authority.
class BleVolumeWriteReceipt {
  public:
    void sent(uint32_t session, uint32_t ingressBoundary) {
        if (++revision_ == 0) ++revision_;
        session_ = session;
        ingressBoundary_ = ingressBoundary;
        rejected_ = false;
    }
    void reject(uint8_t packetId, uint32_t session, uint32_t ingress) {
        if (packetId == 0x39 && revision_ != 0 && session == session_ && ingress != 0 &&
            static_cast<int32_t>(ingress - ingressBoundary_) > 0) rejected_ = true;
    }
    uint32_t revision() const { return revision_; }
    bool rejected(uint32_t revision, uint32_t session) const {
        return revision != 0 && revision == revision_ && session == session_ && rejected_;
    }

  private:
    uint32_t revision_ = 0;
    uint32_t session_ = 0;
    uint32_t ingressBoundary_ = 0;
    bool rejected_ = false;
};

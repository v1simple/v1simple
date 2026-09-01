#pragma once

#include <cstdint>

namespace WifiMaintenanceInterfacePolicy {

// IPv4 values use the same uint32_t representation supplied by IPAddress.
// A zero AP address means the admission boundary has not been initialized and
// therefore fails closed.
inline bool hasAddressCollision(const uint32_t maintenanceApIp, const uint32_t liveStaIp) {
    return maintenanceApIp != 0 && liveStaIp != 0 && maintenanceApIp == liveStaIp;
}

inline bool allows(const uint32_t acceptedLocalIp, const uint32_t maintenanceApIp, const uint32_t liveStaIp) {
    // When AP and STA have the same address, getsockname() cannot identify the
    // ingress interface. Fail closed for every socket until STA is removed.
    return !hasAddressCollision(maintenanceApIp, liveStaIp) && maintenanceApIp != 0 &&
           (acceptedLocalIp == maintenanceApIp || (liveStaIp != 0 && acceptedLocalIp == liveStaIp));
}

} // namespace WifiMaintenanceInterfacePolicy

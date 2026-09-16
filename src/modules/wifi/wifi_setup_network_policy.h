#pragma once

namespace WifiSetupNetworkPolicy {

enum class SavedNetworkStart {
    None,
    DirectConnect,
    MaintenanceAutoConnect,
};

// Maintenance may use STA for saved-network testing and auto-join, while HTTP
// ingress independently admits only sockets accepted through approved IPs.
inline bool usesSta(const bool savedStaAvailable) {
    return savedStaAvailable;
}

inline SavedNetworkStart selectSavedNetworkStart(const bool maintenanceBootMode, const bool savedStaAvailable) {
    if (!savedStaAvailable) {
        return SavedNetworkStart::None;
    }
    return maintenanceBootMode ? SavedNetworkStart::MaintenanceAutoConnect : SavedNetworkStart::DirectConnect;
}

template <typename Resolver, typename Retry, typename Start>
inline bool startMaintenanceAutoConnect(Resolver&& resolve, Retry&& retry, Start&& start) {
    if (!resolve()) {
        retry();
        return false;
    }
    return start();
}

} // namespace WifiSetupNetworkPolicy

#pragma once

namespace WifiSetupNetworkPolicy {

enum class Mode {
    ApOnly,
    ApSta,
};

enum class SavedNetworkStart {
    None,
    DirectConnect,
    MaintenanceAutoConnect,
};

// Maintenance may use STA for saved-network testing and auto-join, while the
// HTTP ingress independently admits only sockets accepted through the AP IP.
inline Mode select(const bool /*maintenanceBootMode*/, const bool savedStaAvailable) {
    return savedStaAvailable ? Mode::ApSta : Mode::ApOnly;
}

inline bool usesSta(const bool maintenanceBootMode, const bool savedStaAvailable) {
    return select(maintenanceBootMode, savedStaAvailable) == Mode::ApSta;
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

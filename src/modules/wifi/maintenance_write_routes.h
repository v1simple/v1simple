#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "v1_profile_limits.h"

// Ingress policy only. Domain services retain payload validation and mutation.
namespace MaintenanceWriteRoutes {

enum class Id : uint8_t {
    DeviceSettings,
    V1Profile,
    V1ProfileDelete,
    V1Pull,
    V1Push,
    V1Apply,
    V1FactoryReset,
    V1DevicesName,
    V1DevicesProfile,
    V1DevicesDelete,
    AutopushSlot,
    AutopushActivate,
    AutopushPush,
    DisplaySettings,
    DisplaySettingsReset,
    DisplayPreview,
    DisplayPreviewClear,
    AudioSettings,
    QuietSettings,
    SettingsBackupNow,
    SettingsRestore,
    SystemRebootNormal,
    WifiScan,
    WifiDisconnect,
    WifiForget,
    WifiEnable,
    WifiNetworks,
    WifiNetworksPriorities,
    WifiNetworksDelete,
    WifiNetworksTest,
    ObdDevicesName,
    ObdScan,
    ObdForget,
    ObdConfig,
    GpsConfig,
    Count
};

enum class Reader : uint8_t { Framework, Exact };
struct Route {
    Id id;
    const char* path;
    size_t pathLength;
    Reader reader;
    bool bodyOnlyForm;
    bool legacyMultipart;
    size_t maxBytes;
};

inline constexpr size_t kOrdinaryBodyBytes = 4u * 1024u;
inline constexpr size_t kRestoreBodyBytes = 128u * 1024u;
inline constexpr size_t kOperationBodyBytes = 256u;

template <size_t N>
constexpr Route route(Id id, const char (&path)[N], Reader reader, bool bodyOnlyForm,
                      bool legacyMultipart, size_t maxBytes) {
    return {id, path, N - 1u, reader, bodyOnlyForm, legacyMultipart, maxBytes};
}

inline constexpr Route kRoutes[] = {
    route(Id::DeviceSettings, "/api/device/settings", Reader::Exact, true, true, kOrdinaryBodyBytes),
    route(Id::V1Profile, "/api/v1/profile", Reader::Exact, false, false, V1_PROFILE_HTTP_SAVE_MAX_BYTES),
    route(Id::V1ProfileDelete, "/api/v1/profile/delete", Reader::Exact, false, false, V1_PROFILE_HTTP_DELETE_MAX_BYTES),
    route(Id::V1Pull, "/api/v1/pull", Reader::Framework, false, false, kOrdinaryBodyBytes),
    route(Id::V1Push, "/api/v1/push", Reader::Framework, false, false, kOrdinaryBodyBytes),
    route(Id::V1Apply, "/api/v1/apply", Reader::Exact, true, false, kOperationBodyBytes),
    route(Id::V1FactoryReset, "/api/v1/factory-reset", Reader::Exact, true, false, kOperationBodyBytes),
    route(Id::V1DevicesName, "/api/v1/devices/name", Reader::Exact, true, true, kOrdinaryBodyBytes),
    route(Id::V1DevicesProfile, "/api/v1/devices/profile", Reader::Exact, true, true, kOrdinaryBodyBytes),
    route(Id::V1DevicesDelete, "/api/v1/devices/delete", Reader::Exact, true, true, kOrdinaryBodyBytes),
    route(Id::AutopushSlot, "/api/autopush/slot", Reader::Exact, true, true, kOrdinaryBodyBytes),
    route(Id::AutopushActivate, "/api/autopush/activate", Reader::Exact, true, true, kOrdinaryBodyBytes),
    route(Id::AutopushPush, "/api/autopush/push", Reader::Exact, true, false, kOperationBodyBytes),
    route(Id::DisplaySettings, "/api/display/settings", Reader::Framework, false, false, kOrdinaryBodyBytes),
    route(Id::DisplaySettingsReset, "/api/display/settings/reset", Reader::Framework, false, false, kOrdinaryBodyBytes),
    route(Id::DisplayPreview, "/api/display/preview", Reader::Framework, false, false, kOrdinaryBodyBytes),
    route(Id::DisplayPreviewClear, "/api/display/preview/clear", Reader::Framework, false, false, kOrdinaryBodyBytes),
    route(Id::AudioSettings, "/api/audio/settings", Reader::Framework, false, false, kOrdinaryBodyBytes),
    route(Id::QuietSettings, "/api/quiet/settings", Reader::Framework, false, false, kOrdinaryBodyBytes),
    route(Id::SettingsBackupNow, "/api/settings/backup-now", Reader::Framework, false, false, kOrdinaryBodyBytes),
    route(Id::SettingsRestore, "/api/settings/restore", Reader::Exact, false, false, kRestoreBodyBytes),
    route(Id::SystemRebootNormal, "/api/system/reboot-normal", Reader::Framework, false, false, kOrdinaryBodyBytes),
    route(Id::WifiScan, "/api/wifi/scan", Reader::Framework, false, false, kOrdinaryBodyBytes),
    route(Id::WifiDisconnect, "/api/wifi/disconnect", Reader::Framework, false, false, kOrdinaryBodyBytes),
    route(Id::WifiForget, "/api/wifi/forget", Reader::Framework, false, false, kOrdinaryBodyBytes),
    route(Id::WifiEnable, "/api/wifi/enable", Reader::Framework, false, false, kOrdinaryBodyBytes),
    route(Id::WifiNetworks, "/api/wifi/networks", Reader::Framework, false, false, kOrdinaryBodyBytes),
    route(Id::WifiNetworksPriorities, "/api/wifi/networks/priorities", Reader::Framework, false, false, kOrdinaryBodyBytes),
    route(Id::WifiNetworksDelete, "/api/wifi/networks/delete", Reader::Framework, false, false, kOrdinaryBodyBytes),
    route(Id::WifiNetworksTest, "/api/wifi/networks/test", Reader::Framework, false, false, kOrdinaryBodyBytes),
    route(Id::ObdDevicesName, "/api/obd/devices/name", Reader::Framework, false, true, kOrdinaryBodyBytes),
    route(Id::ObdScan, "/api/obd/scan", Reader::Framework, false, false, kOrdinaryBodyBytes),
    route(Id::ObdForget, "/api/obd/forget", Reader::Framework, false, false, kOrdinaryBodyBytes),
    route(Id::ObdConfig, "/api/obd/config", Reader::Framework, false, false, kOrdinaryBodyBytes),
    route(Id::GpsConfig, "/api/gps/config", Reader::Framework, false, false, kOrdinaryBodyBytes),
};

constexpr bool routeIdsMatchIndices() {
    for (size_t i = 0; i < sizeof(kRoutes) / sizeof(kRoutes[0]); ++i) {
        if (static_cast<size_t>(kRoutes[i].id) != i) return false;
    }
    return true;
}
static_assert(sizeof(kRoutes) / sizeof(kRoutes[0]) == static_cast<size_t>(Id::Count));
static_assert(routeIdsMatchIndices());

template <Id id>
constexpr const Route& get() {
    static_assert(id < Id::Count);
    return kRoutes[static_cast<size_t>(id)];
}

inline const Route* find(const char* begin, const char* end) {
    const size_t length = static_cast<size_t>(end - begin);
    for (const Route& candidate : kRoutes) {
        if (length == candidate.pathLength && std::memcmp(begin, candidate.path, length) == 0) return &candidate;
    }
    return nullptr;
}

} // namespace MaintenanceWriteRoutes

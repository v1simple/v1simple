#pragma once

#include <cstdint>

namespace V1ProfilePushPolicy {

template <typename SettingsLike>
inline bool shouldDisableV1Laser(const SettingsLike& settings) {
    return settings.alpEnabled && settings.alpDisableV1LaserOnPush;
}

template <typename SettingsLike>
inline void applyBeforePush(const SettingsLike& settings, uint8_t bytes[6]) {
    static constexpr uint8_t kV1LaserEnableBit = 0x08;  // V1 user byte 0 bit 3

    if (!bytes || !shouldDisableV1Laser(settings)) {
        return;
    }

    bytes[0] &= static_cast<uint8_t>(~kV1LaserEnableBit);
}

template <typename SettingsLike, typename UserSettingsLike>
inline void applyBeforePushToUserSettings(const SettingsLike& settings,
                                          UserSettingsLike& userSettings) {
    if (!shouldDisableV1Laser(settings)) {
        return;
    }

    applyBeforePush(settings, userSettings.bytes);
}

}  // namespace V1ProfilePushPolicy

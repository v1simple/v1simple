#pragma once

namespace WifiApLifecyclePolicy {

constexpr bool afterBringupAbort(bool wasEnabled) {
    (void)wasEnabled;
    return false;
}

constexpr bool isSetupModeActive(bool serviceStateActive, bool interfaceEnabled) {
    return serviceStateActive && interfaceEnabled;
}

constexpr bool shouldDisableInterfaceOnStop(bool modeHasAp, bool interfaceEnabled) {
    return modeHasAp || interfaceEnabled;
}

} // namespace WifiApLifecyclePolicy

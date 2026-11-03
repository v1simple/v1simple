#pragma once

// Literal compatibility oracle captured from the previous explicit route and
// preflight policies. Do not derive expected behavior from the descriptor.
struct RouteCase {
    const char* path;
    bool exact;
    bool bodyOnlyForm;
    bool multipart;
    size_t maxBytes;
};
inline constexpr RouteCase routeCases[] = {
    {"/api/device/settings", true, true, true, 4096u},
    {"/api/v1/profile", true, false, false, 16384u},
    {"/api/v1/profile/delete", true, false, false, 2048u},
    {"/api/v1/pull", false, false, false, 4096u},
    {"/api/v1/push", false, false, false, 4096u},
    {"/api/v1/apply", true, true, false, 256u},
    {"/api/v1/factory-reset", true, true, false, 256u},
    {"/api/v1/devices/name", true, true, true, 4096u},
    {"/api/v1/devices/profile", true, true, true, 4096u},
    {"/api/v1/devices/delete", true, true, true, 4096u},
    {"/api/autopush/slot", true, true, true, 4096u},
    {"/api/autopush/activate", true, true, true, 4096u},
    {"/api/autopush/push", true, true, false, 256u},
    {"/api/display/settings", false, false, false, 4096u},
    {"/api/display/settings/reset", false, false, false, 4096u},
    {"/api/display/preview", false, false, false, 4096u},
    {"/api/display/preview/clear", false, false, false, 4096u},
    {"/api/audio/settings", false, false, false, 4096u},
    {"/api/quiet/settings", false, false, false, 4096u},
    {"/api/settings/backup-now", false, false, false, 4096u},
    {"/api/settings/restore", true, false, false, 131072u},
    {"/api/system/reboot-normal", false, false, false, 4096u},
    {"/api/wifi/scan", false, false, false, 4096u},
    {"/api/wifi/disconnect", false, false, false, 4096u},
    {"/api/wifi/forget", false, false, false, 4096u},
    {"/api/wifi/enable", false, false, false, 4096u},
    {"/api/wifi/networks", false, false, false, 4096u},
    {"/api/wifi/networks/priorities", false, false, false, 4096u},
    {"/api/wifi/networks/delete", false, false, false, 4096u},
    {"/api/wifi/networks/test", false, false, false, 4096u},
    {"/api/obd/devices/name", false, false, true, 4096u},
    {"/api/obd/scan", false, false, false, 4096u},
    {"/api/obd/forget", false, false, false, 4096u},
    {"/api/obd/config", false, false, false, 4096u},
    {"/api/gps/config", false, false, false, 4096u},
};

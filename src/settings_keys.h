#pragma once

#include <cstddef>

// NVS key constants for settings namespaces (SETTINGS_NS_A / SETTINGS_NS_B).
// Eliminates raw string literals in settings read/write paths.
// See settings_namespace_ids.h for namespace name constants.

// ── Validation / versioning ────────────────────────────────────────────────
inline constexpr const char* kNvsValid = "nvsValid";
inline constexpr const char* kNvsSettingsVer = "settingsVer";
// Set when NVS was detected empty/partial before an SD backup could be
// consulted.  While present, SD backup restore remains authoritative even if
// later boot/shutdown paths persist factory defaults into NVS.
inline constexpr const char* kNvsRestorePending = "restorePend";
inline constexpr const char* kNvsBackupDueRevision = "backupDueRev";
inline constexpr const char* kNvsBackupCompletedRevision = "backupDoneRev";

// ── WiFi ──────────────────────────────────────────────────────────────────
inline constexpr const char* kNvsEnableWifi = "enableWifi";
// kNvsWifiMode was removed: wifiMode is derived from wifiClientEnabled on
// every load() — storing it separately is redundant and misleading.
inline constexpr const char* kNvsApSsid = "apSSID";
inline constexpr const char* kNvsApPassword = "apPassword";
inline constexpr const char* kNvsWifiClientEnabled = "wifiClientEn";
// Legacy single-SSID key; readable for migration into wifiStaSlot0.
inline constexpr const char* kNvsWifiClientSsid = "wifiClSSID";
inline constexpr size_t kNvsWifiStaSlotCount = 4;
inline constexpr const char* kNvsWifiStaSlotSsid[kNvsWifiStaSlotCount] = {"sta0Ssid", "sta1Ssid", "sta2Ssid",
                                                                          "sta3Ssid"};
inline constexpr const char* kNvsWifiStaSlotLabel[kNvsWifiStaSlotCount] = {"sta0Label", "sta1Label", "sta2Label",
                                                                           "sta3Label"};
inline constexpr const char* kNvsWifiStaSlotPriority[kNvsWifiStaSlotCount] = {"sta0Prio", "sta1Prio", "sta2Prio",
                                                                              "sta3Prio"};
inline constexpr const char* kNvsWifiStaSlotLastConnected[kNvsWifiStaSlotCount] = {"sta0Last", "sta1Last", "sta2Last",
                                                                                   "sta3Last"};

// ── Proxy BLE ─────────────────────────────────────────────────────────────
inline constexpr const char* kNvsProxyBle = "proxyBLE";
inline constexpr const char* kNvsProxyName = "proxyName";

// ── Display ───────────────────────────────────────────────────────────────
inline constexpr const char* kNvsDisplayOff = "displayOff";
inline constexpr const char* kNvsBrightness = "brightness";

// ── Alert colors ──────────────────────────────────────────────────────────
inline constexpr const char* kNvsColorBogey = "colorBogey";
inline constexpr const char* kNvsColorFreq = "colorFreq";
inline constexpr const char* kNvsColorArrowFront = "colorArrF";
inline constexpr const char* kNvsColorArrowSide = "colorArrS";
inline constexpr const char* kNvsColorArrowRear = "colorArrR";

// ── Band colors ───────────────────────────────────────────────────────────
inline constexpr const char* kNvsColorBandLaser = "colorBandL";
inline constexpr const char* kNvsColorBandKa = "colorBandKa";
inline constexpr const char* kNvsColorBandK = "colorBandK";
inline constexpr const char* kNvsColorBandX = "colorBandX";
inline constexpr const char* kNvsColorBandPhoto = "colorBandP";

// ── Status icon colors ────────────────────────────────────────────────────
inline constexpr const char* kNvsColorWifi = "colorWiFi";
inline constexpr const char* kNvsColorWifiConnected = "colorWiFiC";
inline constexpr const char* kNvsColorBleConnected = "colorBleC";
inline constexpr const char* kNvsColorBleDisconnected = "colorBleD";

// ── Signal bar colors ─────────────────────────────────────────────────────
inline constexpr const char* kNvsColorBar1 = "colorBar1";
inline constexpr const char* kNvsColorBar2 = "colorBar2";
inline constexpr const char* kNvsColorBar3 = "colorBar3";
inline constexpr const char* kNvsColorBar4 = "colorBar4";
inline constexpr const char* kNvsColorBar5 = "colorBar5";
inline constexpr const char* kNvsColorBar6 = "colorBar6";

// ── Misc UI colors ────────────────────────────────────────────────────────
inline constexpr const char* kNvsColorMuted = "colorMuted";
inline constexpr const char* kNvsColorPersisted = "colorPersist";
inline constexpr const char* kNvsColorVolumeMain = "colorVolMain";
inline constexpr const char* kNvsColorVolumeMute = "colorVolMute";
inline constexpr const char* kNvsColorRssiV1 = "colorRssiV1";
inline constexpr const char* kNvsColorRssiProxy = "colorRssiPrx";
inline constexpr const char* kNvsColorObd = "colorObd";
inline constexpr const char* kNvsColorAlpConn = "colorAlpCon";
inline constexpr const char* kNvsColorAlpDli = "colorAlpDli";
inline constexpr const char* kNvsColorAlpLid = "colorAlpLid";
inline constexpr const char* kNvsColorAlpAlert = "colorAlpAlt";

// ── UI toggles ────────────────────────────────────────────────────────────
inline constexpr const char* kNvsFreqBandColor = "freqBandCol";
inline constexpr const char* kNvsHideWifi = "hideWifi";
inline constexpr const char* kNvsHideProfile = "hideProfile";
inline constexpr const char* kNvsHideBattery = "hideBatt";
inline constexpr const char* kNvsBatteryPercent = "battPct";
inline constexpr const char* kNvsHideBle = "hideBle";
inline constexpr const char* kNvsHideVolume = "hideVol";
inline constexpr const char* kNvsHideRssi = "hideRssi";

// Voice alerts
inline constexpr const char* kNvsVoiceAlertsLegacy = "voiceAlerts"; // migration key only
inline constexpr const char* kNvsVoiceMode = "voiceMode";
inline constexpr const char* kNvsVoiceDirection = "voiceDir";
inline constexpr const char* kNvsVoiceBogeys = "voiceBogeys";
inline constexpr const char* kNvsMuteVoiceAtVol0 = "muteVoiceVol0";
inline constexpr const char* kNvsVoiceVolume = "voiceVol";

// Secondary alerts
inline constexpr const char* kNvsSecondaryAlerts = "secAlerts";
inline constexpr const char* kNvsSecondaryLaser = "secLaser";
inline constexpr const char* kNvsSecondaryKa = "secKa";
inline constexpr const char* kNvsSecondaryK = "secK";
inline constexpr const char* kNvsSecondaryX = "secX";

// ── Volume fade ───────────────────────────────────────────────────────────
inline constexpr const char* kNvsVolFadeEnabled = "volFadeEn";
inline constexpr const char* kNvsVolFadeSeconds = "volFadeSec";
inline constexpr const char* kNvsVolFadeVolume = "volFadeVol";

// ── Speed mute ────────────────────────────────────────────────────────────
inline constexpr const char* kNvsSpeedMuteEnabled = "spdMuteEn";
inline constexpr const char* kNvsSpeedMuteThreshold = "spdMuteThr";
inline constexpr const char* kNvsSpeedMuteHysteresis = "spdMuteHys";
inline constexpr const char* kNvsSpeedMuteVolume = "spdMuteVol";
inline constexpr const char* kNvsSpeedMuteVoice = "spdMuteVce";

// ── Stealth mode ──────────────────────────────────────────────────────────
inline constexpr const char* kNvsStealthEnabled = "stealthEn";

// ── Profiles / settings slots ─────────────────────────────────────────────
inline constexpr const char* kNvsAutoPush = "autoPush";
inline constexpr const char* kNvsActiveSlot = "activeSlot";

inline constexpr const char* kNvsSlot0Name = "slot0name";
inline constexpr const char* kNvsSlot1Name = "slot1name";
inline constexpr const char* kNvsSlot2Name = "slot2name";

inline constexpr const char* kNvsSlot0Color = "slot0color";
inline constexpr const char* kNvsSlot1Color = "slot1color";
inline constexpr const char* kNvsSlot2Color = "slot2color";

inline constexpr const char* kNvsSlot0Volume = "slot0vol";
inline constexpr const char* kNvsSlot1Volume = "slot1vol";
inline constexpr const char* kNvsSlot2Volume = "slot2vol";

inline constexpr const char* kNvsSlot0MuteVolume = "slot0mute";
inline constexpr const char* kNvsSlot1MuteVolume = "slot1mute";
inline constexpr const char* kNvsSlot2MuteVolume = "slot2mute";

inline constexpr const char* kNvsSlot0DarkMode = "slot0dark";
inline constexpr const char* kNvsSlot1DarkMode = "slot1dark";
inline constexpr const char* kNvsSlot2DarkMode = "slot2dark";

inline constexpr const char* kNvsSlot0MuteToZero = "slot0mz";
inline constexpr const char* kNvsSlot1MuteToZero = "slot1mz";
inline constexpr const char* kNvsSlot2MuteToZero = "slot2mz";

inline constexpr const char* kNvsSlot0Persistence = "slot0persist";
inline constexpr const char* kNvsSlot1Persistence = "slot1persist";
inline constexpr const char* kNvsSlot2Persistence = "slot2persist";

inline constexpr const char* kNvsSlot0PriorityArrow = "slot0prio";
inline constexpr const char* kNvsSlot1PriorityArrow = "slot1prio";
inline constexpr const char* kNvsSlot2PriorityArrow = "slot2prio";

inline constexpr const char* kNvsSlot0Profile = "slot0prof";
inline constexpr const char* kNvsSlot0Mode = "slot0mode";
inline constexpr const char* kNvsSlot1Profile = "slot1prof";
inline constexpr const char* kNvsSlot1Mode = "slot1mode";
inline constexpr const char* kNvsSlot2Profile = "slot2prof";
inline constexpr const char* kNvsSlot2Mode = "slot2mode";

// ── Miscellaneous ─────────────────────────────────────────────────────────
inline constexpr const char* kNvsLastV1Address = "lastV1Addr";
inline constexpr const char* kNvsAutoPowerOff = "autoPwrOff";
inline constexpr const char* kNvsApTimeout = "apTimeout";

// ── OBD ───────────────────────────────────────────────────────────────────
inline constexpr const char* kNvsObdEnabled = "obdEn";
inline constexpr const char* kNvsObdAddress = "obdAddr";
inline constexpr const char* kNvsObdName = "obdName";
inline constexpr const char* kNvsObdAddressType = "obdAddrT";
inline constexpr const char* kNvsObdMinRssi = "obdMinRssi";
inline constexpr const char* kNvsCycleObdScanWindow = "cycObdWin";
inline constexpr const char* kNvsCycleObdRetryInt = "cycObdRet";
inline constexpr const char* kNvsCycleProxyOpenWindow = "cycPrxWin";
inline constexpr const char* kNvsCycleWifiOpenTimeout = "cycWifiOp";
inline constexpr const char* kNvsCycleV1SettleQuiet = "cycV1Quiet";
inline constexpr const char* kNvsCycleV1SettleFallback = "cycV1Fall";
inline constexpr const char* kNvsCycleTeardownAckTimeout = "cycTdAck";

// ── ALP ──────────────────────────────────────────────────────────────────
inline constexpr const char* kNvsAlpEnabled = "alpEn";
inline constexpr const char* kNvsAlpSdLog = "alpSdLog";
// Separate persistence window for ALP laser display. Defaults to 0 because the
// ALP has its own speaker and the post-engagement visual tail adds no value
// once the session closes. Reuse of slot alertPersistSec was the original
// design but user log analysis showed it holds a phantom laser event on
// screen for the full V1 persist window. See docs/plans/ALP_PERSIST_SETTING.
inline constexpr const char* kNvsAlpPersistSec = "alpPersist";
inline constexpr const char* kNvsAlpNoV1Laser = "alpNoV1Laser";

// ── Debug / diagnostics ──────────────────────────────────────────────────
inline constexpr const char* kNvsPowerOffSdLog = "pwrOffSdLog";
inline constexpr const char* kNvsMaintenanceBootReq = "maintBoot";
// ── GPS ───────────────────────────────────────────────────────────────────────
inline constexpr const char* kNvsGpsEnabled = "gpsEnabled";
inline constexpr const char* kNvsGpsBaud = "gpsBaud";
inline constexpr const char* kNvsGpsEnablePolarity = "gpsEnHi";
inline constexpr const char* kNvsGpsLogUtcToPerf = "gpsLogUtcP";
inline constexpr const char* kNvsGpsLogUtcToAlp = "gpsLogUtcA";
// ── Separate namespaces ───────────────────────────────────────────────────
// Namespace: v1wificlient (WIFI_CLIENT_NS)
inline constexpr const char* kSettingsWifiClientNamespace = "v1wificlient";
// Legacy single-password key; readable for migration into wifiStaSlot0.
inline constexpr const char* kNvsWifiPassword = "password";
inline constexpr const char* kNvsWifiStaSlotPassword[kNvsWifiStaSlotCount] = {"sta0Pass", "sta1Pass", "sta2Pass",
                                                                              "sta3Pass"};
// Namespace: v1settingsMeta (SETTINGS_NS_META)
inline constexpr const char* kNvsMetaActive = "active";
// Namespace: v1boot
inline constexpr const char* kNvsBootId = "bootId";
inline constexpr const char* kNvsCleanShutdn =
    "cleanShutdn"; // set true at end of prepareForShutdown(); read+reset at boot

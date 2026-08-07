#pragma once

class StorageManager;
class SettingsManager;
class PerfSdLogger;
class AlpSdLogger;
class GpsRuntimeModule;
class GpsTimePublisher;
class GpsGeoPublisher;

struct BootLoggingRuntimeServices {
    StorageManager& storage;
    SettingsManager& settings;
    PerfSdLogger& perfLogger;
    AlpSdLogger& alpLogger;
    GpsRuntimeModule& gpsRuntime;
    GpsTimePublisher& gpsTime;
    GpsGeoPublisher& gpsGeo;

    explicit BootLoggingRuntimeServices(StorageManager& storageRef, SettingsManager& settingsRef,
                                        PerfSdLogger& perfLoggerRef, AlpSdLogger& alpLoggerRef,
                                        GpsRuntimeModule& gpsRuntimeRef, GpsTimePublisher& gpsTimeRef,
                                        GpsGeoPublisher& gpsGeoRef)
        : storage(storageRef), settings(settingsRef), perfLogger(perfLoggerRef), alpLogger(alpLoggerRef),
          gpsRuntime(gpsRuntimeRef), gpsTime(gpsTimeRef), gpsGeo(gpsGeoRef) {}
};

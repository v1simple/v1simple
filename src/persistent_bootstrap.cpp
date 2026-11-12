#include "persistent_bootstrap.h"

#include "settings.h"
#include "storage_manager.h"
#include "v1_devices.h"
#include "v1_profiles.h"

void PersistentBootstrap::run(StorageManager& storage, SettingsManager& settings,
                              V1ProfileManager& profiles, V1DeviceStore& devices,
                              const PersistentBootstrapEffects& effects) {
    Serial.println("[Setup] Mounting storage...");
    if (storage.begin()) {
        Serial.printf("[Setup] Storage ready: %s\n", storage.statusText().c_str());
        profiles.begin(storage, [](void* context) {
            return static_cast<SettingsManager*>(context)->canOpenProfileStorage();
        }, &settings);
        devices.begin(storage.getFilesystem(), storage.getLittleFS());
        effects.afterCatalogMounted(effects.context);

        if (settings.checkAndRestoreFromSD()) {
            effects.afterSettingsRestored(effects.context);
        }
        settings.migrateAutoPushProfilesToV2();

        const bool deleteResolved = settings.resolvePendingV1DeviceDelete(devices);
        if (!deleteResolved) {
            Serial.println("[Setup] WARN: pending V1 device deletion blocks fallback bootstrap");
        }
        const String storedFallback = deleteResolved ? settings.loadLastV1AddressFallback() : String();
        const String degradedFallback = normalizeV1DeviceAddress(storedFallback);
        if (storedFallback.length() > 0 && degradedFallback.length() == 0) {
            settings.clearLastV1AddressFallback();
        }
        const String settingsFallback = deleteResolved
                                            ? normalizeV1DeviceAddress(settings.get().lastV1Address)
                                            : String();
        const String restoredLastKnownV1 = degradedFallback.length() > 0 ? degradedFallback : settingsFallback;
        if (restoredLastKnownV1.length() > 0) {
            settings.setLastV1Address(restoredLastKnownV1);
            if (devices.isReady() &&
                devices.bootstrapDevice(restoredLastKnownV1, degradedFallback.length() > 0) &&
                degradedFallback.length() > 0) {
                settings.clearLastV1AddressFallback();
            }
        }
        settings.validateProfileReferences(profiles);
    } else {
        Serial.println("[Setup] Storage unavailable - profiles will be disabled");
        const String storedFallback = settings.loadLastV1AddressFallback();
        const String degradedFallback = normalizeV1DeviceAddress(storedFallback);
        if (degradedFallback.length() > 0) {
            settings.setLastV1Address(degradedFallback);
            Serial.println("[Setup] Restored degraded V1 address fallback from NVS");
        } else if (storedFallback.length() > 0) {
            settings.clearLastV1AddressFallback();
        }
    }
}

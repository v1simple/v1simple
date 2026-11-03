#pragma once

class StorageManager;
class SettingsManager;
class V1ProfileManager;
class V1DeviceStore;

// Fixed synchronous insertion points; run() does not retain the borrowed context.
struct PersistentBootstrapEffects {
    void* context;
    void (*afterCatalogMounted)(void*);
    void (*afterSettingsRestored)(void*);
};

namespace PersistentBootstrap {
void run(StorageManager& storage, SettingsManager& settings,
         V1ProfileManager& profiles, V1DeviceStore& devices,
         const PersistentBootstrapEffects& effects);
}

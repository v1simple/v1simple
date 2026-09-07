#include "usb_profile_runtime.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>
#include <cstdio>

#include "build_metadata.h"
#include "drive_runtime.h"
#include "maintenance_runtime.h"
#include "modules/wifi/wifi_json_document.h"
#include "settings.h"
#include "usb_profile_document.h"

static_assert(UsbProfileProtocol::MaxDocumentBytes == kUsbProfileDocumentMaxBytes,
              "USB staging and profile document limits must agree");

UsbProfileRuntime::UsbProfileRuntime(DriveRuntime& drive, MaintenanceRuntime& maintenance,
                                     SettingsManager& settings, V1ProfileManager& profiles)
    : drive_(drive), maintenance_(maintenance), settings_(settings), profiles_(profiles), protocol_(*this, *this) {}

UsbProfileStatus UsbProfileRuntime::status() const {
    UsbProfileStatus result;
    result.maintenance = maintenance_.active();
    result.busy = result.maintenance ? !maintenance_.usbConfigurationAllowed() : !drive_.usbMaintenanceAllowed();
    result.boot = result.maintenance ? maintenance_.bootId() : drive_.bootId();
    result.git = getBuildGitSha();
    result.image = getRuntimeImageId();
    result.slot = V1Settings::normalizeAutoPushSlotIndex(settings_.get().activeSlot);
    result.persistence = settings_.getSlotAlertPersistSec(result.slot);
    result.enabled = settings_.get().autoPushEnabled;
    return result;
}

uint8_t* UsbProfileRuntime::allocate(size_t bytes) {
    // Transfer storage exists only for a requested maintenance operation. Do
    // not consume internal DMA/alert memory for a host's bulk payload.
    return static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

void UsbProfileRuntime::release(uint8_t* data) { heap_caps_free(data); }

bool UsbProfileRuntime::backup(uint8_t*& data, size_t& length, char* error, size_t errorSize) {
    if (!maintenance_.usbConfigurationAllowed()) return false;
    WifiJson::Document document;
    String detail;
    if (!buildUsbProfileDocument(document, settings_, profiles_, detail)) {
        std::snprintf(error, errorSize, "%s", detail.c_str());
        return false;
    }
    length = measureJson(document);
    if (document.overflowed() || length == 0 || length > kUsbProfileDocumentMaxBytes) {
        std::snprintf(error, errorSize, "Profile backup exceeds USB capacity");
        return false;
    }
    data = allocate(length + 1);
    if (!data) { std::snprintf(error, errorSize, "Profile transfer memory unavailable"); return false; }
    if (serializeJson(document, reinterpret_cast<char*>(data), length + 1) != length) {
        std::snprintf(error, errorSize, "Profile backup serialization incomplete");
        return false;
    }
    return true;
}

bool UsbProfileRuntime::apply(const uint8_t* data, size_t length, bool& backupPending, int& profiles,
                              char* error, size_t errorSize) {
    if (!maintenance_.usbConfigurationAllowed()) return false;
    WifiJson::Document document;
    const auto parsed = deserializeJson(document, data, length, DeserializationOption::NestingLimit(8));
    if (parsed || document.overflowed()) {
        std::snprintf(error, errorSize, "Invalid or oversized profile JSON");
        return false;
    }
    String detail;
    const SettingsRestoreWatchdog watchdog{[](void*) { (void)esp_task_wdt_reset(); }, nullptr};
    const auto result = applyUsbProfileDocument(settings_, profiles_, document, detail, watchdog);
    if (!result.success) {
        std::snprintf(error, errorSize, "%s", detail.length() ? detail.c_str() : "Profile storage transaction failed");
        return false;
    }
    profiles = result.profilesRestored;
    backupPending = settings_.deferredBackupPending();
    return true;
}

void UsbProfileRuntime::enterMaintenance() { drive_.requestUsbMaintenanceBoot(); }
void UsbProfileRuntime::restartNormal() { maintenance_.requestUsbNormalBoot(); }
void UsbProfileRuntime::activity(uint32_t nowMs) { maintenance_.recordUsbActivity(nowMs); }
int UsbProfileRuntime::available() { return Serial.available(); }
int UsbProfileRuntime::read() { return Serial.read(); }
int UsbProfileRuntime::availableForWrite() { return Serial.availableForWrite(); }
size_t UsbProfileRuntime::write(const uint8_t* data, size_t length) { return Serial.write(data, length); }

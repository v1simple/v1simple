#pragma once

#include "usb_profile_protocol.h"

class DriveRuntime;
class MaintenanceRuntime;
class SettingsManager;
class V1ProfileManager;

class UsbProfileRuntime final : public UsbProfileBackend, private UsbProfileTransport {
  public:
    UsbProfileRuntime(DriveRuntime& drive, MaintenanceRuntime& maintenance,
                      SettingsManager& settings, V1ProfileManager& profiles);
    void tick(uint32_t nowMs) { protocol_.tick(nowMs); }

  private:
    UsbProfileStatus status() const override;
    uint8_t* allocate(size_t bytes) override;
    void release(uint8_t* data) override;
    bool backup(uint8_t*& data, size_t& length, char* error, size_t errorSize) override;
    bool apply(const uint8_t* data, size_t length, bool& backupPending, int& profiles,
               char* error, size_t errorSize) override;
    void enterMaintenance() override;
    void restartNormal() override;
    void activity(uint32_t nowMs) override;
    int available() override;
    int read() override;
    int availableForWrite() override;
    size_t write(const uint8_t* data, size_t length) override;

    DriveRuntime& drive_;
    MaintenanceRuntime& maintenance_;
    SettingsManager& settings_;
    V1ProfileManager& profiles_;
    UsbProfileProtocol protocol_;
};

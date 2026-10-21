#include <cassert>
#include <cstdio>
// Production update/UART ingest path; UNIT_TEST is deliberately absent here.
#include "src/modules/gps/gps_runtime_module.cpp"
// Only the selector's disabled OBD dependency is replaced.
#define UNIT_TEST
class ObdRuntimeModule {
  public:
    bool getFreshSpeed(uint32_t, float &, uint32_t &) const { return false; }
};
#include "src/modules/speed/speed_source_selector.cpp"
#include "src/modules/speed_mute/speed_mute_module.cpp"
#include "src/runtime_coordinator.h"
struct SensorDriveRuntime {
    GpsRuntimeModule &gps;
    SpeedSourceSelector &selector;
    SpeedMuteModule &mute;
    bool inSettings = false;
    unsigned gpsCalls = 0;
    struct Connection {
        bool connected = true, backpressured = false, overloaded = false, requestShowInitialScanning = false;
    };
    bool active() const { return true; }
    DriveLoopTiming beginDriveLoop() {
        return {static_cast<uint32_t>(mockMillis * 1000), static_cast<uint32_t>(mockMillis)};
    }
    Connection processConnectionRuntime(uint32_t) { return {}; }
    void acceptConnectionSnapshot(const Connection &) {}
    void showInitialScanningScreen() {}
    void markInitialScanningScreenHandled() {}
    bool powerOwnsPresentation() const { return false; }
    void presentConnectionState(const Connection &) {}
    void processPower(uint32_t) {}
    bool processTouch(uint32_t) { return inSettings; }
    void servicePowerDisplayOwnership(uint32_t) {}
    void processAlp(uint32_t) {}
    bool preemptSettingsForLiveAlert() { return false; }
    void openBootReadyGate(uint32_t) {}
    void processBleRuntime() {}
    void observeAlpProductState(uint32_t) {}
    void processPeriodicMaintenance(uint32_t, bool, bool, bool, bool = false) {}
    void finishDriveLoop(bool, uint32_t, bool) {}
    void processTapGesture(uint32_t) {}
    void processBleQueue() {}
    bool bleQueueBackpressured() { return false; }
    void processConnectionCycle(uint32_t, bool) {}
    void processObd(uint32_t, bool) {}
    void processAlpPresentationAndPower(uint32_t) {}
    template <typename Edges> bool processParsedPolicies(const Edges&) { return false; }
    void processGps(uint32_t now) {
        ++gpsCalls;
        gps.update(now);
    }
    void processSpeed(uint32_t now) { selector.update(now); }
    void processSpeedAlert(uint32_t now) {
        auto s = selector.selectedSpeed();
        mute.update(s.speedMph, s.valid, now);
    }
    bool hasLiveAlertPresentation() { return false; }
    void servicePowerAlertPresentation(uint32_t, bool) {}
    bool powerAlertPresentationNeedsRestore() { return false; }
    int consumeDisplayEdges() { return 0; }
    void presentDisplay(int, bool) {}
    DriveLoopDispatch processConnectionDispatch(bool) { return {static_cast<uint32_t>(mockMillis), true}; }
};

static void checkIndependentQualityFreshness() {
    const char* good = "$GPGGA,123520,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*4D\r\n";
    const char* corrupt = "$GPGGA,123520,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*00\r\n";
    const char* speed = "$GPRMC,123519,A,4807.038,N,01131.000,E,000.0,084.4,230394,003.1,W*6E\r\n";
    for (unsigned mode : {0u, 1u, 2u}) {
        Serial1.reset();
        mockMillis = 1;
        GpsRuntimeModule gps;
        gps.begin(true);
        SpeedSourceSelector selector;
        selector.begin(nullptr, false, &gps, true);
        SpeedMuteModule mute;
        mute.begin(true, 25, 3, 0);
        mockMillis = 10000;
        if (mode != 0)
            Serial1.receive(good);
        Serial1.receive(speed);
        gps.update(10000);
        selector.update(10000);
        auto selected = selector.selectedSpeed();
        assert(selected.valid == (mode != 0));
        assert(mute.update(selected.speedMph, selected.valid, 10000).shouldMute == (mode != 0));
        // Only actual UART parsing can refresh quality. Current RMC sentences
        // and corrupt GGA traffic must not extend the last good GGA's lifetime.
        for (uint32_t now = 11000; now <= 71000; now += 1000) {
            mockMillis = now;
            if (mode == 2)
                Serial1.receive(corrupt);
            Serial1.receive(speed);
            gps.update(now);
            selector.update(now);
            selected = selector.selectedSpeed();
            mute.update(selected.speedMph, selected.valid, now);
        }
        assert(gps.snapshot(71000).hasFix && selector.snapshot().gpsFresh);
        assert(!selected.valid && !mute.getState().muteActive);
        printf("fresh UART RMC with quality mode=%u after 61 seconds: selected=%d mute=%d\n",
               mode, selected.valid, mute.getState().muteActive);
        mockMillis = 72000;
        Serial1.receive(std::string(good) + speed);
        gps.update(72000);
        selector.update(72000);
        selected = selector.selectedSpeed();
        assert(selected.valid && mute.update(selected.speedMph, selected.valid, 72000).shouldMute);
    }
}

static void checkIndependentSpeedExpiry() {
    const char* gga = "$GPGGA,123520,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*4D\r\n";
    const char* stopped = "$GPRMC,123519,A,4807.038,N,01131.000,E,000.0,084.4,230394,003.1,W*6E\r\n";
    const char* moving = "$GPRMC,123519,A,4807.038,N,01131.000,E,040.0,084.4,230394,003.1,W*6A\r\n";
    Serial1.reset();
    mockMillis = 1;
    GpsRuntimeModule gps;
    gps.begin(true);
    SpeedSourceSelector selector;
    selector.begin(nullptr, false, &gps, true);
    SpeedMuteModule mute;
    mute.begin(true, 25, 3, 0);
    auto tick = [&](uint32_t now) {
        mockMillis = now;
        Serial1.receive(gga);
        gps.update(now);
        selector.update(now);
        const auto selected = selector.selectedSpeed();
        mute.update(selected.speedMph, selected.valid, now);
        return selected;
    };
    Serial1.receive(stopped);
    assert(tick(10000).valid && mute.getState().muteActive);
    for (uint32_t now = 11000; now <= 14000; now += 1000) tick(now);
    assert(gps.snapshot(14000).hasFix && !gps.snapshot(14000).sampleValid);
    assert(!selector.selectedSpeed().valid && !mute.getState().muteActive);
    // Expiry was observed by the actual loop before a complete counter cycle.
    assert(!tick(10000).valid && !mute.getState().muteActive);
    assert(gps.snapshot(10000).qualityFresh && gps.snapshot(10000).hardwareSamples == 1);
    Serial1.receive(moving);
    assert(tick(11000).speedMph > 46 && !mute.getState().muteActive);
    puts("expired UART RMC stays invalid across a full timer cycle; fresh RMC recovers");
}

int main() {
    checkIndependentQualityFreshness();
    checkIndependentSpeedExpiry();
    const char *gga = "$GPGGA,123520,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*4D\r\n";
    const char *stoppedRmc = "$GPRMC,123519,A,4807.038,N,01131.000,E,000.0,084.4,230394,003.1,W*6E\r\n";
    const char *movingRmc = "$GPRMC,123519,A,4807.038,N,01131.000,E,040.0,084.4,230394,003.1,W*6A\r\n";
    for (bool modal : {false, true}) {
        Serial1.reset();
        mockMillis = 1;
        GpsRuntimeModule gps;
        gps.begin(true);
        SpeedSourceSelector selector;
        selector.begin(nullptr, false, &gps, true);
        SpeedMuteModule mute;
        mute.begin(true, 25, 3, 0);
        // Bytes are received by UART at t10000. A settings modal must not defer GPS processing.
        mockMillis = 10000;
        Serial1.receive(std::string(gga) + stoppedRmc);
        assert(Serial1.available() < 256);
        SensorDriveRuntime drive{gps, selector, mute};
        drive.inSettings = modal;
        DriveLoopCoordinator::tick(drive);
        assert(drive.gpsCalls == 1U);
        assert(Serial1.available() == 0);
        assert(selector.selectedSpeed().timestampMs == 10000);
        for (mockMillis = 11000; mockMillis < 20000; mockMillis += 1000)
            DriveLoopCoordinator::tick(drive);
        assert(drive.gpsCalls == 10U && Serial1.available() == 0);
        drive.inSettings = false;
        mockMillis = 20000;
        DriveLoopCoordinator::tick(drive);
        auto selected = selector.selectedSpeed();
        const bool muted = mute.update(selected.speedMph, selected.valid, 20000).shouldMute;
        printf("sample received t10000, examined t20000, settings open=%d: selected=%d timestamp=%u age=%u "
               "speed=%.1f mute=%d\n",
               modal, selected.valid, selected.timestampMs, selected.ageMs, selected.speedMph, muted);
        assert(!selected.valid && !muted);
        // A current moving fix works normally after the old sample expires.
        mockMillis = 22000;
        Serial1.receive(std::string(gga) + movingRmc);
        gps.update(22000);
        selector.update(22000);
        selected = selector.selectedSpeed();
        assert(selected.valid && selected.speedMph > 46 && selected.speedMph < 47);
        assert(!mute.update(selected.speedMph, selected.valid, 22000).shouldMute);
    }
}

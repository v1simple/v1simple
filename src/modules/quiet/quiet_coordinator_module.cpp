#include "quiet_coordinator_module.h"

#ifndef UNIT_TEST
#include "../../ble_client.h"
#include "../../packet_parser.h"
#include "../../perf_metrics.h"
#else
#include "../../../test/mocks/ble_client.h"
#include "../../../test/mocks/packet_parser.h"
#endif

const char* quietOwnerName(const QuietOwner owner) {
    switch (owner) {
        case QuietOwner::None:
            return "none";
        case QuietOwner::SpeedVolume:
            return "speed_volume";
        case QuietOwner::VolumeFade:
            return "volume_fade";
        case QuietOwner::TapGesture:
            return "tap_gesture";
        case QuietOwner::WifiCommand:
            return "wifi_command";
        case QuietOwner::AutoPush:
            return "auto_push";
        default:
            return "unknown";
    }
}

void QuietCoordinatorModule::begin(V1BLEClient* bleClient, PacketParser* parser) {
    ble_ = bleClient;
    parser_ = parser;
    reset();
}

void QuietCoordinatorModule::reset() {
    desired_ = QuietDesiredState{};
    committed_ = QuietCommittedState{};
    presentation_ = QuietPresentationState{};

    speedVolActive_ = false;
    speedVolSavedOriginal_ = 0xFF;
    speedVolSavedMuteVol_ = 0;
    pendingSpeedVolRestoreVol_ = 0xFF;
    pendingSpeedVolRestoreMuteVol_ = 0;
    pendingSpeedVolRestoreSetMs_ = 0;
    pendingSpeedVolRestoreLastRetryMs_ = 0;
    speedVolLastRetryMs_ = 0;

    syncCommittedState();
}

void QuietCoordinatorModule::syncCommittedState() {
    committed_.connected = ble_ ? ble_->isConnected() : false;
    committed_.hasDisplayState = false;
    committed_.muted = false;
    committed_.mainVolume = 0;
    committed_.muteVolume = 0;

    if (parser_) {
        const DisplayState& state = parser_->getDisplayState();
        committed_.hasDisplayState = true;
        committed_.muted = state.muted;
        committed_.mainVolume = state.mainVolume;
        committed_.muteVolume = state.muteVolume;
        presentation_.effectiveMuted = state.muted;
    } else {
        presentation_.effectiveMuted = false;
    }

    refreshPendingState();
}

void QuietCoordinatorModule::refreshPendingState() {
    if (desired_.mutePending && committed_.hasDisplayState && committed_.muted == desired_.mute) {
        desired_.mutePending = false;
    }
    if (desired_.volumePending && committed_.hasDisplayState && committed_.mainVolume == desired_.volume) {
        desired_.volumePending = false;
    }
}

QuietCommittedState QuietCoordinatorModule::getCommittedState() {
    syncCommittedState();
    return committed_;
}

bool QuietCoordinatorModule::sendMute(QuietOwner owner, bool muted) {
    syncCommittedState();

    desired_.muteOwner = owner;
    desired_.mute = muted;
    desired_.mutePending = committed_.hasDisplayState ? (committed_.muted != muted) : true;

    if (!ble_) {
        return false;
    }

    const bool sent = ble_->setMute(muted);
    if (sent) {
        presentation_.activeMuteOwner = muted ? owner : QuietOwner::None;
    }
    return sent;
}

bool QuietCoordinatorModule::sendVolume(QuietOwner owner, uint8_t volume, uint8_t muteVolume) {
    syncCommittedState();

    desired_.volumeOwner = owner;
    desired_.volume = volume;
    desired_.muteVolume = muteVolume;
    desired_.volumePending = committed_.hasDisplayState ? (committed_.mainVolume != volume) : true;

    if (!ble_) {
        return false;
    }

    const bool sent = ble_->setVolume(volume, muteVolume);
    if (sent) {
        presentation_.activeVolumeOwner = owner;
    }
    return sent;
}

bool QuietCoordinatorModule::retryPendingSpeedVolRestore(const uint32_t nowMs) {
    syncCommittedState();
    if (pendingSpeedVolRestoreVol_ == 0xFF || !ble_ || !parser_) {
        return false;
    }

    if ((nowMs - pendingSpeedVolRestoreSetMs_) >= SPEED_VOL_RESTORE_TIMEOUT_MS) {
        Serial.println("[SpeedVol] restore retry timeout");
        pendingSpeedVolRestoreVol_ = 0xFF;
        if (presentation_.activeVolumeOwner == QuietOwner::SpeedVolume) {
            presentation_.activeVolumeOwner = QuietOwner::None;
        }
        presentation_.speedVolZeroActive = false;
        return false;
    }

    if (committed_.mainVolume == pendingSpeedVolRestoreVol_) {
        pendingSpeedVolRestoreVol_ = 0xFF;
        if (!speedVolActive_ && presentation_.activeVolumeOwner == QuietOwner::SpeedVolume) {
            presentation_.activeVolumeOwner = QuietOwner::None;
        }
        presentation_.speedVolZeroActive = false;
        return false;
    }

    if ((nowMs - pendingSpeedVolRestoreLastRetryMs_) < SPEED_VOL_RETRY_INTERVAL_MS) {
        return true;
    }

    pendingSpeedVolRestoreLastRetryMs_ = nowMs;
    sendVolume(QuietOwner::SpeedVolume,
               pendingSpeedVolRestoreVol_,
               pendingSpeedVolRestoreMuteVol_);
#ifndef UNIT_TEST
    perfRecordSpeedVolRetry();
#endif
    return true;
}

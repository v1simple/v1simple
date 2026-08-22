import Foundation

/// One scenario-independent record of the exact packet plan handed to the BLE
/// peripheral for a replay sample. This is request evidence, not a physical
/// notification-delivery acknowledgment.
struct ReplayStimulusEvent: Encodable, Equatable {
    struct Alert: Encodable, Equatable {
        let band: String
        let bandMask: UInt8
        let frequencyMHz: UInt16
        let bars: Int
        let direction: String
        let priority: Bool

        init(_ alert: ReplayAlert) {
            band = alert.band.name
            bandMask = alert.band.mask
            frequencyMHz = alert.frequencyMHz
            bars = alert.strength
            direction = alert.direction.label
            priority = alert.isPriority
        }
    }

    struct ExpectedDisplay: Encodable, Equatable {
        let phase: String
        let alerts: [Alert]
        let muted: Bool
        let mainVolume: UInt8
        let muteVolume: UInt8
        let modeChar: String
        let displayOn: Bool
        let arrowBlink: Bool
    }

    struct NotificationRequest: Encodable, Equatable {
        let ordinal: Int
        let channel: String
        let kind: String
        let alertRowIndex: Int?
        let alertRowCount: Int?
        let bytesHex: String

        init(ordinal: Int, emission: V1.PlaybackPacketPlan.Emission) {
            self.ordinal = ordinal
            switch emission.channel {
            case .displayShort: channel = "display_short"
            case .displayLong: channel = "display_long"
            }
            switch emission.kind {
            case .alertRow(let index, let count):
                kind = "alert_row"
                alertRowIndex = index
                alertRowCount = count
            case .displayFrame:
                kind = "display_frame"
                alertRowIndex = nil
                alertRowCount = nil
            }
            bytesHex = emission.bytes.map { String(format: "%02X", $0) }.joined()
        }
    }

    let state = "stimulus_requested"
    let schemaVersion = 2
    let stimulusSequence: Int
    let sourceIndex: Int
    let replayOffsetSeconds: Double
    let intendedHostMonotonicNs: UInt64
    let requestedHostMonotonicSeconds: Double
    let requestedHostMonotonicNs: UInt64
    let expected: ExpectedDisplay
    let notifications: [NotificationRequest]

    init(sequence: Int,
         sample: TimedSample,
         controlState: V1.Session.ControlState,
         muted: Bool,
         displayOn: Bool,
         arrowBlink: Bool,
         plan: V1.PlaybackPacketPlan,
         intendedHostMonotonicNs: UInt64,
         requestedHostMonotonicNs: UInt64) {
        precondition(sequence > 0, "stimulus sequence must be positive")
        stimulusSequence = sequence
        sourceIndex = sample.sourceIndex
        replayOffsetSeconds = sample.offset
        self.intendedHostMonotonicNs = intendedHostMonotonicNs
        self.requestedHostMonotonicNs = requestedHostMonotonicNs
        requestedHostMonotonicSeconds =
            Double(requestedHostMonotonicNs) / 1_000_000_000.0
        expected = ExpectedDisplay(
            phase: sample.phase,
            alerts: sample.alerts.map(Alert.init),
            muted: muted,
            mainVolume: controlState.mainVolume,
            muteVolume: controlState.mutedVolume,
            modeChar: controlState.mode.displayCharacter,
            displayOn: displayOn,
            arrowBlink: arrowBlink
        )
        notifications = plan.emissions.enumerated().map {
            NotificationRequest(ordinal: $0.offset, emission: $0.element)
        }
    }

    var machineEventLine: String {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.sortedKeys, .withoutEscapingSlashes]
        guard let payload = try? encoder.encode(self),
              let json = String(data: payload, encoding: .utf8) else {
            preconditionFailure("replay stimulus event must be JSON encodable")
        }
        return "V1REPLAY_EVENT " + json
    }
}

extension V1.ModeGlyph {
    var displayCharacter: String {
        switch self {
        case .allBogeys: return "A"
        case .logic: return "l"
        case .advancedLogic: return "L"
        case .customSweeps: return "C"
        case .euroKaOnly: return "u"
        case .euroKaPhoto: return "U"
        }
    }
}

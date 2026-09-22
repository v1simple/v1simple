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
        let junk: Bool
        let photoType: UInt8?

        init(_ alert: ReplayAlert) {
            band = alert.band.name
            bandMask = alert.band.mask
            frequencyMHz = alert.frequencyMHz
            bars = alert.strength
            direction = alert.direction.label
            priority = alert.isPriority
            junk = alert.isJunk
            photoType = alert.photoType == 0 ? nil : alert.photoType
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
        let bandBlink: Bool
        let bogeyCounterChar: String
        let bogeyCounterOffChar: String
        let bogeyCounterBlink: Bool
        let bogeyCounterImage1: UInt8
        let bogeyCounterImage2: UInt8
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
    let schemaVersion = 4
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
         bogeyBlink: Bool = false,
         arrowBlink: Bool,
         bandBlink: Bool,
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
        let bogeyCounterChar: String
        let bogeyCounterImage1: UInt8
        let bogeyCounterImage2: UInt8
        if let counter = sample.bogeyCounterOverride {
            bogeyCounterChar = counter.displayCharacter
            bogeyCounterImage1 = counter.image1
            bogeyCounterImage2 = counter.image2
        } else if sample.alerts.contains(where: { $0.photoType != 0 }) {
            bogeyCounterChar = "P"
            bogeyCounterImage1 = 0x73
            bogeyCounterImage2 = bogeyBlink ? 0x00 : 0x73
        } else if !sample.alerts.isEmpty {
            bogeyCounterChar = String(sample.alerts.count)
            bogeyCounterImage1 = V1.bogeyGlyph(forCount: sample.alerts.count)
            bogeyCounterImage2 = bogeyBlink ? 0x00 : bogeyCounterImage1
        } else {
            bogeyCounterChar = controlState.mode.displayCharacter
            bogeyCounterImage1 = controlState.mode.rawValue
            bogeyCounterImage2 = bogeyCounterImage1
        }
        let bogeyCounterBlink = bogeyCounterImage1 != bogeyCounterImage2
        let bogeyCounterOffChar = bogeyCounterBlink ? " " : bogeyCounterChar
        expected = ExpectedDisplay(
            phase: sample.phase,
            alerts: sample.alerts.map(Alert.init),
            muted: muted,
            mainVolume: controlState.mainVolume,
            muteVolume: controlState.mutedVolume,
            modeChar: controlState.mode.displayCharacter,
            displayOn: displayOn,
            arrowBlink: arrowBlink,
            bandBlink: bandBlink,
            bogeyCounterChar: bogeyCounterChar,
            bogeyCounterOffChar: bogeyCounterOffChar,
            bogeyCounterBlink: bogeyCounterBlink,
            bogeyCounterImage1: bogeyCounterImage1,
            bogeyCounterImage2: bogeyCounterImage2
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

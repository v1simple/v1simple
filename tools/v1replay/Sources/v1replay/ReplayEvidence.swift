import Foundation
import CryptoKit

@inline(__always)
func hostMonotonicNanoseconds() -> UInt64 {
    return DispatchTime.now().uptimeNanoseconds
}

func sha256Hex(_ data: Data) -> String {
    return SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
}

func fnv1a32Hex(_ data: Data) -> String {
    var digest: UInt32 = 2_166_136_261
    for byte in data {
        digest ^= UInt32(byte)
        digest = digest &* 16_777_619
    }
    return String(format: "%08X", digest)
}

private func encodedMachineEventLine<T: Encodable>(_ event: T) -> String {
    let encoder = JSONEncoder()
    encoder.outputFormatting = [.sortedKeys, .withoutEscapingSlashes]
    guard let data = try? encoder.encode(event),
          let json = String(data: data, encoding: .utf8) else {
        preconditionFailure("replay evidence must be JSON encodable")
    }
    return "V1REPLAY_EVENT " + json
}

/// Stable identity assigned when one notification is appended to the
/// CoreBluetooth queue. A repeated payload receives a different global sequence.
struct ReplayNotificationIdentity: Equatable {
    let globalTxSequence: UInt64
    let stimulusSequence: Int?
    let emissionOrdinal: Int?
    let characteristic: String
    let payloadSha256: String
    let payloadFnv1a32: String
    let payloadHex: String
    let requestedHostMonotonicNs: UInt64

    init(globalTxSequence: UInt64,
         stimulusSequence: Int?,
         emissionOrdinal: Int?,
         characteristic: String,
         payload: Data,
         requestedHostMonotonicNs: UInt64) {
        precondition(globalTxSequence > 0, "global TX sequence must be positive")
        if let stimulusSequence {
            precondition(stimulusSequence > 0, "stimulus sequence must be positive")
        }
        if let emissionOrdinal {
            precondition(emissionOrdinal >= 0, "emission ordinal must be non-negative")
        }
        precondition(
            (stimulusSequence == nil) == (emissionOrdinal == nil),
            "stimulus sequence and emission ordinal must be paired"
        )
        self.globalTxSequence = globalTxSequence
        self.stimulusSequence = stimulusSequence
        self.emissionOrdinal = emissionOrdinal
        self.characteristic = characteristic
        self.payloadSha256 = sha256Hex(payload)
        self.payloadFnv1a32 = fnv1a32Hex(payload)
        self.payloadHex = payload.map { String(format: "%02X", $0) }.joined()
        self.requestedHostMonotonicNs = requestedHostMonotonicNs
    }

    var requestedEvent: ReplayNotificationEvent {
        return ReplayNotificationEvent(
            state: .requested,
            identity: self,
            hostMonotonicNs: requestedHostMonotonicNs
        )
    }

    func acceptedEvent(hostMonotonicNs: UInt64) -> ReplayNotificationEvent {
        return ReplayNotificationEvent(
            state: .accepted,
            identity: self,
            hostMonotonicNs: hostMonotonicNs
        )
    }
}

/// Request means queued by v1replay. Acceptance means
/// CBPeripheralManager.updateValue returned true; it is not proof that the DUT
/// received, parsed, or displayed the bytes.
struct ReplayNotificationEvent: Encodable, Equatable {
    enum State: String {
        case requested = "notification_requested"
        case accepted = "notification_accepted"
    }

    let state: String
    let schemaVersion = 1
    let globalTxSequence: UInt64
    let stimulusSequence: Int?
    let emissionOrdinal: Int?
    let characteristic: String
    let payloadSha256: String
    let payloadFnv1a32: String
    let payloadHex: String
    let hostMonotonicNs: UInt64

    init(state: State,
         identity: ReplayNotificationIdentity,
         hostMonotonicNs: UInt64) {
        self.state = state.rawValue
        self.globalTxSequence = identity.globalTxSequence
        self.stimulusSequence = identity.stimulusSequence
        self.emissionOrdinal = identity.emissionOrdinal
        self.characteristic = identity.characteristic
        self.payloadSha256 = identity.payloadSha256
        self.payloadFnv1a32 = identity.payloadFnv1a32
        self.payloadHex = identity.payloadHex
        self.hostMonotonicNs = hostMonotonicNs
    }

    var machineEventLine: String {
        return encodedMachineEventLine(self)
    }
}

private struct ResolvedScenarioDocument: Encodable {
    struct Alert: Encodable {
        let band: String
        let bandMask: UInt8
        let frequencyMHz: UInt16
        let strength: Int
        let direction: String
        let directionMask: UInt8
        let priority: Bool
    }

    struct Volume: Encodable {
        let main: UInt8
        let muted: UInt8
    }

    struct Sample: Encodable {
        let sourceIndex: Int
        let offsetSeconds: Double
        let phase: String
        let muted: Bool
        let alerts: [Alert]
        let detectorVolume: Volume?
        let detectorMode: String?
        let scenarioArrowBlink: Bool
    }

    let schemaVersion = 1
    let origin: String
    let samples: [Sample]

    init(encounter: Encounter) {
        origin = encounter.origin.evidenceName
        samples = encounter.samples.map { sample in
            Sample(
                sourceIndex: sample.sourceIndex,
                offsetSeconds: sample.offset,
                phase: sample.phase,
                muted: sample.muted,
                alerts: sample.alerts.map { alert in
                    Alert(
                        band: alert.band.name,
                        bandMask: alert.band.mask,
                        frequencyMHz: alert.frequencyMHz,
                        strength: alert.strength,
                        direction: alert.direction.label,
                        directionMask: alert.direction.rawValue,
                        priority: alert.isPriority
                    )
                },
                detectorVolume: sample.detectorVolume.map {
                    Volume(main: $0.mainVolume, muted: $0.muteVolume)
                },
                detectorMode: sample.detectorMode?.displayCharacter,
                scenarioArrowBlink: sample.scenarioArrowBlink
            )
        }
    }
}

struct ResolvedScenarioEvidence: Encodable, Equatable {
    let state = "scenario_resolved"
    let schemaVersion = 1
    let origin: String
    let sampleCount: Int
    let sha256: String
    let byteCount: Int

    var machineEventLine: String {
        return encodedMachineEventLine(self)
    }
}

struct ReplayConfiguredEvent: Encodable, Equatable {
    let state = "configured"
    let schemaVersion = 1
    let blinkProfile: String
    let blinkSource: String
    let blinkSamples: Int
    let totalSamples: Int
    let durationSeconds: Double
    let cadenceHz: Double?
    let scenarioOrigin: String
    let scenarioSha256: String

    var machineEventLine: String {
        return encodedMachineEventLine(self)
    }
}

extension Encounter {
    var uniformCadenceHz: Double? {
        guard samples.count > 1 else { return nil }
        let gap = samples[1].offset - samples[0].offset
        guard gap > 0 else { return nil }
        let tolerance = max(0.000_001, gap * 0.000_001)
        for index in 2..<samples.count {
            let nextGap = samples[index].offset - samples[index - 1].offset
            guard abs(nextGap - gap) <= tolerance else { return nil }
        }
        return 1.0 / gap
    }

    func resolvedScenarioEvidenceData() throws -> Data {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.sortedKeys, .withoutEscapingSlashes]
        return try encoder.encode(ResolvedScenarioDocument(encounter: self))
    }

    func writeResolvedScenarioEvidence(path: String) throws -> ResolvedScenarioEvidence {
        let data: Data
        do {
            data = try resolvedScenarioEvidenceData()
            try data.write(to: URL(fileURLWithPath: path), options: .atomic)
        } catch {
            throw ReplayError.message("resolved scenario evidence could not be written")
        }
        return ResolvedScenarioEvidence(
            origin: origin.evidenceName,
            sampleCount: samples.count,
            sha256: sha256Hex(data),
            byteCount: data.count
        )
    }
}

extension Encounter.Origin {
    var evidenceName: String {
        switch self {
        case .externalInput: return "external"
        case .syntheticDemo: return "synthetic_demo"
        case .syntheticBench: return "synthetic_bench"
        }
    }
}

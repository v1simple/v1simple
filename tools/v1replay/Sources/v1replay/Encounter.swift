import Foundation

// =============================================================================
// External replay input -> a timed packet schedule.
//
// Input files are private runtime material. They must live outside every Git
// checkout; this tool never copies them, names them in output, or writes a
// normalized copy. The built-in demo is generated in memory.
// =============================================================================

private struct EncounterFile: Decodable {
    let band: String
    let frequencyMHz: Int
    let samples: [Sample]

    struct Sample: Decodable {
        let offsetSeconds: Double?
        let timestamp: String?
        let strength: Int
        let frequencyGHz: Double?
        let direction: String
        let muteState: String?
    }
}

/// One active alert row in a replay step.
struct ReplayAlert {
    let band: V1.Band
    let frequencyMHz: UInt16
    let strength: Int
    let direction: V1.Direction
    let isPriority: Bool

    fileprivate func hasSameState(as other: ReplayAlert) -> Bool {
        return band.mask == other.band.mask
            && frequencyMHz == other.frequencyMHz
            && strength == other.strength
            && direction == other.direction
            && isPriority == other.isPriority
    }
}

/// One detector-authored current-volume pair carried by infDisplayData aux2.
/// Saved-volume state is deliberately outside this replay stimulus.
struct DetectorVolume: Equatable {
    let mainVolume: UInt8
    let muteVolume: UInt8

    init(mainVolume: UInt8, muteVolume: UInt8) {
        precondition(mainVolume <= 9, "detector main volume must be 0...9")
        precondition(muteVolume <= 9, "detector mute volume must be 0...9")
        self.mainVolume = mainVolume
        self.muteVolume = muteVolume
    }
}

/// The bounded machine-readable record emitted when an authored detector
/// volume begins. This is intentionally specific to the fixed bench stimulus.
struct DetectorVolumeCheckpoint: Equatable {
    let replaySecond: Int
    let volume: DetectorVolume

    init(replaySecond: Int, mainVolume: UInt8, muteVolume: UInt8) {
        precondition(replaySecond >= 0, "replay second must be non-negative")
        self.replaySecond = replaySecond
        self.volume = DetectorVolume(mainVolume: mainVolume, muteVolume: muteVolume)
    }

    var mainVolume: UInt8 { return volume.mainVolume }
    var muteVolume: UInt8 { return volume.muteVolume }

    var machineEventLine: String {
        return "V1REPLAY_EVENT {\"state\":\"detector_volume\","
            + "\"replaySecond\":\(replaySecond),"
            + "\"mainVolume\":\(mainVolume),"
            + "\"muteVolume\":\(muteVolume)}"
    }
}

/// One modeled physical-V1 mute edge in the fixed bench stimulus.
struct DetectorMuteCheckpoint: Equatable {
    let replaySecond: Int
    let muted: Bool

    init(replaySecond: Int, muted: Bool) {
        precondition(replaySecond >= 0, "replay second must be non-negative")
        self.replaySecond = replaySecond
        self.muted = muted
    }

    var machineEventLine: String {
        return "V1REPLAY_EVENT {\"state\":\"detector_mute\","
            + "\"replaySecond\":\(replaySecond),"
            + "\"muted\":\(muted ? "true" : "false")}"
    }
}

/// One modeled physical-V1 mode edge in the fixed bench stimulus.
struct DetectorModeCheckpoint: Equatable {
    let replaySecond: Int
    let mode: V1.ModeGlyph

    init(replaySecond: Int, mode: V1.ModeGlyph) {
        precondition(replaySecond >= 0, "replay second must be non-negative")
        self.replaySecond = replaySecond
        self.mode = mode
    }

    var modeChar: String {
        switch mode {
        case .allBogeys: return "A"
        case .logic: return "l"
        case .advancedLogic: return "L"
        case .customSweeps: return "C"
        case .euroKaOnly: return "u"
        case .euroKaPhoto: return "U"
        }
    }

    var machineEventLine: String {
        return "V1REPLAY_EVENT {\"state\":\"detector_mode\","
            + "\"replaySecond\":\(replaySecond),"
            + "\"modeChar\":\"\(modeChar)\"}"
    }
}

/// One replay step: the complete ordered alert table to transmit, and when.
struct TimedSample {
    let offset: TimeInterval
    let phase: String
    let muted: Bool
    let alerts: [ReplayAlert]
    let detectorVolume: DetectorVolume?
    let detectorMode: V1.ModeGlyph?
    let scenarioArrowBlink: Bool
    let sourceIndex: Int

    init(offset: TimeInterval,
         phase: String,
         muted: Bool,
         alerts: [ReplayAlert],
         detectorVolume: DetectorVolume? = nil,
         detectorMode: V1.ModeGlyph? = nil,
         scenarioArrowBlink: Bool = false,
         sourceIndex: Int) {
        precondition((0...3).contains(alerts.count), "replay steps support zero through three alerts")
        let priorityCount = alerts.reduce(0) { $0 + ($1.isPriority ? 1 : 0) }
        precondition(alerts.isEmpty || priorityCount == 1,
                     "non-empty replay steps require exactly one priority alert")
        precondition(!scenarioArrowBlink || priorityCount == 1,
                     "scenario arrow blink requires a priority alert")

        self.offset = offset
        self.phase = phase
        self.muted = muted
        self.alerts = alerts
        self.detectorVolume = detectorVolume
        self.detectorMode = detectorMode
        self.scenarioArrowBlink = scenarioArrowBlink
        self.sourceIndex = sourceIndex
    }

    var priorityAlert: ReplayAlert? {
        return alerts.first(where: { $0.isPriority })
    }

    var secondaryAlerts: [ReplayAlert] {
        return alerts.filter { !$0.isPriority }
    }

    fileprivate func hasSameState(as other: TimedSample) -> Bool {
        guard phase == other.phase,
              muted == other.muted,
              detectorVolume == other.detectorVolume,
              detectorMode == other.detectorMode,
              scenarioArrowBlink == other.scenarioArrowBlink,
              alerts.count == other.alerts.count else {
            return false
        }
        return zip(alerts, other.alerts).allSatisfy { $0.hasSameState(as: $1) }
    }
}

/// Selects how the priority-arrow blink plane is authored for playback.
enum ArrowBlinkProfile: String, CaseIterable {
    /// Use the blink intent embedded in the generated scenario.
    case scenario
    /// Keep image1 and image2 identical as a negative control.
    case steady
    /// Blink every priority arrow as a worst-case repaint control.
    case stress

    static func named(_ value: String) -> ArrowBlinkProfile? {
        return ArrowBlinkProfile(rawValue: value.lowercased())
    }

    func shouldBlink(_ sample: TimedSample) -> Bool {
        guard sample.priorityAlert != nil else { return false }
        switch self {
        case .scenario: return sample.scenarioArrowBlink
        case .steady: return false
        case .stress: return true
        }
    }

    func sampleCount(in encounter: Encounter) -> Int {
        return encounter.samples.filter(shouldBlink).count
    }

    var sourceLabel: String {
        switch self {
        case .scenario: return "generated_multi_alert_assumption"
        case .steady, .stress: return "explicit_control"
        }
    }
}

struct Encounter {
    enum Origin {
        case externalInput
        case syntheticDemo
        case syntheticBench

        var label: String {
            switch self {
            case .externalInput: return "external input"
            case .syntheticDemo: return "synthetic demo"
            case .syntheticBench: return "synthetic bench"
            }
        }
    }

    let origin: Origin
    let samples: [TimedSample]

    var duration: TimeInterval {
        return samples.last?.offset ?? 0
    }

    /// Load private runtime input without retaining identifying metadata.
    ///
    /// Relative `offsetSeconds` values are preferred. Legacy timestamp fields
    /// are accepted only to reconstruct cadence, then discarded immediately.
    static func loadExternal(path: String, mutedWhen: String) throws -> Encounter {
        let url = try ExternalInputPolicy.validate(path: path)

        let file: EncounterFile
        do {
            let data = try Data(contentsOf: url)
            file = try JSONDecoder().decode(EncounterFile.self, from: data)
        } catch {
            throw ReplayError.message("external replay input could not be read or decoded")
        }

        guard !file.samples.isEmpty else {
            throw ReplayError.message("external replay input contains no samples")
        }
        guard let band = V1.Band.named(file.band) else {
            throw ReplayError.message("external replay input contains an unknown band")
        }

        let offsets = try makeOffsets(for: file.samples)
        var timed: [TimedSample] = []
        timed.reserveCapacity(file.samples.count)

        for (index, sample) in file.samples.enumerated() {
            let mhz: UInt16
            if let ghz = sample.frequencyGHz, ghz > 0 {
                mhz = UInt16(clamping: Int((ghz * 1000.0).rounded()))
            } else {
                mhz = UInt16(clamping: file.frequencyMHz)
            }
            timed.append(TimedSample(
                offset: offsets[index],
                phase: "external",
                muted: (sample.muteState ?? "") == mutedWhen,
                alerts: [ReplayAlert(
                    band: band,
                    frequencyMHz: mhz,
                    strength: sample.strength,
                    direction: V1.Direction.named(sample.direction),
                    isPriority: true
                )],
                sourceIndex: index
            ))
        }

        return Encounter(
            origin: .externalInput,
            samples: timed
        )
    }

    /// A deterministic, generated stimulus for demos and routine bench checks.
    /// No capture-derived values or fixture files are involved.
    static func syntheticDemo() -> Encounter {
        let cadence = 1.0 / 3.0
        let count = 72
        let samples = (0..<count).map { index -> TimedSample in
            let phase = index % 24
            let bars = phase < 12 ? 1 + phase * 7 / 11 : 1 + (23 - phase) * 7 / 11
            let direction: V1.Direction
            switch index / 24 {
            case 0: direction = .front
            case 1: direction = .side
            default: direction = .rear
            }
            return TimedSample(
                offset: Double(index) * cadence,
                phase: "demo",
                muted: false,
                alerts: [ReplayAlert(
                    band: .ka,
                    frequencyMHz: 34_700,
                    strength: bars,
                    direction: direction,
                    isPriority: true
                )],
                sourceIndex: index
            )
        }
        return Encounter(
            origin: .syntheticDemo,
            samples: samples
        )
    }

    static func idle() -> Encounter {
        return Encounter(origin: .syntheticDemo, samples: [])
    }

    /// Build relative timing without retaining absolute timestamps.
    private static func makeOffsets(for samples: [EncounterFile.Sample]) throws -> [Double] {
        if samples.allSatisfy({ $0.offsetSeconds != nil }) {
            let offsets = samples.map { $0.offsetSeconds! }
            guard offsets.allSatisfy({ $0.isFinite && $0 >= 0 }),
                  zip(offsets, offsets.dropFirst()).allSatisfy({ $0 <= $1 }) else {
                throw ReplayError.message("external replay input has invalid relative timing")
            }
            return offsets
        }

        let formatter = ISO8601DateFormatter()
        formatter.formatOptions = [.withInternetDateTime]
        let absolute = samples.compactMap { sample -> Double? in
            guard let timestamp = sample.timestamp,
                  let date = formatter.date(from: timestamp) else { return nil }
            return date.timeIntervalSince1970
        }
        guard absolute.count == samples.count, let base = absolute.first else {
            throw ReplayError.message(
                "external replay input needs relative offsets or parseable timestamps"
            )
        }

        var offsets = absolute.map { $0 - base }

        // Whole-second exports often contain same-second runs. Spread those
        // samples evenly while preserving genuine gaps.
        var runStart = 0
        while runStart < offsets.count {
            var runEnd = runStart
            while runEnd + 1 < offsets.count && offsets[runEnd + 1] == offsets[runStart] {
                runEnd += 1
            }
            let runLength = runEnd - runStart + 1
            if runLength > 1 {
                let span: Double
                if runEnd + 1 < offsets.count {
                    span = min(1.0, offsets[runEnd + 1] - offsets[runStart])
                } else {
                    span = 1.0
                }
                let step = span / Double(runLength)
                for k in 0..<runLength {
                    offsets[runStart + k] = offsets[runStart] + step * Double(k)
                }
            }
            runStart = runEnd + 1
        }
        return offsets
    }

    /// Re-time the whole input to a fixed cadence (`--rate`).
    func retimed(toHz hz: Double) -> Encounter {
        guard hz > 0 else { return self }
        let step = 1.0 / hz
        let resampled = samples.enumerated().map { index, sample in
            TimedSample(offset: Double(index) * step,
                        phase: sample.phase,
                        muted: sample.muted,
                        alerts: sample.alerts,
                        detectorVolume: sample.detectorVolume,
                        detectorMode: sample.detectorMode,
                        scenarioArrowBlink: sample.scenarioArrowBlink,
                        sourceIndex: sample.sourceIndex)
        }
        return Encounter(origin: origin, samples: resampled)
    }

    var strengthHistogram: [(bars: Int, count: Int)] {
        var counts: [Int: Int] = [:]
        for sample in samples {
            guard let priority = sample.priorityAlert else { continue }
            counts[priority.strength, default: 0] += 1
        }
        return counts.sorted { $0.key < $1.key }.map { (bars: $0.key, count: $0.value) }
    }

    var changeIndices: [Int] {
        var result: [Int] = []
        var previous: TimedSample?
        for (index, sample) in samples.enumerated() {
            if previous.map({ sample.hasSameState(as: $0) }) != true {
                result.append(index)
            }
            previous = sample
        }
        return result
    }

    func detectorVolumeCheckpoint(at index: Int) -> DetectorVolumeCheckpoint? {
        guard samples.indices.contains(index),
              let volume = samples[index].detectorVolume else { return nil }
        let previous = index > samples.startIndex ? samples[index - 1].detectorVolume : nil
        guard previous != volume else { return nil }
        return DetectorVolumeCheckpoint(
            replaySecond: Int(samples[index].offset),
            mainVolume: volume.mainVolume,
            muteVolume: volume.muteVolume
        )
    }

    var detectorVolumeCheckpoints: [DetectorVolumeCheckpoint] {
        return samples.indices.compactMap(detectorVolumeCheckpoint(at:))
    }

    func detectorMuteCheckpoint(at index: Int) -> DetectorMuteCheckpoint? {
        guard samples.indices.contains(index) else { return nil }
        let muted = samples[index].muted
        let previous = index > samples.startIndex ? samples[index - 1].muted : false
        guard muted != previous else { return nil }
        return DetectorMuteCheckpoint(
            replaySecond: Int(samples[index].offset),
            muted: muted
        )
    }

    var detectorMuteCheckpoints: [DetectorMuteCheckpoint] {
        return samples.indices.compactMap(detectorMuteCheckpoint(at:))
    }

    func detectorModeCheckpoint(at index: Int) -> DetectorModeCheckpoint? {
        guard samples.indices.contains(index),
              let mode = samples[index].detectorMode else { return nil }
        let previous = index > samples.startIndex ? samples[index - 1].detectorMode : nil
        guard mode != previous else { return nil }
        return DetectorModeCheckpoint(
            replaySecond: Int(samples[index].offset),
            mode: mode
        )
    }

    var detectorModeCheckpoints: [DetectorModeCheckpoint] {
        return samples.indices.compactMap(detectorModeCheckpoint(at:))
    }
}

private enum ExternalInputPolicy {
    static func validate(path: String) throws -> URL {
        let url = URL(fileURLWithPath: path).standardizedFileURL.resolvingSymlinksInPath()
        var isDirectory: ObjCBool = false
        guard FileManager.default.fileExists(atPath: url.path, isDirectory: &isDirectory),
              !isDirectory.boolValue else {
            throw ReplayError.message("external replay input was not found")
        }
        guard containingGitCheckout(url) == nil else {
            throw ReplayError.message(
                "refusing replay input inside a Git checkout; keep captures in private external storage"
            )
        }
        return url
    }

    private static func containingGitCheckout(_ fileURL: URL) -> URL? {
        var directory = fileURL.deletingLastPathComponent()
        while true {
            let marker = directory.appendingPathComponent(".git")
            if FileManager.default.fileExists(atPath: marker.path) {
                return directory
            }
            let parent = directory.deletingLastPathComponent()
            if parent.path == directory.path { return nil }
            directory = parent
        }
    }
}

enum ReplayError: Error, CustomStringConvertible {
    case message(String)

    var description: String {
        switch self {
        case .message(let text): return text
        }
    }
}

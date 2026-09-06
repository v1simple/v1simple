import Foundation

/// Ordinary V1 wire input for observing the configured persistence behavior.
/// The durations provide recording opportunities; they are not response limits.
/// Set persistence using the device's normal controls and retain its CFG evidence.
enum PersistenceScenario {
    static let durationSeconds = 64

    static func make() -> Encounter {
        func alert(_ band: V1.Band, _ frequency: UInt16, _ strength: Int,
                   _ direction: V1.Direction, priority: Bool = true) -> ReplayAlert {
            ReplayAlert(band: band, frequencyMHz: frequency, strength: strength,
                        direction: direction, isPriority: priority)
        }
        let k = alert(.k, 24_150, 4, .front)
        let x = alert(.x, 10_525, 2, .rear)
        let ka = alert(.ka, 34_700, 6, .side)
        let xCard = alert(.x, 10_525, 2, .rear, priority: false)
        let phases: [(Int, String, [ReplayAlert])] = [
            (6, "persistence_initial_idle", []),
            (10, "persistence_primary_seed", [k]),
            (18, "persistence_primary_release", []),
            (22, "persistence_preemption_seed", [x]),
            (23, "persistence_preemption_release", []),
            (31, "persistence_live_preemption", [ka]),
            (35, "persistence_live_secondary", [ka, xCard]),
            (43, "persistence_secondary_release", [ka]),
            (51, "persistence_priority_replacement", [k]),
            (55, "persistence_live_secondary_again", [k, xCard]),
            (64, "persistence_final_release", []),
        ]
        let samples = (0..<(durationSeconds * BenchScenario.cadenceHz)).map { tick in
            let second = tick / BenchScenario.cadenceHz
            let phase = phases.first { second < $0.0 }!
            return TimedSample(offset: Double(tick) / Double(BenchScenario.cadenceHz),
                               phase: phase.1, muted: false, alerts: phase.2,
                               sourceIndex: tick)
        }
        return Encounter(origin: .syntheticBench, samples: samples)
    }
}

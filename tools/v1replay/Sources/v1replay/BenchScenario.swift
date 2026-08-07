import Foundation

// =============================================================================
// Deterministic, generated bench stimulus.
//
// The long approach uses only the documented aggregate cadence, durations, and
// strength envelope. Its direction transitions are deliberately authored here;
// no private input or sample-for-sample fixture is embedded in this source.
// =============================================================================

enum BenchScenario {
    static let cadenceHz = 3
    static let durationSeconds = 254

    private static func alert(_ band: V1.Band,
                              _ frequencyMHz: UInt16,
                              _ strength: Int,
                              _ direction: V1.Direction,
                              priority: Bool) -> ReplayAlert {
        return ReplayAlert(band: band,
                           frequencyMHz: frequencyMHz,
                           strength: strength,
                           direction: direction,
                           isPriority: priority)
    }

    private static func triangleBars(tick: Int, count: Int) -> Int {
        guard count > 1 else { return 1 }
        let distance = min(tick, count - 1 - tick)
        let fraction = Double(distance) / (Double(count - 1) / 2.0)
        return 1 + Int((fraction * 5.0).rounded())
    }

    private static func ramp(from start: Int, to end: Int, tick: Int, count: Int) -> Int {
        guard count > 1 else { return end }
        let fraction = Double(tick) / Double(count - 1)
        return Int((Double(start) + Double(end - start) * fraction).rounded())
    }

    private static func sweepDirection(tick: Int, count: Int) -> V1.Direction {
        let third = max(1, count / 3)
        if tick < third { return .front }
        if tick < third * 2 { return .side }
        return .rear
    }

    private static func dukeBars(tick: Int) -> Int {
        switch tick {
        case 0..<(95 * cadenceHz):
            // Six one-second pulses in 95 seconds keep this region mostly at one.
            return tick >= 45 && tick % 45 < cadenceHz ? 2 : 1
        case (95 * cadenceHz)..<(100 * cadenceHz):
            return ramp(from: 1, to: 2,
                        tick: tick - 95 * cadenceHz, count: 5 * cadenceHz)
        case (100 * cadenceHz)..<(120 * cadenceHz):
            return ramp(from: 2, to: 3,
                        tick: tick - 100 * cadenceHz, count: 20 * cadenceHz)
        case (120 * cadenceHz)..<(140 * cadenceHz):
            return 6
        case (140 * cadenceHz)..<(145 * cadenceHz):
            return ramp(from: 6, to: 2,
                        tick: tick - 140 * cadenceHz, count: 5 * cadenceHz)
        case (145 * cadenceHz)..<(150 * cadenceHz):
            return ramp(from: 2, to: 1,
                        tick: tick - 145 * cadenceHz, count: 5 * cadenceHz)
        default:
            return 1
        }
    }

    private static func dukeDirection(tick: Int) -> V1.Direction {
        // These broad transitions are authored synthetic stimulus. The private
        // input's exact direction sequence is not available in this repository.
        switch tick {
        case 0..<(95 * cadenceHz): return .front
        case (95 * cadenceHz)..<(120 * cadenceHz): return .side
        case (120 * cadenceHz)..<(140 * cadenceHz): return .front
        case (140 * cadenceHz)..<(160 * cadenceHz): return .side
        default: return .rear
        }
    }

    static func make() -> Encounter {
        let sampleCount = durationSeconds * cadenceHz
        var samples: [TimedSample] = []
        samples.reserveCapacity(sampleCount)

        for tick in 0..<sampleCount {
            let second = tick / cadenceHz
            let phase: String
            let alerts: [ReplayAlert]

            switch second {
            case 0..<5:
                phase = "idle"
                alerts = []

            case 5..<17:
                phase = "k_encounter"
                let local = tick - 5 * cadenceHz
                let count = 12 * cadenceHz
                alerts = [alert(.k, 24_150, triangleBars(tick: local, count: count),
                                sweepDirection(tick: local, count: count), priority: true)]

            case 17..<29:
                phase = "ka_encounter"
                let local = tick - 17 * cadenceHz
                let count = 12 * cadenceHz
                alerts = [alert(.ka, 35_500, triangleBars(tick: local, count: count),
                                sweepDirection(tick: local, count: count), priority: true)]

            case 29..<33:
                phase = "priority_handoff"
                alerts = [alert(.k, 24_150, 4, .side, priority: true)]

            case 33..<39:
                phase = "priority_handoff"
                alerts = [
                    alert(.k, 24_150, 4, .side, priority: false),
                    alert(.ka, 34_700, 5, .front, priority: true),
                ]

            case 39..<49:
                phase = "three_bogeys"
                alerts = [
                    alert(.k, 24_150, 4, .side, priority: false),
                    alert(.ka, 34_700, 6, .front, priority: true),
                    alert(.ka, 35_500, 4, .rear, priority: false),
                ]

            case 49..<52:
                phase = "handoff_clear"
                alerts = [
                    alert(.k, 24_150, 4, .side, priority: false),
                    alert(.ka, 34_700, 5, .front, priority: true),
                ]

            case 52..<56:
                phase = "handoff_clear"
                alerts = [alert(.k, 24_150, 4, .side, priority: true)]

            case 56..<59:
                phase = "handoff_clear"
                alerts = []

            case 59..<244:
                phase = "duke_shaped_approach"
                let local = tick - 59 * cadenceHz
                alerts = [alert(.ka, 34_700, dukeBars(tick: local),
                                dukeDirection(tick: local), priority: true)]

            default:
                phase = "idle_tail"
                alerts = []
            }

            // Provisional until real-V1 display-frame evidence establishes the
            // detector's exact policy: blink the selected arrow while multiple
            // alerts are active, and keep single-alert periods steady.
            let scenarioArrowBlink = alerts.count > 1
            samples.append(TimedSample(offset: Double(tick) / Double(cadenceHz),
                                       phase: phase,
                                       muted: false,
                                       alerts: alerts,
                                       scenarioArrowBlink: scenarioArrowBlink,
                                       sourceIndex: tick))
        }

        validate(samples)
        return Encounter(origin: .syntheticBench, samples: samples)
    }

    /// Keep the generated scenario self-checking without storing a fixture.
    private static func validate(_ samples: [TimedSample]) {
        precondition(samples.count == durationSeconds * cadenceHz)
        for (index, sample) in samples.enumerated() {
            precondition(sample.sourceIndex == index)
            precondition(abs(sample.offset - Double(index) / Double(cadenceHz)) < 0.000_001)
        }

        let phaseCounts = Dictionary(grouping: samples, by: { $0.phase }).mapValues(\.count)
        precondition(phaseCounts == [
            "idle": 15,
            "k_encounter": 36,
            "ka_encounter": 36,
            "priority_handoff": 30,
            "three_bogeys": 30,
            "handoff_clear": 30,
            "duke_shaped_approach": 555,
            "idle_tail": 30,
        ])
        precondition(samples.filter { !$0.alerts.isEmpty }.count == 708)
        precondition(samples.filter { $0.alerts.count == 3 }.count == 30)
        precondition(samples.filter(\.scenarioArrowBlink).count == 57)
        precondition(samples.filter(\.scenarioArrowBlink).allSatisfy { $0.alerts.count > 1 })
        precondition(samples[(33 * cadenceHz)..<(52 * cadenceHz)]
            .allSatisfy(\.scenarioArrowBlink))
        precondition(samples[..<(33 * cadenceHz)].allSatisfy { !$0.scenarioArrowBlink })
        precondition(samples[(52 * cadenceHz)...].allSatisfy { !$0.scenarioArrowBlink })

        let handoff = samples[33 * cadenceHz]
        precondition(handoff.alerts.count == 2)
        precondition(handoff.alerts[0].frequencyMHz == 24_150 && !handoff.alerts[0].isPriority)
        precondition(handoff.alerts[1].frequencyMHz == 34_700 && handoff.alerts[1].isPriority)

        let threeBogeys = samples[39 * cadenceHz]
        precondition(threeBogeys.alerts.map(\.frequencyMHz) == [24_150, 34_700, 35_500])
        precondition(threeBogeys.alerts.map(\.isPriority) == [false, true, false])

        let dukeStart = 59 * cadenceHz
        let first95 = samples[dukeStart..<(dukeStart + 95 * cadenceHz)]
        precondition(first95.filter { $0.priorityAlert?.strength == 1 }.count > first95.count * 9 / 10)
        precondition(first95.contains { $0.priorityAlert?.strength == 2 })

        let plateauStart = dukeStart + 120 * cadenceHz
        let plateauEnd = dukeStart + 140 * cadenceHz
        precondition(samples[plateauStart..<plateauEnd].allSatisfy { $0.priorityAlert?.strength == 6 })
        precondition(samples[(244 * cadenceHz)...].allSatisfy { $0.alerts.isEmpty })
    }

    static func expectedCSV(for encounter: Encounter) -> String {
        var lines = [
            "offset_s,phase,active_alert_count,priority_frequency_mhz,priority_band,priority_direction,priority_bars,scenario_arrow_blink,card_1_frequency_mhz,card_1_direction,card_1_bars,card_2_frequency_mhz,card_2_direction,card_2_bars"
        ]
        lines.reserveCapacity(encounter.samples.count + 1)

        for sample in encounter.samples {
            var fields = [
                String(format: "%.3f", locale: Locale(identifier: "en_US_POSIX"), sample.offset),
                sample.phase,
                String(sample.alerts.count),
            ]

            if let priority = sample.priorityAlert {
                fields.append(contentsOf: [
                    String(priority.frequencyMHz),
                    bandLabel(priority.band),
                    priority.direction.label,
                    String(priority.strength),
                ])
            } else {
                fields.append(contentsOf: ["", "", "", ""])
            }
            fields.append(sample.scenarioArrowBlink ? "1" : "0")

            let cards = Array(sample.secondaryAlerts.prefix(2))
            for index in 0..<2 {
                if index < cards.count {
                    let card = cards[index]
                    fields.append(contentsOf: [
                        String(card.frequencyMHz),
                        card.direction.label,
                        String((card.strength * 6 + 4) / 8),
                    ])
                } else {
                    fields.append(contentsOf: ["", "", ""])
                }
            }

            precondition(fields.count == 14)
            lines.append(fields.joined(separator: ","))
        }
        return lines.joined(separator: "\n")
    }

    private static func bandLabel(_ band: V1.Band) -> String {
        switch band.mask {
        case V1.Band.k.mask: return "K"
        case V1.Band.ka.mask: return "Ka"
        default: return band.name.uppercased()
        }
    }
}

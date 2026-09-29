import Foundation

/// A finite closed-loop profile exercise. This authors RF rows, not expected
/// DUT mute commands. See README's profile-controls profile and phase table.
enum ProfileControlsScenario {
    static let cadenceHz = 3
    static let durationSeconds = 89

    private struct Segment {
        let phase: String
        let seconds: Int
        let alerts: [ReplayAlert]
        let muted: Bool

        init(_ phase: String, _ seconds: Int, _ alerts: [ReplayAlert] = [],
             muted: Bool = false) {
            self.phase = phase
            self.seconds = seconds
            self.alerts = alerts
            self.muted = muted
        }
    }

    private static func radar(_ band: V1.Band, _ frequency: UInt16,
                              priority: Bool = true) -> ReplayAlert {
        ReplayAlert(band: band, frequencyMHz: frequency, strength: 6,
                    direction: priority ? .front : .rear, isPriority: priority)
    }

    static func make() -> Encounter {
        let segments: [Segment] = [
            // The lead permits an already-staged AutoPush to finish; evidence
            // must still establish completion before the first active row.
            Segment("profile_controls_idle_lead", 12),
            Segment("box_k_lower_outside", 3, [radar(.k, 24_049)]),
            Segment("box_k_lower_edge", 3, [radar(.k, 24_050)]),
            Segment("box_k_upper_edge", 3, [radar(.k, 24_150)]),
            Segment("box_k_upper_outside", 3, [radar(.k, 24_151)]),
            Segment("box_mixed_outside_inside", 3, [
                radar(.k, 24_151), radar(.k, 24_100, priority: false),
            ]),
            Segment("box_clear_after_mixed", 2),
            Segment("box_outside_before_unselected", 3, [radar(.k, 24_049)]),
            Segment("box_mixed_unselected_ka", 3, [
                radar(.k, 24_049), radar(.ka, 33_950, priority: false),
            ]),
            Segment("box_unselected_ka_alone", 3, [radar(.ka, 33_950)]),
            Segment("box_clear_after_unselected", 2),
            Segment("box_outside_before_laser", 3, [radar(.k, 24_049)]),
            Segment("box_mixed_laser", 3, [
                radar(.laser, 0), radar(.k, 24_049, priority: false),
            ]),
            Segment("box_clear_after_laser", 2),
            Segment("box_outside_before_clear", 3, [radar(.k, 24_049)]),
            Segment("box_clear_release", 2),
            // All eight frequencies remain within physical K/Ka domains.
            // Exclusion here is performed by Session's modeled scan filter.
            Segment("scan_k_lower_admitted", 3, [radar(.k, 24_000)]),
            Segment("scan_k_below_excluded", 3, [radar(.k, 23_999)]),
            Segment("scan_k_upper_admitted", 3, [radar(.k, 24_200)]),
            Segment("scan_k_above_excluded", 3, [radar(.k, 24_201)]),
            Segment("scan_ka_lower_admitted", 3, [radar(.ka, 33_900)]),
            Segment("scan_ka_below_excluded", 3, [radar(.ka, 33_899)]),
            Segment("scan_ka_upper_admitted", 3, [radar(.ka, 35_000)]),
            Segment("scan_ka_above_excluded", 3, [radar(.ka, 35_001)]),
            Segment("box_clear_before_seed", 2),
            // Exactly one modeled detector mute edge. Keeping the authored
            // value true through the tail avoids a scripted OFF masking a
            // missing DUT unmute command on the new inside encounter.
            Segment("box_preexisting_detector_mute_seed", 3,
                    [radar(.ka, 33_950)], muted: true),
            Segment("box_new_inside_releases_preexisting_mute", 3,
                    [radar(.k, 24_100)], muted: true),
            Segment("profile_controls_idle_tail", 4, muted: true),
        ]
        var samples: [TimedSample] = []
        samples.reserveCapacity(durationSeconds * cadenceHz)
        for segment in segments {
            for _ in 0..<(segment.seconds * cadenceHz) {
                let tick = samples.count
                samples.append(TimedSample(
                    offset: Double(tick) / Double(cadenceHz),
                    phase: segment.phase, muted: segment.muted,
                    alerts: segment.alerts, sourceIndex: tick))
            }
        }
        precondition(samples.count == durationSeconds * cadenceHz)
        return Encounter(origin: .syntheticBench, samples: samples)
    }
}

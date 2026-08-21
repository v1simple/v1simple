import XCTest
@testable import v1replay

final class V1ReplayStimulusEventTests: XCTestCase {
    func testStimulusEventOwnsExactPlanAndResolvedState() throws {
        let sample = TimedSample(
            offset: 12.0,
            phase: "changed_scenario",
            muted: true,
            alerts: [ReplayAlert(
                band: .ka,
                frequencyMHz: 34_700,
                strength: 6,
                direction: .rear,
                isPriority: true
            )],
            detectorVolume: DetectorVolume(mainVolume: 7, muteVolume: 2),
            detectorMode: .allBogeys,
            scenarioArrowBlink: true,
            sourceIndex: 41
        )
        var config = V1.Session.Config()
        config.mode = .allBogeys
        config.mainVolume = 7
        config.mutedVolume = 2
        let control = V1.Session(config: config).controlState
        let plan = V1.PlaybackPacketPlan(
            sample: sample,
            controlState: control,
            displayOn: true,
            muted: true,
            blinkBogey: false,
            blinkArrow: true
        )
        let event = ReplayStimulusEvent(
            sequence: 9,
            sample: sample,
            controlState: control,
            muted: true,
            displayOn: true,
            arrowBlink: true,
            plan: plan,
            requestedHostMonotonicNs: 123_500_000_000
        )

        XCTAssertEqual(event.state, "stimulus_requested")
        XCTAssertEqual(event.schemaVersion, 1)
        XCTAssertEqual(event.stimulusSequence, 9)
        XCTAssertEqual(event.sourceIndex, 41)
        XCTAssertEqual(event.requestedHostMonotonicNs, 123_500_000_000)
        XCTAssertEqual(event.requestedHostMonotonicSeconds, 123.5)
        XCTAssertEqual(event.expected.phase, "changed_scenario")
        XCTAssertEqual(event.expected.modeChar, "A")
        XCTAssertEqual(event.expected.mainVolume, 7)
        XCTAssertEqual(event.expected.muteVolume, 2)
        XCTAssertTrue(event.expected.muted)
        XCTAssertTrue(event.expected.arrowBlink)
        XCTAssertEqual(event.expected.alerts, [ReplayStimulusEvent.Alert(sample.alerts[0])])
        XCTAssertEqual(event.notifications.map(\.ordinal), Array(plan.emissions.indices))
        XCTAssertEqual(
            event.notifications.map(\.bytesHex),
            plan.emissions.map { $0.bytes.map { String(format: "%02X", $0) }.joined() }
        )

        XCTAssertTrue(event.machineEventLine.hasPrefix("V1REPLAY_EVENT {") )
        let payload = String(event.machineEventLine.dropFirst("V1REPLAY_EVENT ".count))
        let decoded = try XCTUnwrap(
            JSONSerialization.jsonObject(with: Data(payload.utf8)) as? [String: Any]
        )
        XCTAssertEqual(decoded["state"] as? String, "stimulus_requested")
        XCTAssertEqual(decoded["schemaVersion"] as? Int, 1)
        XCTAssertEqual(decoded["stimulusSequence"] as? Int, 9)
        XCTAssertEqual((decoded["notifications"] as? [[String: Any]])?.count, plan.emissions.count)
    }

    func testModeCharactersCoverEveryProtocolGlyph() {
        XCTAssertEqual(V1.ModeGlyph.allBogeys.displayCharacter, "A")
        XCTAssertEqual(V1.ModeGlyph.logic.displayCharacter, "l")
        XCTAssertEqual(V1.ModeGlyph.advancedLogic.displayCharacter, "L")
        XCTAssertEqual(V1.ModeGlyph.customSweeps.displayCharacter, "C")
        XCTAssertEqual(V1.ModeGlyph.euroKaOnly.displayCharacter, "u")
        XCTAssertEqual(V1.ModeGlyph.euroKaPhoto.displayCharacter, "U")
    }
}

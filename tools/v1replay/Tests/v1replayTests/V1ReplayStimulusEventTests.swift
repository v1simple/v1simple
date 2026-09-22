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
            blinkArrow: true,
            blinkBand: true
        )
        let event = ReplayStimulusEvent(
            sequence: 9,
            sample: sample,
            controlState: control,
            muted: true,
            displayOn: true,
            arrowBlink: true,
            bandBlink: true,
            plan: plan,
            intendedHostMonotonicNs: 123_499_000_000,
            requestedHostMonotonicNs: 123_500_000_000
        )

        XCTAssertEqual(event.state, "stimulus_requested")
        XCTAssertEqual(event.schemaVersion, 4)
        XCTAssertEqual(event.stimulusSequence, 9)
        XCTAssertEqual(event.sourceIndex, 41)
        XCTAssertEqual(event.intendedHostMonotonicNs, 123_499_000_000)
        XCTAssertEqual(event.requestedHostMonotonicNs, 123_500_000_000)
        XCTAssertEqual(event.requestedHostMonotonicSeconds, 123.5)
        XCTAssertEqual(event.expected.phase, "changed_scenario")
        XCTAssertEqual(event.expected.modeChar, "A")
        XCTAssertEqual(event.expected.mainVolume, 7)
        XCTAssertEqual(event.expected.muteVolume, 2)
        XCTAssertTrue(event.expected.muted)
        XCTAssertTrue(event.expected.arrowBlink)
        XCTAssertTrue(event.expected.bandBlink)
        XCTAssertEqual(event.expected.bogeyCounterChar, "1")
        XCTAssertEqual(event.expected.bogeyCounterOffChar, "1")
        XCTAssertFalse(event.expected.bogeyCounterBlink)
        XCTAssertEqual(event.expected.bogeyCounterImage1, 0x06)
        XCTAssertEqual(event.expected.bogeyCounterImage2, 0x06)
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
        XCTAssertEqual(decoded["schemaVersion"] as? Int, 4)
        XCTAssertEqual(decoded["intendedHostMonotonicNs"] as? Int, 123_499_000_000)
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

    func testStimulusEvidenceRetainsPhotoType() throws {
        let sample = TimedSample(
            offset: 1,
            phase: "photo",
            muted: false,
            alerts: [ReplayAlert(
                band: .k,
                frequencyMHz: 24_125,
                strength: 5,
                direction: .front,
                isPriority: true,
                photoType: 3
            )],
            sourceIndex: 3
        )
        let control = V1.Session.ControlState(
            mode: .advancedLogic, mainVolume: 4, mutedVolume: 0,
            savedMainVolume: 4, savedMutedVolume: 0)
        let plan = V1.PlaybackPacketPlan(
            sample: sample, controlState: control, displayOn: true, muted: false,
            blinkBogey: false, blinkArrow: false)
        let event = ReplayStimulusEvent(
            sequence: 1, sample: sample, controlState: control, muted: false,
            displayOn: true, arrowBlink: false, bandBlink: false, plan: plan,
            intendedHostMonotonicNs: 1, requestedHostMonotonicNs: 2)

        XCTAssertEqual(event.expected.alerts[0].photoType, 3)
        let row = try XCTUnwrap(plan.alertTablePackets.first)
        XCTAssertEqual(try IndependentFrameForStimulus.decode(row).payload[6] & 0x0F, 3)
    }

    func testStimulusEvidenceRetainsJunkBitAndExactBlinkPlanes() throws {
        let sample = TimedSample(
            offset: 2,
            phase: "junk_qualification_marked",
            muted: false,
            alerts: [ReplayAlert(
                band: .k,
                frequencyMHz: 24_199,
                strength: 2,
                direction: .front,
                isPriority: true,
                isJunk: true
            )],
            bogeyCounterOverride: .junkBlink,
            sourceIndex: 6
        )
        let control = V1.Session.ControlState(
            mode: .advancedLogic, mainVolume: 4, mutedVolume: 0,
            savedMainVolume: 4, savedMutedVolume: 0)
        let plan = V1.PlaybackPacketPlan(
            sample: sample, controlState: control, displayOn: true, muted: false,
            blinkBogey: false, blinkArrow: false)
        let event = ReplayStimulusEvent(
            sequence: 1, sample: sample, controlState: control, muted: false,
            displayOn: true, arrowBlink: false, bandBlink: false, plan: plan,
            intendedHostMonotonicNs: 1, requestedHostMonotonicNs: 2)

        XCTAssertTrue(event.expected.alerts[0].junk)
        XCTAssertEqual(event.expected.bogeyCounterChar, "J")
        XCTAssertEqual(event.expected.bogeyCounterOffChar, " ")
        XCTAssertTrue(event.expected.bogeyCounterBlink)
        XCTAssertEqual(event.expected.bogeyCounterImage1, 0x1E)
        XCTAssertEqual(event.expected.bogeyCounterImage2, 0x00)
        let row = try IndependentFrameForStimulus.decode(
            XCTUnwrap(plan.alertTablePackets.first))
        XCTAssertEqual(row.payload[6], 0xC0)
        let display = try IndependentFrameForStimulus.decode(plan.displayPacket)
        XCTAssertEqual(Array(display.payload.prefix(2)), [0x1E, 0x00])
    }
}

private struct IndependentFrameForStimulus {
    let payload: [UInt8]

    static func decode(_ bytes: [UInt8]) throws -> IndependentFrameForStimulus {
        guard bytes.count >= 7, bytes.first == 0xAA, bytes.last == 0xAB else {
            throw DecodeError.invalid
        }
        let payloadLength = Int(bytes[4]) - 1
        return IndependentFrameForStimulus(payload: Array(bytes[5..<(5 + payloadLength)]))
    }

    private enum DecodeError: Error { case invalid }
}

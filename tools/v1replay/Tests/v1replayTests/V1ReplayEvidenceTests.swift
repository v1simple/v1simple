import Foundation
import XCTest
@testable import v1replay

final class V1ReplayEvidenceTests: XCTestCase {
    func testNotificationEventsJoinRepeatedPayloadsByAssignedSequences() throws {
        let payload = Data("abc".utf8)
        let first = ReplayNotificationIdentity(
            globalTxSequence: 17,
            stimulusSequence: 4,
            emissionOrdinal: 1,
            characteristic: "B2CE",
            payload: payload,
            intendedHostMonotonicNs: 900,
            requestedHostMonotonicNs: 1_000
        )
        let repeated = ReplayNotificationIdentity(
            globalTxSequence: 18,
            stimulusSequence: 5,
            emissionOrdinal: 0,
            characteristic: "B2CE",
            payload: payload,
            requestedHostMonotonicNs: 2_000
        )

        XCTAssertEqual(first.payloadHex, "616263")
        XCTAssertEqual(
            first.payloadSha256,
            "ba7816bf8f01cfea414140de5dae2223"
                + "b00361a396177a9cb410ff61f20015ad"
        )
        XCTAssertEqual(first.payloadFnv1a32, "1A47E90B")
        XCTAssertEqual(fnv1a32Hex(Data()), "811C9DC5")
        XCTAssertEqual(first.payloadSha256, repeated.payloadSha256)
        XCTAssertNotEqual(first.globalTxSequence, repeated.globalTxSequence)

        let requested = first.requestedEvent
        let accepted = first.acceptedEvent(hostMonotonicNs: 1_250)
        let delayed = first.delayedEvent(hostMonotonicNs: 1_100)
        let dropped = first.droppedEvent(hostMonotonicNs: 1_200)
        let skipped = first.skippedEvent(hostMonotonicNs: 1_225)
        XCTAssertEqual(requested.state, "notification_requested")
        XCTAssertEqual(accepted.state, "notification_accepted")
        XCTAssertEqual(delayed.state, "notification_delayed")
        XCTAssertEqual(dropped.state, "notification_dropped")
        XCTAssertEqual(skipped.state, "notification_skipped")
        XCTAssertEqual(requested.globalTxSequence, accepted.globalTxSequence)
        XCTAssertEqual(requested.stimulusSequence, 4)
        XCTAssertEqual(requested.emissionOrdinal, 1)
        XCTAssertEqual(requested.intendedHostMonotonicNs, 900)
        XCTAssertEqual(requested.hostMonotonicNs, 1_000)
        XCTAssertEqual(accepted.hostMonotonicNs, 1_250)

        for event in [requested, accepted] {
            let line = event.machineEventLine
            XCTAssertTrue(line.hasPrefix("V1REPLAY_EVENT {"))
            let json = String(line.dropFirst("V1REPLAY_EVENT ".count))
            let decoded = try XCTUnwrap(
                JSONSerialization.jsonObject(with: Data(json.utf8)) as? [String: Any]
            )
            XCTAssertEqual(decoded["globalTxSequence"] as? Int, 17)
            XCTAssertEqual(decoded["characteristic"] as? String, "B2CE")
            XCTAssertEqual(decoded["payloadFnv1a32"] as? String, "1A47E90B")
            XCTAssertEqual(decoded["payloadHex"] as? String, "616263")
        }
    }

    func testResolvedScenarioEvidenceContainsUsedValuesButNoSourcePath() throws {
        let encounter = Encounter(
            origin: .externalInput,
            samples: [
                TimedSample(
                    offset: 0,
                    phase: "external",
                    muted: false,
                    alerts: [ReplayAlert(
                        band: .ka,
                        frequencyMHz: 34_700,
                        strength: 3,
                        direction: .front,
                        isPriority: true
                    )],
                    sourceIndex: 8
                ),
                TimedSample(
                    offset: 0.25,
                    phase: "external",
                    muted: true,
                    alerts: [ReplayAlert(
                        band: .k,
                        frequencyMHz: 24_150,
                        strength: 5,
                        direction: .rear,
                        isPriority: true
                    )],
                    sourceIndex: 9
                ),
            ]
        )
        let output = FileManager.default.temporaryDirectory
            .appendingPathComponent(UUID().uuidString)
            .appendingPathExtension("json")
        defer { try? FileManager.default.removeItem(at: output) }

        let summary = try encounter.writeResolvedScenarioEvidence(path: output.path)
        let data = try Data(contentsOf: output)
        let decoded = try XCTUnwrap(
            JSONSerialization.jsonObject(with: data) as? [String: Any]
        )
        let samples = try XCTUnwrap(decoded["samples"] as? [[String: Any]])

        XCTAssertEqual(summary.origin, "external")
        XCTAssertEqual(summary.sampleCount, 2)
        XCTAssertEqual(summary.byteCount, data.count)
        XCTAssertEqual(summary.sha256, sha256Hex(data))
        XCTAssertEqual(decoded["origin"] as? String, "external")
        XCTAssertEqual(samples[0]["sourceIndex"] as? Int, 8)
        XCTAssertEqual(samples[1]["offsetSeconds"] as? Double, 0.25)
        XCTAssertNil(decoded["sourcePath"])
        XCTAssertFalse(String(decoding: data, as: UTF8.self).contains(output.path))
        XCTAssertEqual(encounter.uniformCadenceHz, 4.0)
    }

    func testIrregularScenarioHasNoInventedFixedCadence() {
        let samples = [0.0, 0.2, 0.7].enumerated().map { index, offset in
            TimedSample(
                offset: offset,
                phase: "external",
                muted: false,
                alerts: [],
                sourceIndex: index
            )
        }
        XCTAssertNil(Encounter(origin: .externalInput, samples: samples).uniformCadenceHz)
    }

    func testManagedScenarioArgumentsRemainExplicit() {
        let parsed = Arguments([
            "bench",
            "--scenario", "external.json",
            "--scenario-evidence", "resolved.json",
            "--machine-events",
        ])
        XCTAssertEqual(parsed.command, "bench")
        XCTAssertEqual(parsed.optionalString("scenario"), "external.json")
        XCTAssertEqual(parsed.optionalString("scenario-evidence"), "resolved.json")
        XCTAssertTrue(parsed.bool("machine-events"))
        XCTAssertTrue(parsed.positional.isEmpty)
        XCTAssertEqual(
            ArrowBlinkProfile.scenario.sourceLabel(for: .externalInput),
            "resolved_scenario"
        )
        XCTAssertEqual(
            ArrowBlinkProfile.scenario.sourceLabel(for: .syntheticBench),
            "generated_multi_alert_assumption"
        )
    }
}

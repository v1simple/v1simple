import Foundation
import XCTest
@testable import v1replay

final class V1ProfileControlsScenarioTests: XCTestCase {
    func testBoxCasesKeepEdgesMixedRowsLaserAndOneIsolatedMuteSeed() throws {
        let encounter = ProfileControlsScenario.make()
        XCTAssertEqual(encounter.samples.count, 267)
        XCTAssertEqual(encounter.duration, 88.0 + 2.0 / 3.0, accuracy: 0.000_001)
        for (index, sample) in encounter.samples.enumerated() {
            XCTAssertEqual(sample.sourceIndex, index)
            XCTAssertEqual(sample.offset, Double(index) / 3, accuracy: 0.000_001)
            XCTAssertFalse(sample.scenarioArrowBlink)
        }
        XCTAssertTrue(encounter.samples.prefix(36).allSatisfy { $0.alerts.isEmpty })
        XCTAssertTrue(encounter.samples.suffix(12).allSatisfy { $0.alerts.isEmpty })

        let expectedK: [(String, [UInt16])] = [
            ("box_k_lower_outside", [24_049]),
            ("box_k_lower_edge", [24_050]),
            ("box_k_upper_edge", [24_150]),
            ("box_k_upper_outside", [24_151]),
            ("box_mixed_outside_inside", [24_151, 24_100]),
        ]
        for (phase, frequencies) in expectedK {
            let samples = encounter.samples.filter { $0.phase == phase }
            XCTAssertEqual(samples.count, 9, phase)
            XCTAssertTrue(samples.allSatisfy { $0.alerts.map(\.frequencyMHz) == frequencies }, phase)
            XCTAssertTrue(samples.allSatisfy { $0.alerts.allSatisfy { $0.band.mask == V1.Band.k.mask } })
        }
        let unselected = try first("box_mixed_unselected_ka", encounter)
        XCTAssertEqual(unselected.alerts.map(\.band.mask), [V1.Band.k.mask, V1.Band.ka.mask])
        XCTAssertEqual(unselected.alerts.map(\.frequencyMHz), [24_049, 33_950])
        let laser = try first("box_mixed_laser", encounter)
        XCTAssertEqual(laser.alerts.map(\.band.mask), [V1.Band.laser.mask, V1.Band.k.mask])
        XCTAssertEqual(laser.alerts.map(\.frequencyMHz), [0, 24_049])
        XCTAssertTrue(try first("box_clear_release", encounter).alerts.isEmpty)

        XCTAssertEqual(encounter.detectorMuteCheckpoints, [
            DetectorMuteCheckpoint(replaySecond: 79, muted: true),
        ])
        XCTAssertTrue(encounter.samples.filter { $0.offset < 79 }.allSatisfy { !$0.muted })
        XCTAssertTrue(encounter.samples.filter { $0.offset >= 79 }.allSatisfy(\.muted))
        let seed = try first("box_preexisting_detector_mute_seed", encounter)
        XCTAssertEqual(seed.alerts.first?.band.mask, V1.Band.ka.mask)
        let inside = try first("box_new_inside_releases_preexisting_mute", encounter)
        XCTAssertEqual(inside.offset, 82)
        XCTAssertEqual(inside.alerts.first?.frequencyMHz, 24_100)
    }

    func testWrittenScanProfileProjectsOnlyInclusiveAdmittedRowsAndReadsBackExactly() throws {
        var session = V1.Session()
        let userBytes: [UInt8] = [0x7F, 0xF7, 0xFF, 0xFF, 0xFF, 0xFF]
        XCTAssertEqual(session.receive(request(.reqWriteUserBytes, userBytes))[0].effects,
                       [.userBytesStored(userBytes)])
        let k = V1.Session.SweepDefinition(index: 0, lowerMHz: 24_000, upperMHz: 24_200)
        let ka = V1.Session.SweepDefinition(index: 1, lowerMHz: 33_900, upperMHz: 35_000)
        XCTAssertEqual(session.receive(request(.reqWriteSweepDefinition, sweep(k, commit: false)))[0].effects, [])
        let committed = session.receive(request(.reqWriteSweepDefinition, sweep(ka, commit: true)))
        let result = try XCTUnwrap(try replies(committed).first)
        XCTAssertEqual(result.packetID, 0x21)
        XCTAssertEqual(result.payload, [0])
        XCTAssertEqual(session.storedSweepDefinitions, [k, ka])
        let readback = try replies(session.receive(request(.reqAllSweepDefinitions)))
            .filter { $0.packetID == 0x17 }
        XCTAssertEqual(readback.map(\.payload), [
            [0x80, 0x5E, 0x88, 0x5D, 0xC0], // index0 upper24200 lower24000
            [0x81, 0x88, 0xB8, 0x84, 0x6C], // index1 upper35000 lower33900
        ])

        let encounter = ProfileControlsScenario.make()
        for phase in ["scan_k_lower_admitted", "scan_k_upper_admitted",
                      "scan_ka_lower_admitted", "scan_ka_upper_admitted"] {
            let authored = try first(phase, encounter)
            let projected = session.projectedSample(authored)
            XCTAssertEqual(projected.alerts.map(\.frequencyMHz), authored.alerts.map(\.frequencyMHz), phase)
        }
        for phase in ["scan_k_below_excluded", "scan_k_above_excluded",
                      "scan_ka_below_excluded", "scan_ka_above_excluded"] {
            let authored = try first(phase, encounter)
            XCTAssertEqual(authored.alerts.count, 1, phase)
            let projected = session.projectedSample(authored)
            XCTAssertTrue(projected.alerts.isEmpty, phase)
            let plan = V1.PlaybackPacketPlan(
                sample: projected, controlState: session.controlState,
                displayOn: true, muted: false, blinkBogey: false,
                blinkArrow: false, blinkBand: false)
            let clear = try ProfileControlsFrame.decode(XCTUnwrap(plan.alertTablePackets.first))
            XCTAssertEqual(clear.packetID, 0x43)
            XCTAssertEqual(clear.payload[0], 0, phase)
        }
        // The scan filter cannot accidentally remove a row that is meant to
        // challenge the DUT's all-current-alerts box policy.
        for sample in encounter.samples where !sample.phase.hasPrefix("scan_") {
            XCTAssertEqual(session.projectedSample(sample).alerts.count, sample.alerts.count, sample.phase)
        }
    }

    func testDutMuteCommandEvidenceIsDistinctFromAuthoredMuteAndPreservesTimestamp() throws {
        for muted in [false, true] {
            let event = ReplayMuteCommandEvent(muted: muted, hostMonotonicNs: 123_456_789_012_345)
            let prefix = "V1REPLAY_EVENT "
            XCTAssertTrue(event.machineEventLine.hasPrefix(prefix))
            let json = Data(event.machineEventLine.dropFirst(prefix.count).utf8)
            let decoded = try XCTUnwrap(JSONSerialization.jsonObject(with: json) as? [String: Any])
            XCTAssertEqual(decoded["state"] as? String, "dut_mute_command")
            XCTAssertEqual(decoded["schemaVersion"] as? Int, 1)
            XCTAssertEqual(decoded["muted"] as? Bool, muted)
            XCTAssertEqual((decoded["hostMonotonicNs"] as? NSNumber)?.uint64Value, 123_456_789_012_345)
            XCTAssertNil(decoded["replaySecond"], "Receipt context must not claim a causal input")
        }
    }

    private func first(_ phase: String, _ encounter: Encounter) throws -> TimedSample {
        try XCTUnwrap(encounter.samples.first { $0.phase == phase })
    }

    private func request(_ id: V1.PacketID, _ payload: [UInt8] = []) -> [UInt8] {
        var bytes: [UInt8] = [0xAA, 0xDA, 0xE6, id.rawValue, UInt8(payload.count + 1)] + payload
        bytes.append(bytes.reduce(0, &+))
        bytes.append(0xAB)
        return bytes
    }

    private func sweep(_ definition: V1.Session.SweepDefinition, commit: Bool) -> [UInt8] {
        [0x80 | definition.index | (commit ? 0x40 : 0),
         UInt8(definition.upperMHz >> 8), UInt8(definition.upperMHz & 0xFF),
         UInt8(definition.lowerMHz >> 8), UInt8(definition.lowerMHz & 0xFF)]
    }

    private func replies(_ outcomes: [V1.Session.CommandOutcome]) throws -> [ProfileControlsFrame] {
        try outcomes.flatMap(\.effects).compactMap { effect in
            guard case .reply(let reply) = effect else { return nil }
            return try ProfileControlsFrame.decode(reply.bytes)
        }
    }
}

private struct ProfileControlsFrame {
    let packetID: UInt8
    let payload: [UInt8]

    static func decode(_ bytes: [UInt8]) throws -> ProfileControlsFrame {
        guard bytes.count >= 7, bytes.first == 0xAA, bytes.last == 0xAB,
              bytes.count == Int(bytes[4]) + 6,
              bytes[bytes.count - 2] == bytes.dropLast(2).reduce(UInt8(0), &+) else {
            throw NSError(domain: "ProfileControlsFrame", code: 1)
        }
        return ProfileControlsFrame(packetID: bytes[3], payload: Array(bytes[5..<(bytes.count - 2)]))
    }
}

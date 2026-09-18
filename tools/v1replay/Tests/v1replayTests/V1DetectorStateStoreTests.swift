import Foundation
import XCTest
@testable import v1replay

final class V1DetectorStateStoreTests: XCTestCase {
    func testWebProfileWritesSurviveAProcessStateRoundTrip() throws {
        var originalConfig = V1.Session.Config()
        originalConfig.version = "4.1039"
        var original = V1.Session(config: originalConfig)

        let desiredUserBytes: [UInt8] = [0x7F, 0xF7, 0xFB, 0x7F, 0xFC, 0xFF]
        XCTAssertEqual(
            original.receive(request(.reqWriteUserBytes, payload: desiredUserBytes))[0].effects,
            [.userBytesStored(desiredUserBytes)]
        )

        let k = V1.Session.SweepDefinition(
            index: 0, lowerMHz: 24_100, upperMHz: 24_200)
        let ka = V1.Session.SweepDefinition(
            index: 1, lowerMHz: 34_600, upperMHz: 34_800)
        XCTAssertEqual(
            original.receive(request(
                .reqWriteSweepDefinition,
                payload: sweepWritePayload(k, commit: false)
            ))[0].effects,
            []
        )
        let commit = original.receive(request(
            .reqWriteSweepDefinition,
            payload: sweepWritePayload(ka, commit: true)
        ))
        guard case .reply(let sweepResult) = commit[0].effects.first else {
            return XCTFail("custom sweep commit did not reply")
        }
        XCTAssertEqual(sweepResult.bytes[5], 0)

        _ = original.receive(request(.changeMode, payload: [0x02]))
        _ = original.receive(request(.reqWriteVolume, payload: [7, 2, 0x04]))

        let temporary = FileManager.default.temporaryDirectory
            .appendingPathComponent(UUID().uuidString, isDirectory: true)
        defer { try? FileManager.default.removeItem(at: temporary) }
        let path = temporary.appendingPathComponent("detector-state.json").path
        let store = V1DetectorStateStore(path: path)
        store.save(original.persistentState)
        try store.flush()

        let decoded = try XCTUnwrap(store.load(expectedVersion: "4.1039"))
        var restoredConfig = V1.Session.Config()
        restoredConfig.version = "4.1039"
        try V1.Session.applyPersistentState(decoded, to: &restoredConfig)
        let restored = V1.Session(config: restoredConfig)

        XCTAssertEqual(restored.storedUserBytes, desiredUserBytes)
        XCTAssertEqual(restored.storedSweepDefinitions, [k, ka])
        XCTAssertEqual(restored.controlState, V1.Session.ControlState(
            mode: .logic,
            mainVolume: 7,
            mutedVolume: 2,
            savedMainVolume: 7,
            savedMutedVolume: 2
        ))
    }

    func testStateFromAnotherFirmwareVersionFailsClosed() throws {
        let temporary = FileManager.default.temporaryDirectory
            .appendingPathComponent(UUID().uuidString, isDirectory: true)
        defer { try? FileManager.default.removeItem(at: temporary) }
        let store = V1DetectorStateStore(
            path: temporary.appendingPathComponent("detector-state.json").path
        )

        var config = V1.Session.Config()
        config.version = "4.1039"
        store.save(V1.Session(config: config).persistentState)
        try store.flush()

        XCTAssertThrowsError(try store.load(expectedVersion: "4.1038"))
    }

    private func request(
        _ id: V1.PacketID,
        payload: [UInt8] = []
    ) -> [UInt8] {
        var packet: [UInt8] = [
            0xAA, 0xDA, 0xE6, id.rawValue, UInt8(payload.count + 1),
        ]
        packet.append(contentsOf: payload)
        packet.append(packet.reduce(UInt8(0), &+))
        packet.append(0xAB)
        return packet
    }

    private func sweepWritePayload(
        _ definition: V1.Session.SweepDefinition,
        commit: Bool
    ) -> [UInt8] {
        return [
            definition.index | (commit ? 0x40 : 0),
            UInt8((definition.upperMHz >> 8) & 0xFF),
            UInt8(definition.upperMHz & 0xFF),
            UInt8((definition.lowerMHz >> 8) & 0xFF),
            UInt8(definition.lowerMHz & 0xFF),
        ]
    }
}

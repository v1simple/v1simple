import Foundation
import XCTest
@testable import v1replay

final class V1HandshakeLedgerTests: XCTestCase {
    func testLedgerIsBoundedAnonymousAndRecordsOnlyHandshakeEvidence() throws {
        let url = FileManager.default.temporaryDirectory
            .appendingPathComponent("v1replay-handshake-\(UUID().uuidString).jsonl")
        defer { try? FileManager.default.removeItem(at: url) }

        let ledger = try HandshakeLedger(path: url.path)
        XCTAssertEqual(ledger.beginEpoch(), 1)
        ledger.recordAcceptedRequest(
            bytes: [0xAA, 0xDA, 0xE6, 0x41, 0x01, 0xAC, 0xAB],
            channel: "B6D4",
            belongsToEpochSubscriber: true
        )
        ledger.recordAcceptedRequest(
            bytes: [0xAA, 0xDA, 0xE6, 0x01, 0x01, 0x6C, 0xAB],
            channel: "B6D4",
            belongsToEpochSubscriber: true
        )
        ledger.recordDelivered(
            bytes: [0xAA, 0xD6, 0xEA, 0x02, 0x08, 0x76, 0x34, 0x2E,
                    0x31, 0x30, 0x33, 0x38, 0x18, 0xAB],
            channel: "B2CE",
            epoch: 1
        )
        ledger.recordAcceptedRequest(
            bytes: [0xAA, 0xDA, 0xE6, 0x3C, 0x01, 0xA7, 0xAB],
            channel: "B6D4",
            belongsToEpochSubscriber: true
        )
        ledger.recordDelivered(
            bytes: [0xAA, 0xD6, 0xEA, 0x3D, 0x05, 0x04, 0x00, 0x04, 0x00, 0xB4, 0xAB],
            channel: "B2CE",
            epoch: 1
        )
        let firstRow: [UInt8] = [
            0xAA, 0xD8, 0xEA, 0x43, 0x08, 0x12, 0x5E, 0x56,
            0xA9, 0x00, 0x24, 0x00, 0x4A, 0xAB,
        ]
        ledger.recordDelivered(bytes: firstRow, channel: "B2CE", epoch: 1)
        ledger.recordDelivered(bytes: firstRow, channel: "B2CE", epoch: 1)
        // Display traffic and packets outside the startup contract are omitted.
        ledger.recordDelivered(
            bytes: [0xAA, 0xD8, 0xEA, 0x31, 0x01, 0xA4, 0xAB],
            channel: "B2CE",
            epoch: 1
        )

        let lines = try String(contentsOf: url, encoding: .utf8)
            .split(separator: "\n")
            .map(String.init)
        XCTAssertEqual(lines.count, 8)
        let objects = try lines.map { line -> [String: Any] in
            let data = try XCTUnwrap(line.data(using: .utf8))
            return try XCTUnwrap(
                JSONSerialization.jsonObject(with: data) as? [String: Any]
            )
        }
        XCTAssertEqual(objects[0]["kind"] as? String, "v1replay_handshake_ledger")
        XCTAssertEqual(objects[1]["event"] as? String, "subscribe")
        XCTAssertEqual(objects[2]["event"] as? String, "request")
        XCTAssertEqual(objects[3]["event"] as? String, "request")
        XCTAssertEqual(objects[4]["event"] as? String, "response")
        XCTAssertEqual(objects[5]["event"] as? String, "request")
        XCTAssertEqual(objects[6]["event"] as? String, "response")
        XCTAssertEqual(objects[7]["event"] as? String, "stream_started")

        let serialized = lines.joined(separator: "\n")
        XCTAssertFalse(serialized.contains("UUID"))
        XCTAssertFalse(serialized.contains("timestamp"))
        XCTAssertFalse(serialized.contains(url.path))
    }

    func testLedgerRejectsStaleDeliveryAndCapsAnonymousEpochs() throws {
        let url = FileManager.default.temporaryDirectory
            .appendingPathComponent("v1replay-handshake-cap-\(UUID().uuidString).jsonl")
        defer { try? FileManager.default.removeItem(at: url) }

        let ledger = try HandshakeLedger(path: url.path)
        XCTAssertEqual(ledger.beginEpoch(), 1)
        ledger.endEpoch()
        XCTAssertEqual(ledger.beginEpoch(), 2)
        ledger.recordDelivered(
            bytes: [0xAA, 0xD6, 0xEA, 0x3D, 0x05, 0x04, 0x00, 0x04, 0x00, 0xB4, 0xAB],
            channel: "B2CE",
            epoch: 1
        )
        for expected in 3...HandshakeLedger.maximumEpochs {
            XCTAssertEqual(ledger.beginEpoch(), expected)
        }
        XCTAssertNil(ledger.beginEpoch())

        let lines = try String(contentsOf: url, encoding: .utf8)
            .split(separator: "\n")
        XCTAssertEqual(lines.count, 1 + HandshakeLedger.maximumEpochs)
        XCTAssertFalse(lines.contains { $0.contains("response") })
    }

    func testLedgerRefusesToOverwriteExistingEvidence() throws {
        let url = FileManager.default.temporaryDirectory
            .appendingPathComponent("v1replay-handshake-existing-\(UUID().uuidString).jsonl")
        let original = Data("keep-existing-evidence\n".utf8)
        try original.write(to: url)
        defer { try? FileManager.default.removeItem(at: url) }

        XCTAssertThrowsError(try HandshakeLedger(path: url.path))
        XCTAssertEqual(try Data(contentsOf: url), original)
    }

    func testForeignAcceptedWriteEndsEpochBeforeRecordingOrDelivery() throws {
        let url = FileManager.default.temporaryDirectory
            .appendingPathComponent("v1replay-handshake-owner-\(UUID().uuidString).jsonl")
        defer { try? FileManager.default.removeItem(at: url) }

        let ledger = try HandshakeLedger(path: url.path)
        XCTAssertEqual(ledger.beginEpoch(), 1)

        // A valid accepted command outside the three recorded handshake IDs
        // must still invalidate attribution when a different central wrote it.
        ledger.recordAcceptedRequest(
            bytes: [0xAA, 0xDA, 0xE6, 0x34, 0x01, 0x9F, 0xAB],
            channel: "B6D4",
            belongsToEpochSubscriber: false
        )
        XCTAssertNil(ledger.activeEpoch)

        ledger.recordAcceptedRequest(
            bytes: [0xAA, 0xDA, 0xE6, 0x01, 0x01, 0x6C, 0xAB],
            channel: "B6D4",
            belongsToEpochSubscriber: true
        )
        ledger.recordDelivered(
            bytes: [0xAA, 0xD6, 0xEA, 0x02, 0x08, 0x76, 0x34, 0x2E,
                    0x31, 0x30, 0x33, 0x38, 0x18, 0xAB],
            channel: "B2CE",
            epoch: 1
        )

        let lines = try String(contentsOf: url, encoding: .utf8)
            .split(separator: "\n")
        XCTAssertEqual(lines.count, 2)
        XCTAssertFalse(lines.contains { $0.contains("\"event\":\"request\"") })
        XCTAssertFalse(lines.contains { $0.contains("response") })
    }

    func testHandshakeEvidenceOwnerMatchesOnlyTheSoleShortSubscriber() {
        let owner = UUID()
        XCTAssertTrue(
            V1Peripheral.handshakeEvidenceOwnerMatches(subscriber: owner, writer: owner)
        )
        XCTAssertFalse(
            V1Peripheral.handshakeEvidenceOwnerMatches(subscriber: owner, writer: UUID())
        )
        XCTAssertFalse(
            V1Peripheral.handshakeEvidenceOwnerMatches(subscriber: nil, writer: owner)
        )
    }

    func testPeripheralChecksWriterIdentityBeforeStreamReassembly() throws {
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let source = try String(
            contentsOf: packageRoot
                .appendingPathComponent("Sources/v1replay/Peripheral.swift"),
            encoding: .utf8
        )
        let ownerCheck = try XCTUnwrap(
            source.range(of: "let belongsToHandshakeSubscriber = Self.handshakeEvidenceOwnerMatches(")
        )
        let writerIdentity = try XCTUnwrap(
            source.range(of: "writer: request.central.identifier", range: ownerCheck.lowerBound..<source.endIndex)
        )
        let append = try XCTUnwrap(
            source.range(of: "withState { $0.session.append", range: ownerCheck.lowerBound..<source.endIndex)
        )
        XCTAssertLessThan(writerIdentity.lowerBound, append.lowerBound)
        XCTAssertTrue(
            source.contains(
                "handshakeSubscriberID = central.identifier\n            handshakeLedger?.beginEpoch()"
            )
        )
    }

}

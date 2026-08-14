import Foundation
import XCTest
@testable import v1replay

final class V1HandshakeLedgerTests: XCTestCase {
    func testLedgerIsBoundedAnonymousAndRecordsOnlyHandshakeEvidence() throws {
        let url = FileManager.default.temporaryDirectory
            .appendingPathComponent("v1replay-handshake-\(UUID().uuidString).jsonl")
        defer { try? FileManager.default.removeItem(at: url) }

        var now: Int64 = 50_000
        let ledger = try HandshakeLedger(path: url.path, nowMilliseconds: { now })
        XCTAssertEqual(ledger.beginEpoch(), 1)
        now = 50_007
        ledger.recordAcceptedRequest(
            bytes: [0xAA, 0xDA, 0xE6, 0x41, 0x01, 0xAC, 0xAB],
            channel: "B6D4",
            belongsToEpochSubscriber: true
        )
        now = 50_012
        ledger.recordAcceptedRequest(
            bytes: [0xAA, 0xDA, 0xE6, 0x01, 0x01, 0x6C, 0xAB],
            channel: "B6D4",
            belongsToEpochSubscriber: true
        )
        now = 50_020
        ledger.recordDelivered(
            bytes: [0xAA, 0xD6, 0xEA, 0x02, 0x08, 0x76, 0x34, 0x2E,
                    0x31, 0x30, 0x33, 0x38, 0x18, 0xAB],
            channel: "B2CE",
            epoch: 1
        )
        now = 50_021
        ledger.recordAcceptedRequest(
            bytes: [0xAA, 0xDA, 0xE6, 0x3C, 0x01, 0xA7, 0xAB],
            channel: "B6D4",
            belongsToEpochSubscriber: true
        )
        now = 50_034
        ledger.recordDelivered(
            bytes: [0xAA, 0xD6, 0xEA, 0x3D, 0x05, 0x04, 0x00, 0x04, 0x00, 0xB4, 0xAB],
            channel: "B2CE",
            epoch: 1
        )
        let firstRow: [UInt8] = [
            0xAA, 0xD8, 0xEA, 0x43, 0x08, 0x12, 0x5E, 0x56,
            0xA9, 0x00, 0x24, 0x00, 0x4A, 0xAB,
        ]
        now = 50_055
        ledger.recordDelivered(bytes: firstRow, channel: "B2CE", epoch: 1)
        now = 50_089
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
        XCTAssertEqual(
            Set(objects[0].keys),
            Set(["kind", "schema_version", "timebase"])
        )
        XCTAssertEqual(objects[0]["kind"] as? String, "v1replay_handshake_ledger")
        XCTAssertEqual((objects[0]["schema_version"] as? NSNumber)?.intValue, 2)
        XCTAssertEqual(objects[0]["timebase"] as? String, "epoch_monotonic_ms")
        XCTAssertEqual(objects[1]["event"] as? String, "subscribe")
        XCTAssertEqual(objects[2]["event"] as? String, "request")
        XCTAssertEqual(objects[3]["event"] as? String, "request")
        XCTAssertEqual(objects[4]["event"] as? String, "response")
        XCTAssertEqual(objects[5]["event"] as? String, "request")
        XCTAssertEqual(objects[6]["event"] as? String, "response")
        XCTAssertEqual(objects[7]["event"] as? String, "stream_started")
        XCTAssertEqual(
            objects.dropFirst().compactMap {
                ($0["elapsed_ms"] as? NSNumber)?.int64Value
            },
            [0, 7, 12, 20, 21, 34, 55]
        )
        XCTAssertTrue(objects.dropFirst().allSatisfy { object in
            object["elapsed_ms"] is NSNumber
        })

        let serialized = lines.joined(separator: "\n")
        XCTAssertFalse(serialized.contains("UUID"))
        XCTAssertFalse(serialized.contains("timestamp"))
        XCTAssertFalse(serialized.contains("50000"))
        XCTAssertFalse(serialized.contains(url.path))
    }

    func testEpochRelativeClockResetsAndNeverEmitsAbsoluteTime() throws {
        let url = FileManager.default.temporaryDirectory
            .appendingPathComponent("v1replay-handshake-reset-\(UUID().uuidString).jsonl")
        defer { try? FileManager.default.removeItem(at: url) }

        var now: Int64 = 9_000_000
        let ledger = try HandshakeLedger(path: url.path, nowMilliseconds: { now })
        XCTAssertEqual(ledger.beginEpoch(), 1)
        now = 9_000_125
        ledger.recordAcceptedRequest(
            bytes: [0xAA, 0xDA, 0xE6, 0x41, 0x01, 0xAC, 0xAB],
            channel: "B6D4",
            belongsToEpochSubscriber: true
        )
        ledger.endEpoch()

        now = 12_000_000
        XCTAssertEqual(ledger.beginEpoch(), 2)
        now = 12_000_009
        ledger.recordAcceptedRequest(
            bytes: [0xAA, 0xDA, 0xE6, 0x01, 0x01, 0x6C, 0xAB],
            channel: "B6D4",
            belongsToEpochSubscriber: true
        )

        let objects = try decodeLedger(url)
        XCTAssertEqual(
            objects.dropFirst().compactMap {
                ($0["elapsed_ms"] as? NSNumber)?.int64Value
            },
            [0, 125, 0, 9]
        )
        XCTAssertEqual(
            objects.dropFirst().compactMap { ($0["epoch"] as? NSNumber)?.intValue },
            [1, 1, 2, 2]
        )
        let serialized = try String(contentsOf: url, encoding: .utf8)
        XCTAssertFalse(serialized.contains("9000000"))
        XCTAssertFalse(serialized.contains("12000000"))
    }

    func testEventCapStillExposesSixthStartIncludingPostStreamRetries() throws {
        let url = FileManager.default.temporaryDirectory
            .appendingPathComponent("v1replay-handshake-event-cap-\(UUID().uuidString).jsonl")
        defer { try? FileManager.default.removeItem(at: url) }

        var now: Int64 = 1_000
        let ledger = try HandshakeLedger(path: url.path, nowMilliseconds: { now })
        XCTAssertEqual(ledger.beginEpoch(), 1)

        let start: [UInt8] = [0xAA, 0xDA, 0xE6, 0x41, 0x01, 0xAC, 0xAB]
        now = 1_001
        ledger.recordAcceptedRequest(
            bytes: start, channel: "B6D4", belongsToEpochSubscriber: true
        )
        now = 1_002
        ledger.recordAcceptedRequest(
            bytes: [0xAA, 0xDA, 0xE6, 0x01, 0x01, 0x6C, 0xAB],
            channel: "B6D4",
            belongsToEpochSubscriber: true
        )
        now = 1_003
        ledger.recordDelivered(
            bytes: [0xAA, 0xD6, 0xEA, 0x02, 0x08, 0x76, 0x34, 0x2E,
                    0x31, 0x30, 0x33, 0x38, 0x18, 0xAB],
            channel: "B2CE",
            epoch: 1
        )
        now = 1_004
        ledger.recordAcceptedRequest(
            bytes: [0xAA, 0xDA, 0xE6, 0x3C, 0x01, 0xA7, 0xAB],
            channel: "B6D4",
            belongsToEpochSubscriber: true
        )
        now = 1_005
        ledger.recordDelivered(
            bytes: [0xAA, 0xD6, 0xEA, 0x3D, 0x05, 0x04, 0x00, 0x04, 0x00, 0xB4, 0xAB],
            channel: "B2CE",
            epoch: 1
        )
        now = 1_006
        ledger.recordDelivered(
            bytes: [0xAA, 0xD8, 0xEA, 0x43, 0x08,
                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xB7, 0xAB],
            channel: "B2CE",
            epoch: 1
        )

        // The cap has room for five retries after the canonical seven events,
        // so the sixth observed start remains visible and the seventh is omitted.
        for elapsed in 7...12 {
            now = 1_000 + Int64(elapsed)
            ledger.recordAcceptedRequest(
                bytes: start, channel: "B6D4", belongsToEpochSubscriber: true
            )
        }

        let events = Array(try decodeLedger(url).dropFirst())
        XCTAssertEqual(events.count, HandshakeLedger.maximumEventsPerEpoch)
        let startIndices = events.indices.filter { index in
            let event = events[index]
            guard let bytes = event["bytes"] as? [NSNumber], bytes.count > 3 else {
                return false
            }
            return bytes[3].intValue == 0x41
        }
        let starts = startIndices.map { events[$0] }
        XCTAssertEqual(starts.count, 6)
        XCTAssertEqual(
            starts.compactMap { ($0["elapsed_ms"] as? NSNumber)?.int64Value },
            [1, 7, 8, 9, 10, 11]
        )
        let streamIndex = try XCTUnwrap(events.firstIndex { event in
            event["event"] as? String == "stream_started"
        })
        XCTAssertTrue(startIndices.dropFirst().allSatisfy { $0 > streamIndex })
        XCTAssertFalse(events.contains { event in
            (event["elapsed_ms"] as? NSNumber)?.int64Value == 12
        })
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

private func decodeLedger(_ url: URL) throws -> [[String: Any]] {
    try String(contentsOf: url, encoding: .utf8)
        .split(separator: "\n")
        .map { line in
            let data = try XCTUnwrap(String(line).data(using: .utf8))
            return try XCTUnwrap(
                JSONSerialization.jsonObject(with: data) as? [String: Any]
            )
        }
}

import CoreBluetooth
import XCTest
@testable import v1replay

final class V1ProxyCaptureTests: XCTestCase {
    private let short = CBUUID(string: V1.displayShortUUID)
    private let long = CBUUID(string: V1.displayLongUUID)
    private let volume: [UInt8] = [0xAA, 0xD6, 0xEA, 0x3D, 0x05, 0x04, 0x00, 0x04, 0x00, 0xB4, 0xAB]
    // Three sweep sections, with an independently summed checksum (0x6D).
    private let sections: [UInt8] = [
        0xAA, 0xD6, 0xEA, 0x23, 0x10,
        0x13, 0x8C, 0xE8, 0x89, 0x23,
        0x23, 0x89, 0x1F, 0x87, 0xD6,
        0x33, 0x87, 0xD2, 0x82, 0x67,
        0x6D, 0xAB
    ]

    private func capture(_ record: (PacketLog) -> Void) throws -> [[String: Any]] {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: directory) }
        let path = directory.appendingPathComponent("capture.jsonl").path
        let log = try PacketLog(path: path, tag: nil)
        record(log)
        log.close()
        return try String(contentsOfFile: path).split(separator: "\n").map {
            try XCTUnwrap(JSONSerialization.jsonObject(with: Data($0.utf8)) as? [String: Any])
        }
    }

    private func frames(_ rows: [[String: Any]]) -> [[String: Any]] {
        rows.filter { $0["kind"] as? String == "frame" }
    }

    func testShortCapturePreservesChecksumFreePayloadAndChecksummedControl() throws {
        let checksumFree: [UInt8] = [0xAA, 0xD6, 0xE9, 0x3D, 0x04, 0x04, 0x00, 0x04, 0x00, 0xAB]
        let rows = try capture { log in
            log.record(direction: "v1->dut", characteristic: short, bytes: checksumFree)
            log.record(direction: "v1->dut", characteristic: short, bytes: volume)
        }
        XCTAssertEqual(frames(rows).compactMap { $0["payload"] as? String }, ["04 00 04 00", "04 00 04 00"])
        XCTAssertEqual(frames(rows).compactMap { $0["hex"] as? String }, [checksumFree.hexString, volume.hexString])
    }

    func testIndexedLongCaptureReassemblesBothArrivalOrdersAndPreservesRawChunks() throws {
        let first = [UInt8(0x12)] + sections.prefix(19)
        let last = [UInt8(0x22)] + sections.dropFirst(19)
        for chunks in [[first, last], [last, first]] {
            let rows = try capture { log in
                for bytes in chunks { log.record(direction: "v1->dut", characteristic: long, bytes: bytes) }
            }
            XCTAssertEqual(frames(rows).compactMap { $0["hex"] as? String }, [sections.hexString])
            XCTAssertEqual(frames(rows).compactMap { $0["payload"] as? String }, [
                "13 8C E8 89 23 23 89 1F 87 D6 33 87 D2 82 67"
            ])
            let raw = rows.filter { $0["kind"] as? String == "chunk" }
            XCTAssertEqual(raw.compactMap { $0["hex"] as? String }, chunks.map(\.hexString))
            XCTAssertEqual(raw.compactMap { $0["len"] as? Int }, chunks.map(\.count))
            XCTAssertEqual(rows.compactMap { $0["char"] as? String }, ["B4E0", "B4E0", "B4E0"])
        }
    }

    func testInterleavedShortTrafficDoesNotCorruptLongAssembly() throws {
        let rows = try capture { log in
            log.record(direction: "v1->dut", characteristic: long, bytes: [0x12] + sections.prefix(19))
            log.record(direction: "v1->dut", characteristic: short, bytes: volume)
            log.record(direction: "v1->dut", characteristic: long, bytes: [0x22] + sections.dropFirst(19))
        }
        XCTAssertEqual(frames(rows).compactMap { $0["hex"] as? String }, [volume.hexString, sections.hexString])
        XCTAssertEqual(frames(rows).compactMap { $0["char"] as? String }, ["B2CE", "B4E0"])
    }

    func testSingleIndexedChunkAndFragmentedShortStreamsRemainIndependent() throws {
        let rows = try capture { log in
            log.record(direction: "v1->dut", characteristic: short, bytes: Array(volume.prefix(6)))
            log.record(direction: "dut->v1", characteristic: short, bytes: volume)
            log.record(direction: "v1->dut", characteristic: long, bytes: [0x11] + volume)
            log.record(direction: "v1->dut", characteristic: short, bytes: Array(volume.dropFirst(6)))
        }
        XCTAssertEqual(frames(rows).compactMap { $0["hex"] as? String }, Array(repeating: volume.hexString, count: 3))
        XCTAssertEqual(frames(rows).compactMap { $0["dir"] as? String }, ["dut->v1", "v1->dut", "v1->dut"])
        XCTAssertEqual(frames(rows).compactMap { $0["char"] as? String }, ["B2CE", "B4E0", "B2CE"])
    }

    func testIncompleteAndInvalidIndexedCandidatesCannotEmitFrames() throws {
        // A missing part, invalid index/count, empty part, oversized part, and
        // a complete envelope containing a truncated ESP frame are not frames.
        for invalid in [[UInt8(0x02), 0xAA], [0x32, 0xAA], [0x10, 0xAA], [0x12],
                        [0x12] + Array(repeating: 0, count: 20), [0x11] + Array(volume.dropLast())] {
            let rows = try capture { log in
                log.record(direction: "v1->dut", characteristic: long, bytes: [0x12] + sections.prefix(19))
                log.record(direction: "v1->dut", characteristic: long, bytes: invalid)
                log.record(direction: "v1->dut", characteristic: long, bytes: [0x22] + sections.dropFirst(19))
                log.record(direction: "v1->dut", characteristic: short, bytes: volume)
                log.record(direction: "v1->dut", characteristic: long, bytes: [0x11] + volume)
            }
            XCTAssertEqual(frames(rows).compactMap { $0["hex"] as? String }, [volume.hexString, volume.hexString])
            XCTAssertEqual(frames(rows).compactMap { $0["char"] as? String }, ["B2CE", "B4E0"])
        }
    }

    func testRepeatedIndexAndChangedCountDiscardEarlierCandidate() throws {
        let rows = try capture { log in
            // Repeating index 2 starts a new candidate; it cannot retain index 1.
            log.record(direction: "v1->dut", characteristic: long, bytes: [0x13] + sections.prefix(10))
            log.record(direction: "v1->dut", characteristic: long, bytes: [0x23] + sections[10..<20])
            log.record(direction: "v1->dut", characteristic: long, bytes: [0x23] + sections[10..<20])
            log.record(direction: "v1->dut", characteristic: long, bytes: [0x33] + sections.dropFirst(20))
            log.note("before_new_frame")
            // A different count replaces the unfinished three-part candidate.
            log.record(direction: "v1->dut", characteristic: long, bytes: [0x11] + volume)
        }
        let boundary = try XCTUnwrap(rows.firstIndex { $0["event"] as? String == "before_new_frame" })
        XCTAssertTrue(frames(Array(rows[..<boundary])).isEmpty)
        XCTAssertEqual(frames(rows).compactMap { $0["hex"] as? String }, [volume.hexString])
    }

    func testResetDiscardsBothPartialFormsBeforeTheNextConnection() throws {
        let rows = try capture { log in
            log.record(direction: "v1->dut", characteristic: short, bytes: Array(volume.prefix(6)))
            log.record(direction: "v1->dut", characteristic: long, bytes: [0x12] + sections.prefix(19))
            log.resetReassembly()
            log.record(direction: "v1->dut", characteristic: short, bytes: Array(volume.dropFirst(6)))
            log.record(direction: "v1->dut", characteristic: long, bytes: [0x22] + sections.dropFirst(19))
            log.note("new_connection")
            log.record(direction: "v1->dut", characteristic: long, bytes: [0x11] + volume)
        }
        let boundary = try XCTUnwrap(rows.firstIndex { $0["event"] as? String == "new_connection" })
        XCTAssertTrue(frames(Array(rows[..<boundary])).isEmpty)
        XCTAssertEqual(frames(rows).compactMap { $0["hex"] as? String }, [volume.hexString])
    }

    func testCommandLinkResetPreservesDetectorAssemblyAndIndexedCommandCapture() throws {
        let command = CBUUID(string: V1.commandUUID)
        let longCommand = CBUUID(string: V1.commandLongUUID)
        let request: [UInt8] = [0xAA, 0xDA, 0xE6, 0x01, 0x01, 0x6C, 0xAB]
        let rows = try capture { log in
            log.record(direction: "v1->dut", characteristic: long, bytes: [0x12] + sections.prefix(19))
            log.record(direction: "dut->v1", characteristic: command, bytes: Array(request.prefix(5)))
            log.resetReassembly(direction: "dut->v1")
            log.record(direction: "dut->v1", characteristic: command, bytes: Array(request.dropFirst(5)))
            log.record(direction: "v1->dut", characteristic: long, bytes: [0x22] + sections.dropFirst(19))
            log.record(direction: "dut->v1", characteristic: longCommand, bytes: [0x11] + request)
        }
        XCTAssertEqual(frames(rows).compactMap { $0["hex"] as? String }, [sections.hexString, request.hexString])
        XCTAssertEqual(frames(rows).compactMap { $0["char"] as? String }, ["B4E0", "B8D2"])
        XCTAssertEqual(frames(rows).compactMap { $0["payload"] as? String }, [
            "13 8C E8 89 23 23 89 1F 87 D6 33 87 D2 82 67", ""
        ])
    }
}

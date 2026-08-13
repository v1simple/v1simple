import XCTest
@testable import v1replay

final class V1ProtocolContractTests: XCTestCase {
    /// Public behavior ID: `V1-BLE-IDENTITY-001`.
    func testBLEUUIDLiteralsDistinguishCoreAndCompatibilitySurface() {
        // This pins string identity only. CoreBluetooth registration, properties,
        // subscriptions, and notification delivery remain integration/bench evidence.
        XCTAssertEqual(V1.serviceUUID, "92A0AFF4-9E05-11E2-AA59-F23C91AEC05E")

        let coreCharacteristics = [
            V1.displayShortUUID,
            V1.displayLongUUID,
            V1.commandUUID,
            V1.commandLongUUID,
        ]
        XCTAssertEqual(coreCharacteristics, [
            "92A0B2CE-9E05-11E2-AA59-F23C91AEC05E",
            "92A0B4E0-9E05-11E2-AA59-F23C91AEC05E",
            "92A0B6D4-9E05-11E2-AA59-F23C91AEC05E",
            "92A0B8D2-9E05-11E2-AA59-F23C91AEC05E",
        ])
        XCTAssertEqual(Set(coreCharacteristics).count, coreCharacteristics.count)

        let compatibilityCharacteristics = [V1.notifyAltUUID, V1.commandAltUUID]
        XCTAssertEqual(compatibilityCharacteristics, [
            "92A0BCE0-9E05-11E2-AA59-F23C91AEC05E",
            "92A0BAD4-9E05-11E2-AA59-F23C91AEC05E",
        ])
    }

    /// Public behavior ID: `V1-VERSION-REPLY-001`.
    func testVersionRequestToReplyDecisionMatchesFramingContract() throws {
        // Request and expected reply are literal contract vectors. The independent
        // decoder below does not use production framing or checksum helpers.
        let rawRequest: [UInt8] = [0xAA, 0xDA, 0xE6, 0x01, 0x01, 0x6C, 0xAB]
        let decodedRequest = try ContractFrame.decode(rawRequest)

        XCTAssertEqual(decodedRequest.destination, 0xDA)
        XCTAssertEqual(decodedRequest.origin, 0xE6)
        XCTAssertEqual(decodedRequest.packetID, 0x01)
        XCTAssertEqual(decodedRequest.payload, [])

        var productionBuffer = rawRequest
        let productionRequests = V1.drainFrames(from: &productionBuffer)
        XCTAssertTrue(productionBuffer.isEmpty)
        let request = try XCTUnwrap(productionRequests.first)
        XCTAssertEqual(productionRequests.count, 1)
        XCTAssertEqual(request.raw, rawRequest)

        let decision = try XCTUnwrap(V1.replyDecision(for: request, version: "4.1038"))
        // The pure route is testable here; actual B2CE notification delivery remains
        // CoreBluetooth integration/bench evidence.
        XCTAssertEqual(decision.channel, .displayShort)
        XCTAssertEqual(decision.bytes, [
            0xAA, 0xD6, 0xEA, 0x02, 0x08,
            0x76, 0x34, 0x2E, 0x31, 0x30, 0x33, 0x38,
            0x18, 0xAB,
        ])

        let reply = try ContractFrame.decode(decision.bytes)
        XCTAssertEqual(reply.destination, 0xD6)
        XCTAssertEqual(reply.origin, 0xEA)
        XCTAssertEqual(reply.packetID, 0x02)
        XCTAssertEqual(reply.payload, Array("v4.1038".utf8))
    }
}

private struct ContractFrame {
    let destination: UInt8
    let origin: UInt8
    let packetID: UInt8
    let payload: [UInt8]

    static func decode(_ bytes: [UInt8]) throws -> ContractFrame {
        guard bytes.count >= 7 else { throw ContractError.tooShort }
        guard bytes.first == 0xAA, bytes.last == 0xAB else {
            throw ContractError.badBoundary
        }

        let declaredLength = Int(bytes[4])
        guard declaredLength >= 1, bytes.count == 6 + declaredLength else {
            throw ContractError.badLength
        }

        let checksumIndex = bytes.count - 2
        let expectedChecksum = bytes[..<checksumIndex].reduce(UInt8(0), &+)
        guard bytes[checksumIndex] == expectedChecksum else {
            throw ContractError.badChecksum
        }

        return ContractFrame(
            destination: bytes[1],
            origin: bytes[2],
            packetID: bytes[3],
            payload: Array(bytes[5..<checksumIndex])
        )
    }
}

private enum ContractError: Error {
    case tooShort
    case badBoundary
    case badLength
    case badChecksum
}

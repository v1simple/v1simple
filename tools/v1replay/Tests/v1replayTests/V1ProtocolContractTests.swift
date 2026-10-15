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

    func testManualCribRepliesUseCanonicalTargetedFraming() throws {
        let replies = V1.cribReplyPackets(version: "4.1038", main: 4, muted: 0)
        XCTAssertEqual(replies.version, [
            0xAA, 0xD6, 0xEA, 0x02, 0x08,
            0x76, 0x34, 0x2E, 0x31, 0x30, 0x33, 0x38,
            0x18, 0xAB,
        ])
        XCTAssertEqual(replies.allVolume, [
            0xAA, 0xD6, 0xEA, 0x3D, 0x05,
            0x04, 0x00, 0x04, 0x00,
            0xB4, 0xAB,
        ])

        XCTAssertEqual(try ContractFrame.decode(replies.version).destination, 0xD6)
        XCTAssertEqual(try ContractFrame.decode(replies.allVolume).origin, 0xEA)
    }

    func testDrainFramesPreservesAllChecksumFreeV1VolumeBytes() throws {
        let raw: [UInt8] = [
            0xAA, 0xD6, 0xE9, 0x3D, 0x04,
            0x04, 0x00, 0x04, 0x00, 0xAB,
        ]
        var buffer = raw
        let frames = V1.drainFrames(from: &buffer)

        XCTAssertTrue(buffer.isEmpty)
        XCTAssertEqual(frames.count, 1)
        let frame = try XCTUnwrap(frames.first)
        XCTAssertEqual(frame.id, 0x3D)
        XCTAssertEqual(frame.payload, [0x04, 0x00, 0x04, 0x00])
        XCTAssertEqual(frame.raw, raw)
    }

    func testDrainFramesExcludesChecksumFromChecksummedV1VolumeBytes() throws {
        let raw: [UInt8] = [
            0xAA, 0xD6, 0xEA, 0x3D, 0x05,
            0x04, 0x00, 0x04, 0x00, 0xB4, 0xAB,
        ]
        var buffer = raw
        let frames = V1.drainFrames(from: &buffer)

        XCTAssertTrue(buffer.isEmpty)
        XCTAssertEqual(frames.count, 1)
        let frame = try XCTUnwrap(frames.first)
        XCTAssertEqual(frame.id, 0x3D)
        XCTAssertEqual(frame.payload, [0x04, 0x00, 0x04, 0x00])
        XCTAssertEqual(frame.raw, raw)
    }

    func testManualCribPresentationUsesCanonicalBroadcastFraming() throws {
        let packets = V1.cribPresentationPackets()
        let decoded = try [packets.alert, packets.oneBar, packets.sixBars].map(ContractFrame.decode)

        XCTAssertEqual(decoded.map(\.destination), [0xD8, 0xD8, 0xD8])
        XCTAssertEqual(decoded.map(\.origin), [0xEA, 0xEA, 0xEA])
        XCTAssertEqual(decoded.map(\.packetID), [0x43, 0x31, 0x31])
        XCTAssertEqual(decoded.map(\.payload.count), [7, 8, 8])
    }

    func testChecksumFreeV1InformationUsesCanonicalE9Framing() {
        let alert = V1.AlertRow.single(
            bars: 1, band: .ka, direction: .front, frequencyMHz: 34_700, priority: true
        ).packet(header: .broadcastInformation, checksum: false)
        let display = V1.DisplayFrame.alerting(
            bars: 1, band: .ka, direction: .front, bogeyCount: 1,
            muted: false, volume: 0x40, displayOn: true
        ).packet(header: .broadcastInformation, checksum: false)

        XCTAssertEqual(alert, [
            0xAA, 0xD8, 0xE9, 0x43, 0x07,
            0x11, 0x87, 0x8C, 0x80, 0x00, 0x22, 0x80, 0xAB,
        ])
        XCTAssertEqual(display, [
            0xAA, 0xD8, 0xE9, 0x31, 0x08,
            0x06, 0x06, 0x01, 0x22, 0x22, 0x0C, 0x0C, 0x40, 0xAB,
        ])
    }

    func testChecksumFreeV1SweepRepliesUseCanonicalE9Framing() {
        XCTAssertEqual(V1.maxSweepIndexPacket(header: .v1ToApp, checksum: false), [
            0xAA, 0xD6, 0xE9, 0x20, 0x01, 0x01, 0xAB,
        ])
        XCTAssertEqual(V1.sweepWriteResultPacket(header: .v1ToApp, result: 0, checksum: false), [
            0xAA, 0xD6, 0xE9, 0x21, 0x01, 0x00, 0xAB,
        ])
    }

    func testHistoricalDraftHeaderPreservesChecksumFreePacketParity() {
        let display = V1.DisplayFrame.alerting(
            bars: 1, band: .ka, direction: .front, bogeyCount: 1,
            muted: false, volume: 0x40, displayOn: true,
            blinkPlane: true, includeModeBits: false
        ).packet(header: .draft, checksum: false)
        XCTAssertEqual(display, [
            0xAA, 0xDA, 0xE4, 0x31, 0x08,
            0x06, 0x00, 0x01, 0x22, 0x22, 0x0C, 0x00, 0x40, 0xAB,
        ])
    }

    func testSweepAndFlowControlUseTheirSpecifiedDestinations() throws {
        let rejected = V1.requestNotProcessedPacket(
            header: .v1ToApp,
            requestID: V1.PacketID.reqMaxSweepIndex.rawValue,
            checksum: true
        )
        let busy = V1.busyPacket(
            requestIDs: [V1.PacketID.reqAllSweepDefinitions.rawValue],
            checksum: true
        )
        let sections = V1.sweepSectionsPacket(header: .v1ToApp, checksum: true)
        let maximum = V1.maxSweepIndexPacket(header: .v1ToApp, checksum: true)
        let definitions = V1.sweepDefinitionPackets(header: .v1ToApp, checksum: true)

        XCTAssertEqual(try ContractFrame.decode(rejected).payload, [0x19])
        XCTAssertEqual(try ContractFrame.decode(busy).payload, [0x16])
        XCTAssertEqual(busy, [0xAA, 0xD8, 0xEA, 0x66, 0x02, 0x16, 0xEA, 0xAB])
        XCTAssertEqual(try ContractFrame.decode(sections).packetID, 0x23)
        XCTAssertEqual(try ContractFrame.decode(sections).payload.count, 10)
        XCTAssertEqual(try ContractFrame.decode(maximum).payload, [0x01])
        XCTAssertEqual(definitions.count, 2)
        XCTAssertEqual(try definitions.map(ContractFrame.decode).map(\.packetID), [0x17, 0x17])
        XCTAssertTrue(([rejected, sections, maximum] + definitions).allSatisfy {
            $0[1] == 0xD6 && $0[2] == 0xEA
        })
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

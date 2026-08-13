import Foundation
import XCTest
@testable import v1replay

final class V1SessionContractTests: XCTestCase {

    func testEveryVersionRequestSplitPointProducesOneLiteralShortReply() {
        let request: [UInt8] = [0xAA, 0xDA, 0xE6, 0x01, 0x01, 0x6C, 0xAB]
        let expected = V1.Session.Effect.reply(V1.ReplyDecision(
            channel: .displayShort,
            bytes: [
                0xAA, 0xD6, 0xEA, 0x02, 0x08,
                0x76, 0x34, 0x2E, 0x31, 0x30, 0x33, 0x38,
                0x18, 0xAB,
            ]
        ))

        for splitPoint in 1..<request.count {
            var session = V1.Session()
            XCTAssertEqual(
                session.receive(Array(request[..<splitPoint])),
                [],
                "split \(splitPoint) acted before the frame was complete"
            )
            XCTAssertEqual(session.bufferedByteCount, splitPoint)

            let completed = session.receive(Array(request[splitPoint...]))
            XCTAssertEqual(completed.map(\.packet.id), [0x01])
            XCTAssertEqual(completed[0].effects, [expected])
            XCTAssertEqual(session.bufferedByteCount, 0)
        }
    }

    /// Public behavior IDs: `V1-SESSION-TRANSPORT-001`,
    /// `V1-ALERT-STREAM-CONTROL-001`, and `V1-VERSION-REPLY-001`.
    func testTransportRetainsFragmentsOrdersFramesAndTracksReadiness() throws {
        var session = V1.Session()
        let central = try XCTUnwrap(UUID(uuidString: "00000000-0000-0000-0000-000000000001"))

        XCTAssertFalse(session.readiness.shortTrafficReady)
        XCTAssertFalse(session.readiness.alertStreamReady)
        XCTAssertFalse(session.readiness.longTrafficSubscribed)

        session.subscribe(central: central, channel: .displayShort)
        XCTAssertTrue(session.readiness.shortTrafficReady)
        XCTAssertFalse(session.readiness.alertStreamReady)

        let startAlert: [UInt8] = [0xAA, 0xDA, 0xE6, 0x41, 0x01, 0xAC, 0xAB]
        XCTAssertEqual(session.receive(Array(startAlert.prefix(4))), [])
        XCTAssertEqual(session.bufferedByteCount, 4)

        let started = session.receive(Array(startAlert.dropFirst(4)))
        XCTAssertEqual(started.map(\.packet.id), [0x41])
        XCTAssertEqual(started[0].effects, [.alertDataChanged(true)])
        XCTAssertTrue(session.readiness.alertStreamReady)

        // B4E0 remains observable optional capacity, but current display and
        // alert-row readiness does not depend on a long-packet subscription.
        session.subscribe(central: central, channel: .displayLong)
        XCTAssertEqual(session.subscriberCount, 1)
        XCTAssertTrue(session.readiness.longTrafficSubscribed)
        XCTAssertTrue(session.readiness.alertStreamReady)

        let stopAlert: [UInt8] = [0xAA, 0xDA, 0xE6, 0x42, 0x01, 0xAD, 0xAB]
        let version: [UInt8] = [0xAA, 0xDA, 0xE6, 0x01, 0x01, 0x6C, 0xAB]
        let allVolume: [UInt8] = [0xAA, 0xDA, 0xE6, 0x3C, 0x01, 0xA7, 0xAB]
        let coalesced = stopAlert + version + Array(allVolume.prefix(4))
        let ordered = session.receive(coalesced)

        XCTAssertEqual(ordered.map(\.packet.id), [0x42, 0x01])
        XCTAssertEqual(ordered[0].effects, [.alertDataChanged(false)])
        XCTAssertEqual(ordered[1].effects, [.reply(V1.ReplyDecision(
            channel: .displayShort,
            bytes: [
                0xAA, 0xD6, 0xEA, 0x02, 0x08,
                0x76, 0x34, 0x2E, 0x31, 0x30, 0x33, 0x38,
                0x18, 0xAB,
            ]
        ))])
        XCTAssertFalse(session.readiness.alertDataRequested)
        XCTAssertEqual(session.bufferedByteCount, 4)

        let completedTail = session.receive(Array(allVolume.dropFirst(4)))
        XCTAssertEqual(completedTail.map(\.packet.id), [0x3C])
        XCTAssertEqual(completedTail[0].effects, [.reply(V1.ReplyDecision(
            channel: .displayShort,
            bytes: [
                0xAA, 0xD6, 0xEA, 0x3D, 0x05,
                0x04, 0x00, 0x04, 0x00,
                0xB4, 0xAB,
            ]
        ))])
        XCTAssertEqual(session.bufferedByteCount, 0)

        _ = session.receive(startAlert)
        _ = session.receive(Array(version.prefix(3)))
        XCTAssertTrue(session.readiness.alertDataRequested)
        XCTAssertEqual(session.bufferedByteCount, 3)
        XCTAssertEqual(session.unsubscribe(central: central, channel: .displayShort), 1)
        XCTAssertFalse(session.readiness.shortTrafficReady)
        XCTAssertEqual(session.unsubscribe(central: central, channel: .displayLong), 0)
        XCTAssertFalse(session.readiness.alertDataRequested)
        XCTAssertEqual(session.bufferedByteCount, 0)
    }

    /// Public behavior ID: `V1-ALL-VOLUME-001`.
    func testAllVolumeReplyRoutesShortAndKeepsConfiguredFieldOrder() {
        var config = V1.Session.Config()
        config.mainVolume = 1
        config.mutedVolume = 2
        config.savedMainVolume = 3
        config.savedMutedVolume = 4
        var session = V1.Session(config: config)

        let request: [UInt8] = [0xAA, 0xDA, 0xE6, 0x3C, 0x01, 0xA7, 0xAB]
        let outcomes = session.receive(request)

        XCTAssertEqual(outcomes.count, 1)
        XCTAssertEqual(outcomes[0].effects, [.reply(V1.ReplyDecision(
            channel: .displayShort,
            bytes: [
                0xAA, 0xD6, 0xEA, 0x3D, 0x05,
                0x01, 0x02, 0x03, 0x04,
                0xB6, 0xAB,
            ]
        ))])
    }

    func testDuplicateStartIsDeterministicAndStopEmitsNoReply() {
        var session = V1.Session()
        let start: [UInt8] = [0xAA, 0xDA, 0xE6, 0x41, 0x01, 0xAC, 0xAB]
        let stop: [UInt8] = [0xAA, 0xDA, 0xE6, 0x42, 0x01, 0xAD, 0xAB]

        for _ in 0..<2 {
            let outcomes = session.receive(start)
            XCTAssertEqual(outcomes.map(\.effects), [[.alertDataChanged(true)]])
            XCTAssertTrue(session.alertDataRequested)
            XCTAssertFalse(outcomes.flatMap(\.effects).contains { effect in
                if case .reply = effect { return true }
                return false
            })
        }

        let stopped = session.receive(stop)
        XCTAssertEqual(stopped.map(\.effects), [[.alertDataChanged(false)]])
        XCTAssertFalse(session.alertDataRequested)
        XCTAssertFalse(stopped.flatMap(\.effects).contains { effect in
            if case .reply = effect { return true }
            return false
        })
    }

    func testCoalescedStartStopExposeWireOrderStateAtDispatchBoundary() {
        var session = V1.Session()
        let start: [UInt8] = [0xAA, 0xDA, 0xE6, 0x41, 0x01, 0xAC, 0xAB]
        let stop: [UInt8] = [0xAA, 0xDA, 0xE6, 0x42, 0x01, 0xAD, 0xAB]

        session.append(start + stop)
        var observedEffects: [[V1.Session.Effect]] = []
        var callbackVisibleStates: [Bool] = []
        while let outcome = session.nextOutcome() {
            observedEffects.append(outcome.effects)
            callbackVisibleStates.append(session.alertDataRequested)
        }

        XCTAssertEqual(observedEffects, [
            [.alertDataChanged(true)],
            [.alertDataChanged(false)],
        ])
        XCTAssertEqual(callbackVisibleStates, [true, false])
    }

    /// Public behavior ID: `V1-USER-BYTES-001`.
    func testUserBytesRespectVersionWidthAndWritesHaveNoImmediateReply() {
        let readRequest: [UInt8] = [0xAA, 0xDA, 0xE6, 0x11, 0x01, 0x7C, 0xAB]
        var defaultSession = V1.Session()

        XCTAssertEqual(defaultSession.storedUserBytes, [0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF])
        XCTAssertEqual(defaultSession.receive(readRequest)[0].effects, [
            .reply(V1.ReplyDecision(
                channel: .displayShort,
                bytes: [
                    0xAA, 0xD6, 0xEA, 0x12, 0x07,
                    0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF,
                    0x81, 0xAB,
                ]
            ))
        ])

        let fourByteEraWrite: [UInt8] = [
            0xAA, 0xDA, 0xE6, 0x13, 0x07,
            0x01, 0x02, 0x03, 0x04, 0xA5, 0x5A,
            0x8D, 0xAB,
        ]
        let stored = defaultSession.receive(fourByteEraWrite)
        XCTAssertEqual(stored[0].effects, [
            .userBytesStored([0x01, 0x02, 0x03, 0x04, 0xFF, 0xFF])
        ])
        XCTAssertFalse(stored[0].effects.contains { effect in
            if case .reply = effect { return true }
            return false
        })
        XCTAssertEqual(defaultSession.receive(readRequest)[0].effects, [
            .reply(V1.ReplyDecision(
                channel: .displayShort,
                bytes: [
                    0xAA, 0xD6, 0xEA, 0x12, 0x07,
                    0x01, 0x02, 0x03, 0x04, 0xFF, 0xFF,
                    0x8B, 0xAB,
                ]
            ))
        ])

        var sixByteConfig = V1.Session.Config()
        sixByteConfig.version = "4.1039"
        sixByteConfig.userBytes = [0x91, 0x82, 0x73, 0x64, 0x55, 0x46]
        var sixByteSession = V1.Session(config: sixByteConfig)
        XCTAssertEqual(
            sixByteSession.storedUserBytes,
            [0x91, 0x82, 0x73, 0x64, 0x55, 0x46]
        )
        let sixByteWrite: [UInt8] = [
            0xAA, 0xDA, 0xE6, 0x13, 0x07,
            0x10, 0x20, 0x30, 0x40, 0x50, 0x60,
            0xD4, 0xAB,
        ]
        XCTAssertEqual(sixByteSession.receive(sixByteWrite)[0].effects, [
            .userBytesStored([0x10, 0x20, 0x30, 0x40, 0x50, 0x60])
        ])
        XCTAssertEqual(sixByteSession.receive(readRequest)[0].effects, [
            .reply(V1.ReplyDecision(
                channel: .displayShort,
                bytes: [
                    0xAA, 0xD6, 0xEA, 0x12, 0x07,
                    0x10, 0x20, 0x30, 0x40, 0x50, 0x60,
                    0xD3, 0xAB,
                ]
            ))
        ])
    }

    func testInvalidHeaderChecksumAndShortUserWriteProduceOnlyRejections() {
        var config = V1.Session.Config()
        config.version = "4.1039"
        var session = V1.Session(config: config)

        let wrongHeader: [UInt8] = [0xAA, 0xDB, 0xE6, 0x01, 0x01, 0x6D, 0xAB]
        let broadcastDestination: [UInt8] = [0xAA, 0xD8, 0xE6, 0x01, 0x01, 0x6A, 0xAB]
        let wrongOrigin: [UInt8] = [0xAA, 0xDA, 0xE5, 0x01, 0x01, 0x6B, 0xAB]
        let wrongChecksum: [UInt8] = [0xAA, 0xDA, 0xE6, 0x01, 0x01, 0x6D, 0xAB]
        let shortUserWrite: [UInt8] = [
            0xAA, 0xDA, 0xE6, 0x13, 0x06,
            0x00, 0x00, 0x00, 0x00, 0x00,
            0x83, 0xAB,
        ]

        let rejected = session.receive(
            wrongHeader + broadcastDestination + wrongOrigin
                + wrongChecksum + shortUserWrite
        )
        XCTAssertEqual(rejected.map(\.effects), [
            [.rejected(.invalidRequestHeader)],
            [.rejected(.invalidRequestHeader)],
            [.rejected(.invalidRequestHeader)],
            [.rejected(.invalidChecksum)],
            [.rejected(.invalidUserBytesLength(5))],
        ])
        XCTAssertEqual(session.storedUserBytes, [0x00, 0x00, 0x00, 0x00, 0x00, 0x00])
        XCTAssertFalse(rejected.flatMap(\.effects).contains { effect in
            if case .reply = effect { return true }
            return false
        })
    }

    func testPayloadFreeRequestsRejectExtraDataWithoutActing() {
        var session = V1.Session()
        let invalidAllVolume: [UInt8] = [
            0xAA, 0xDA, 0xE6, 0x3C, 0x02, 0x00, 0xA8, 0xAB,
        ]
        let invalidUserRead: [UInt8] = [
            0xAA, 0xDA, 0xE6, 0x11, 0x02, 0x00, 0x7D, 0xAB,
        ]
        let invalidStart: [UInt8] = [
            0xAA, 0xDA, 0xE6, 0x41, 0x02, 0x00, 0xAD, 0xAB,
        ]

        let rejected = session.receive(
            invalidAllVolume + invalidUserRead + invalidStart
        )
        XCTAssertEqual(rejected.map(\.effects), [
            [.rejected(.unexpectedPayload(packetID: 0x3C, count: 1))],
            [.rejected(.unexpectedPayload(packetID: 0x11, count: 1))],
            [.rejected(.unexpectedPayload(packetID: 0x41, count: 1))],
        ])
        XCTAssertFalse(session.alertDataRequested)
        XCTAssertFalse(rejected.flatMap(\.effects).contains { effect in
            if case .reply = effect { return true }
            return false
        })

        let validStart: [UInt8] = [0xAA, 0xDA, 0xE6, 0x41, 0x01, 0xAC, 0xAB]
        let invalidStop: [UInt8] = [
            0xAA, 0xDA, 0xE6, 0x42, 0x02, 0x00, 0xAE, 0xAB,
        ]
        _ = session.receive(validStart)
        XCTAssertEqual(session.receive(invalidStop).map(\.effects), [
            [.rejected(.unexpectedPayload(packetID: 0x42, count: 1))],
        ])
        XCTAssertTrue(session.alertDataRequested)
    }

    func testLargeCoalescedWriteDrainsEveryFrameBeforeBoundingPartialTail() {
        var session = V1.Session()
        let version: [UInt8] = [0xAA, 0xDA, 0xE6, 0x01, 0x01, 0x6C, 0xAB]
        let completeCount = 80
        let burst = Array(repeating: version, count: completeCount).flatMap { $0 }

        let outcomes = session.receive(burst + Array(version.prefix(4)))

        XCTAssertEqual(outcomes.count, completeCount)
        XCTAssertEqual(outcomes.map(\.packet.id), Array(repeating: 0x01, count: completeCount))
        XCTAssertTrue(outcomes.allSatisfy { outcome in
            outcome.effects == [.reply(V1.ReplyDecision(
                channel: .displayShort,
                bytes: [
                    0xAA, 0xD6, 0xEA, 0x02, 0x08,
                    0x76, 0x34, 0x2E, 0x31, 0x30, 0x33, 0x38,
                    0x18, 0xAB,
                ]
            ))]
        })
        XCTAssertEqual(session.bufferedByteCount, 4)

        XCTAssertEqual(session.receive(Array(version.dropFirst(4))).count, 1)
        XCTAssertEqual(session.bufferedByteCount, 0)

        XCTAssertEqual(session.receive(Array(repeating: 0x55, count: 1_024)), [])
        XCTAssertEqual(session.bufferedByteCount, 0)
    }

    func testInboundChecksumRemainsMandatoryWhenOutboundChecksumIsDisabled() {
        var config = V1.Session.Config()
        config.outboundChecksum = false
        var session = V1.Session(config: config)
        let valid: [UInt8] = [0xAA, 0xDA, 0xE6, 0x01, 0x01, 0x6C, 0xAB]
        let invalid: [UInt8] = [0xAA, 0xDA, 0xE6, 0x01, 0x01, 0x6D, 0xAB]

        XCTAssertEqual(session.receive(valid)[0].effects, [.reply(V1.ReplyDecision(
            channel: .displayShort,
            bytes: [
                0xAA, 0xD6, 0xEA, 0x02, 0x07,
                0x76, 0x34, 0x2E, 0x31, 0x30, 0x33, 0x38,
                0xAB,
            ]
        ))])
        XCTAssertEqual(session.receive(invalid)[0].effects, [
            .rejected(.invalidChecksum)
        ])
    }

    func testStateCommandsValidatePayloadsAndNeverInventReplies() {
        var session = V1.Session()
        let valid: [[UInt8]] = [
            [0xAA, 0xDA, 0xE6, 0x34, 0x01, 0x9F, 0xAB],
            [0xAA, 0xDA, 0xE6, 0x35, 0x01, 0xA0, 0xAB],
            [0xAA, 0xDA, 0xE6, 0x32, 0x01, 0x9D, 0xAB],
            [0xAA, 0xDA, 0xE6, 0x32, 0x02, 0x00, 0x9E, 0xAB],
            [0xAA, 0xDA, 0xE6, 0x32, 0x02, 0x01, 0x9F, 0xAB],
            [0xAA, 0xDA, 0xE6, 0x33, 0x01, 0x9E, 0xAB],
        ]

        XCTAssertEqual(session.receive(valid.flatMap { $0 }).map(\.effects), [
            [.muteChanged(true)],
            [.muteChanged(false)],
            [.displayPowerChanged(false)],
            [.displayPowerChanged(false)],
            [.displayPowerChanged(false)],
            [.displayPowerChanged(true)],
        ])

        let malformed: [[UInt8]] = [
            [0xAA, 0xDA, 0xE6, 0x34, 0x02, 0x99, 0x39, 0xAB],
            [0xAA, 0xDA, 0xE6, 0x35, 0x02, 0x99, 0x3A, 0xAB],
            [0xAA, 0xDA, 0xE6, 0x33, 0x02, 0x99, 0x38, 0xAB],
            [0xAA, 0xDA, 0xE6, 0x32, 0x02, 0x02, 0xA0, 0xAB],
            [0xAA, 0xDA, 0xE6, 0x32, 0x03, 0x00, 0x01, 0xA0, 0xAB],
        ]
        XCTAssertEqual(session.receive(malformed.flatMap { $0 }).map(\.effects), [
            [.rejected(.unexpectedPayload(packetID: 0x34, count: 1))],
            [.rejected(.unexpectedPayload(packetID: 0x35, count: 1))],
            [.rejected(.unexpectedPayload(packetID: 0x33, count: 1))],
            [.rejected(.unexpectedPayload(packetID: 0x32, count: 1))],
            [.rejected(.unexpectedPayload(packetID: 0x32, count: 2))],
        ])

        let mode: [UInt8] = [0xAA, 0xDA, 0xE6, 0x36, 0x02, 0x03, 0xA5, 0xAB]
        let volume: [UInt8] = [
            0xAA, 0xDA, 0xE6, 0x39, 0x04, 0x04, 0x00, 0x00, 0xAB, 0xAB,
        ]
        let accepted = session.receive(mode + volume)
        XCTAssertEqual(accepted.map(\.effects), [
            [.acceptedWithoutReply], [.acceptedWithoutReply],
        ])
        XCTAssertFalse(accepted.flatMap(\.effects).contains { effect in
            if case .reply = effect { return true }
            return false
        })

        let emptyMode: [UInt8] = [
            0xAA, 0xDA, 0xE6, 0x36, 0x01, 0xA1, 0xAB,
        ]
        let longMode: [UInt8] = [
            0xAA, 0xDA, 0xE6, 0x36, 0x03, 0x03, 0x00, 0xA6, 0xAB,
        ]
        let shortVolume: [UInt8] = [
            0xAA, 0xDA, 0xE6, 0x39, 0x03, 0x04, 0x00, 0xAA, 0xAB,
        ]
        let longVolume: [UInt8] = [
            0xAA, 0xDA, 0xE6, 0x39, 0x05, 0x04, 0x00, 0x00, 0x00, 0xAC, 0xAB,
        ]
        XCTAssertEqual(
            session.receive(emptyMode + longMode + shortVolume + longVolume).map(\.effects),
            [
            [.rejected(.unexpectedPayload(packetID: 0x36, count: 0))],
            [.rejected(.unexpectedPayload(packetID: 0x36, count: 2))],
            [.rejected(.unexpectedPayload(packetID: 0x39, count: 2))],
            [.rejected(.unexpectedPayload(packetID: 0x39, count: 4))],
        ])
    }
}

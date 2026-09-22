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

        let factoryBytes = Array(repeating: UInt8(0xFF), count: 6)
        XCTAssertEqual(defaultSession.storedUserBytes, factoryBytes)
        XCTAssertEqual(defaultSession.receive(readRequest)[0].effects, [
            .reply(V1.ReplyDecision(
                channel: .displayShort,
                bytes: [
                    0xAA, 0xD6, 0xEA, 0x12, 0x07,
                    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                    0x7D, 0xAB,
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
        XCTAssertEqual(session.storedUserBytes, Array(repeating: 0xFF, count: 6))
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
            [.modeChanged(.advancedLogic)],
            [.volumeChanged(V1.Session.ControlState(
                mode: .advancedLogic,
                mainVolume: 4,
                mutedVolume: 0,
                savedMainVolume: 4,
                savedMutedVolume: 0
            ))],
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

    /// Public behavior ID: `V1-CONTROL-VOLUME-001`.
    func testDetectorCurrentVolumeAppliesWithoutAnInboundCommand() {
        var session = V1.Session()
        let authoredPairs: [(main: UInt8, muted: UInt8, displayByte: UInt8)] = [
            (4, 0, 0x40),
            (7, 0, 0x70),
            (7, 2, 0x72),
            (4, 2, 0x42),
            (4, 0, 0x40),
        ]

        for pair in authoredPairs {
            let applied = session.applyDetectorCurrentVolume(
                main: pair.main,
                muted: pair.muted
            )
            XCTAssertEqual(applied.mainVolume, pair.main)
            XCTAssertEqual(applied.mutedVolume, pair.muted)
            XCTAssertEqual(applied.displayVolume, pair.displayByte)
            XCTAssertEqual(applied.savedMainVolume, 4)
            XCTAssertEqual(applied.savedMutedVolume, 0)
            XCTAssertEqual(session.controlState, applied)
        }
    }

    /// Public behavior ID: `V1-CONTROL-VOLUME-001`.
    func testCoalescedVolumeWriteMutatesCurrentInOrderAndKeepsSavedPair() {
        var session = V1.Session()
        let allVolume: [UInt8] = [
            0xAA, 0xDA, 0xE6, 0x3C, 0x01, 0xA7, 0xAB,
        ]
        let writeSevenTwo: [UInt8] = [
            0xAA, 0xDA, 0xE6, 0x39, 0x04, 0x07, 0x02, 0x00, 0xB0, 0xAB,
        ]

        let outcomes = session.receive(allVolume + writeSevenTwo + allVolume)

        XCTAssertEqual(outcomes.map(\.packet.id), [0x3C, 0x39, 0x3C])
        XCTAssertEqual(outcomes.map(\.effects), [
            [.reply(V1.ReplyDecision(
                channel: .displayShort,
                bytes: [
                    0xAA, 0xD6, 0xEA, 0x3D, 0x05,
                    0x04, 0x00, 0x04, 0x00, 0xB4, 0xAB,
                ]
            ))],
            [.volumeChanged(V1.Session.ControlState(
                mode: .advancedLogic,
                mainVolume: 7,
                mutedVolume: 2,
                savedMainVolume: 4,
                savedMutedVolume: 0
            ))],
            [.reply(V1.ReplyDecision(
                channel: .displayShort,
                bytes: [
                    0xAA, 0xD6, 0xEA, 0x3D, 0x05,
                    0x07, 0x02, 0x04, 0x00, 0xB9, 0xAB,
                ]
            ))],
        ])
        XCTAssertEqual(session.controlState, V1.Session.ControlState(
            mode: .advancedLogic,
            mainVolume: 7,
            mutedVolume: 2,
            savedMainVolume: 4,
            savedMutedVolume: 0
        ))
        XCTAssertEqual(outcomes.flatMap(\.effects).filter { effect in
            if case .reply = effect { return true }
            return false
        }.count, 2)
    }

    /// Documented V4.1037+ host compatibility outside V1Simple's `aux0=00`
    /// behavior; no physical-device confirmation is claimed.
    func testVolumeSaveBitUpdatesCurrentAndSavedWithoutReply() {
        var config = V1.Session.Config()
        config.version = "4.1037"
        var session = V1.Session(config: config)
        let saveSevenTwo: [UInt8] = [
            0xAA, 0xDA, 0xE6, 0x39, 0x04, 0x07, 0x02, 0x04, 0xB4, 0xAB,
        ]
        let allVolume: [UInt8] = [
            0xAA, 0xDA, 0xE6, 0x3C, 0x01, 0xA7, 0xAB,
        ]

        let outcomes = session.receive(saveSevenTwo + allVolume)

        XCTAssertEqual(outcomes[0].effects, [.volumeChanged(V1.Session.ControlState(
            mode: .advancedLogic,
            mainVolume: 7,
            mutedVolume: 2,
            savedMainVolume: 7,
            savedMutedVolume: 2
        ))])
        XCTAssertEqual(outcomes[1].effects, [.reply(V1.ReplyDecision(
            channel: .displayShort,
            bytes: [
                0xAA, 0xD6, 0xEA, 0x3D, 0x05,
                0x07, 0x02, 0x07, 0x02, 0xBE, 0xAB,
            ]
        ))])
    }

    /// Pre-4.1037 host policy for the then-reserved `aux0=04` bit; physical
    /// handling is unknown.
    func testVolumeAux04Before41037UpdatesCurrentAndKeepsSavedWithoutReply() {
        var config = V1.Session.Config()
        config.version = "4.1036"
        var session = V1.Session(config: config)
        let reservedAuxSevenTwo: [UInt8] = [
            0xAA, 0xDA, 0xE6, 0x39, 0x04, 0x07, 0x02, 0x04, 0xB4, 0xAB,
        ]

        let outcomes = session.receive(reservedAuxSevenTwo)

        XCTAssertEqual(outcomes.count, 1)
        XCTAssertEqual(outcomes[0].effects, [.volumeChanged(V1.Session.ControlState(
            mode: .advancedLogic,
            mainVolume: 7,
            mutedVolume: 2,
            savedMainVolume: 4,
            savedMutedVolume: 0
        ))])
        XCTAssertEqual(session.controlState, V1.Session.ControlState(
            mode: .advancedLogic,
            mainVolume: 7,
            mutedVolume: 2,
            savedMainVolume: 4,
            savedMutedVolume: 0
        ))
        XCTAssertFalse(outcomes.flatMap(\.effects).contains { effect in
            if case .reply = effect { return true }
            return false
        })
    }

    /// Host compatibility outside V1Simple's `aux0=00` stable behavior.
    func testNonzeroVolumeAuxWithSaveBitClearKeepsSavedWithoutReply() {
        var config = V1.Session.Config()
        config.version = "4.1037"
        var session = V1.Session(config: config)
        let feedbackSevenTwo: [UInt8] = [
            0xAA, 0xDA, 0xE6, 0x39, 0x04, 0x07, 0x02, 0x01, 0xB1, 0xAB,
        ]

        let outcomes = session.receive(feedbackSevenTwo)

        XCTAssertEqual(outcomes.count, 1)
        XCTAssertEqual(outcomes[0].effects, [.volumeChanged(V1.Session.ControlState(
            mode: .advancedLogic,
            mainVolume: 7,
            mutedVolume: 2,
            savedMainVolume: 4,
            savedMutedVolume: 0
        ))])
        XCTAssertEqual(session.controlState, V1.Session.ControlState(
            mode: .advancedLogic,
            mainVolume: 7,
            mutedVolume: 2,
            savedMainVolume: 4,
            savedMutedVolume: 0
        ))
        XCTAssertFalse(outcomes.flatMap(\.effects).contains { effect in
            if case .reply = effect { return true }
            return false
        })
    }

    /// Public behavior IDs: `V1-CONTROL-MODE-001` and
    /// `V1-CONTROL-VOLUME-001`.
    func testInvalidControlValuesAndChecksumCannotMutateState() {
        var session = V1.Session()
        let invalidModeZero: [UInt8] = [
            0xAA, 0xDA, 0xE6, 0x36, 0x02, 0x00, 0xA2, 0xAB,
        ]
        let invalidModeFour: [UInt8] = [
            0xAA, 0xDA, 0xE6, 0x36, 0x02, 0x04, 0xA6, 0xAB,
        ]
        let invalidMain: [UInt8] = [
            0xAA, 0xDA, 0xE6, 0x39, 0x04, 0x0A, 0x02, 0x00, 0xB3, 0xAB,
        ]
        let invalidMuted: [UInt8] = [
            0xAA, 0xDA, 0xE6, 0x39, 0x04, 0x07, 0x0A, 0x00, 0xB8, 0xAB,
        ]
        let badChecksumModeOne: [UInt8] = [
            0xAA, 0xDA, 0xE6, 0x36, 0x02, 0x01, 0xA2, 0xAB,
        ]

        let outcomes = session.receive(
            invalidModeZero + invalidModeFour + invalidMain
                + invalidMuted + badChecksumModeOne
        )

        XCTAssertEqual(outcomes.map(\.effects), [
            [.rejected(.invalidModeValue(0x00))],
            [.rejected(.invalidModeValue(0x04))],
            [.rejected(.invalidVolume(main: 0x0A, muted: 0x02))],
            [.rejected(.invalidVolume(main: 0x07, muted: 0x0A))],
            [.rejected(.invalidChecksum)],
        ])
        XCTAssertEqual(session.controlState, V1.Session.ControlState(
            mode: .advancedLogic,
            mainVolume: 4,
            mutedVolume: 0,
            savedMainVolume: 4,
            savedMutedVolume: 0
        ))
    }

    func testStandardSweepHandshakeExercisesBusyAndOneBoundedRejection() {
        var session = V1.Session()
        let sweepSections: [UInt8] = [0xAA, 0xDA, 0xE6, 0x22, 0x01, 0x8D, 0xAB]
        let maxSweep: [UInt8] = [0xAA, 0xDA, 0xE6, 0x19, 0x01, 0x84, 0xAB]
        let allDefinitions: [UInt8] = [0xAA, 0xDA, 0xE6, 0x16, 0x01, 0x81, 0xAB]

        let sectionEffects = session.receive(sweepSections)[0].effects
        XCTAssertEqual(sectionEffects.count, 1)
        guard case .reply(let sectionReply) = sectionEffects[0] else {
            return XCTFail("sweep sections must reply")
        }
        XCTAssertEqual(sectionReply.bytes[3], V1.PacketID.respSweepSections.rawValue)

        let rejectedEffects = session.receive(maxSweep)[0].effects
        guard case .reply(let rejectedReply) = rejectedEffects[0] else {
            return XCTFail("first max request must be rejected")
        }
        XCTAssertEqual(rejectedReply.bytes[3], V1.PacketID.respRequestNotProcessed.rawValue)
        XCTAssertEqual(rejectedReply.bytes[5], V1.PacketID.reqMaxSweepIndex.rawValue)

        let definitionEffects = session.receive(allDefinitions)[0].effects
        let definitionIDs = definitionEffects.compactMap { effect -> UInt8? in
            guard case .reply(let reply) = effect else { return nil }
            return reply.bytes[3]
        }
        XCTAssertEqual(definitionIDs, [
            V1.PacketID.infV1Busy.rawValue,
            V1.PacketID.respSweepDefinition.rawValue,
            V1.PacketID.respSweepDefinition.rawValue,
        ])

        let retryEffects = session.receive(maxSweep)[0].effects
        guard case .reply(let maxReply) = retryEffects[0] else {
            return XCTFail("max retry must reply")
        }
        XCTAssertEqual(maxReply.bytes[3], V1.PacketID.respMaxSweepIndex.rawValue)
        XCTAssertEqual(maxReply.bytes[5], 0x01)

        session.resetTransport()
        let nextSessionEffects = session.receive(maxSweep)[0].effects
        guard case .reply(let nextRejectedReply) = nextSessionEffects[0] else {
            return XCTFail("new transport must re-arm the exercise")
        }
        XCTAssertEqual(nextRejectedReply.bytes[3], V1.PacketID.respRequestNotProcessed.rawValue)
    }

    func testCustomFrequencyWriteCommitsAndReadsBackExactDefinitions() {
        var session = V1.Session()
        let k = V1.Session.SweepDefinition(
            index: 0, lowerMHz: 24_100, upperMHz: 24_200)
        let ka = V1.Session.SweepDefinition(
            index: 1, lowerMHz: 34_600, upperMHz: 34_800)

        let staged = session.receive(request(
            .reqWriteSweepDefinition,
            payload: sweepWritePayload(k, commit: false)
        ))
        XCTAssertEqual(staged.count, 1)
        XCTAssertEqual(staged[0].effects, [])
        XCTAssertEqual(session.storedSweepDefinitions, defaultSweepDefinitions)

        let committed = session.receive(request(
            .reqWriteSweepDefinition,
            payload: sweepWritePayload(ka, commit: true)
        ))
        XCTAssertEqual(committed[0].effects, [.reply(V1.ReplyDecision(
            channel: .displayShort,
            bytes: [
                0xAA, 0xD6, 0xEA, 0x21, 0x02, 0x00, 0x8D, 0xAB,
            ]
        ))])
        XCTAssertEqual(session.storedSweepDefinitions, [k, ka])

        let readback = session.receive(request(.reqAllSweepDefinitions))[0].effects
        let expectedDefinitions: [V1.Session.Effect] = [
            .reply(V1.ReplyDecision(
                channel: .displayShort,
                bytes: [
                    0xAA, 0xD6, 0xEA, 0x17, 0x06,
                    0x80, 0x5E, 0x88, 0x5E, 0x24, 0x6F, 0xAB,
                ])),
            .reply(V1.ReplyDecision(
                channel: .displayShort,
                bytes: [
                    0xAA, 0xD6, 0xEA, 0x17, 0x06,
                    0x81, 0x87, 0xF0, 0x87, 0x28, 0x2E, 0xAB,
                ])),
        ]
        XCTAssertEqual(readback, [
            .reply(V1.ReplyDecision(
                channel: .displayShort,
                bytes: [
                    0xAA, 0xD8, 0xEA, 0x66, 0x02, 0x16, 0xEA, 0xAB,
                ])),
        ] + expectedDefinitions)
        XCTAssertEqual(
            session.receive(request(.reqAllSweepDefinitions))[0].effects,
            expectedDefinitions
        )
    }

    func testCustomFrequenciesFilterKAndKaButPreserveAuthoredPriority() {
        var config = V1.Session.Config()
        // X/K/Ka/Laser/Ku enabled; Custom Frequencies enabled independently.
        config.userBytes = [0x7F, 0xF7, 0xFF, 0xFF, 0xFF, 0xFF]
        let session = V1.Session(config: config)

        let kaPriorityOutsideDefinition = ReplayAlert(
            band: .ka, frequencyMHz: 34_700, strength: 6,
            direction: .front, isPriority: true)
        let photoInsideDefinition = ReplayAlert(
            band: .k, frequencyMHz: 24_125, strength: 5,
            direction: .side, isPriority: false, photoType: 3)
        let projected = session.projectedSample(TimedSample(
            offset: 1, phase: "custom", muted: false,
            alerts: [kaPriorityOutsideDefinition, photoInsideDefinition],
            scenarioArrowBlink: true, sourceIndex: 1))

        XCTAssertEqual(projected.alerts.count, 1)
        XCTAssertEqual(projected.priorityAlert?.band.mask, V1.Band.k.mask)
        XCTAssertEqual(projected.priorityAlert?.frequencyMHz, 24_125)
        XCTAssertEqual(projected.priorityAlert?.photoType, 3)
        XCTAssertTrue(projected.scenarioArrowBlink)

        let kaInsideDefinition = ReplayAlert(
            band: .ka, frequencyMHz: 34_150, strength: 4,
            direction: .rear, isPriority: true)
        let preserved = session.projectedSample(TimedSample(
            offset: 2, phase: "custom", muted: false,
            alerts: [kaInsideDefinition, photoInsideDefinition.withPriority(false)],
            sourceIndex: 2))
        XCTAssertEqual(preserved.priorityAlert?.band.mask, V1.Band.ka.mask)
        XCTAssertEqual(preserved.alerts.count, 2)
    }

    func testFilteringAllAlertsProducesAValidNonBlinkingEmptySample() {
        var config = V1.Session.Config()
        config.userBytes = [0x7F, 0xF7, 0xFF, 0xFF, 0xFF, 0xFF]
        let session = V1.Session(config: config)
        let projected = session.projectedSample(TimedSample(
            offset: 1, phase: "filtered", muted: false,
            alerts: [ReplayAlert(
                band: .ka, frequencyMHz: 34_700, strength: 6,
                direction: .front, isPriority: true)],
            scenarioArrowBlink: true, sourceIndex: 1))

        XCTAssertTrue(projected.alerts.isEmpty)
        XCTAssertFalse(projected.scenarioArrowBlink)
    }

    func testProjectionPreservesJunkIdentityAndRowlessCounterOverride() {
        var config = V1.Session.Config()
        config.userBytes = [0x7F, 0xF7, 0xFF, 0xFF, 0xFF, 0xFF]
        let session = V1.Session(config: config)
        let outside = ReplayAlert(
            band: .ka, frequencyMHz: 34_700, strength: 6,
            direction: .front, isPriority: true)
        let junk = ReplayAlert(
            band: .k, frequencyMHz: 24_125, strength: 2,
            direction: .front, isPriority: false, isJunk: true)
        let projected = session.projectedSample(TimedSample(
            offset: 1, phase: "junk", muted: false,
            alerts: [outside, junk], bogeyCounterOverride: .junkBlink,
            sourceIndex: 1))

        XCTAssertEqual(projected.alerts.count, 1)
        XCTAssertTrue(projected.priorityAlert?.isPriority == true)
        XCTAssertTrue(projected.priorityAlert?.isJunk == true)
        XCTAssertEqual(projected.bogeyCounterOverride, .junkBlink)
    }

    func testBandSwitchesFilterDirectBitsAndInvertedKuBit() {
        var config = V1.Session.Config()
        // Only K and Ka are enabled. Custom Frequencies remains off.
        config.userBytes = [0x86, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF]
        let session = V1.Session(config: config)
        let alerts = [
            ReplayAlert(band: .x, frequencyMHz: 10_525, strength: 3,
                        direction: .front, isPriority: true),
            ReplayAlert(band: .k, frequencyMHz: 24_125, strength: 4,
                        direction: .side, isPriority: false),
            ReplayAlert(band: .ka, frequencyMHz: 34_700, strength: 5,
                        direction: .rear, isPriority: false),
        ]
        let projected = session.projectedSample(TimedSample(
            offset: 1, phase: "bands", muted: false,
            alerts: alerts, sourceIndex: 1))

        XCTAssertEqual(projected.alerts.map(\.band.mask), [V1.Band.k.mask, V1.Band.ka.mask])
        XCTAssertEqual(projected.priorityAlert?.band.mask, V1.Band.k.mask)

        let disabledLaser = session.projectedSample(TimedSample(
            offset: 2, phase: "laser", muted: false,
            alerts: [ReplayAlert(
                band: .laser, frequencyMHz: 0, strength: 8,
                direction: .front, isPriority: true)],
            sourceIndex: 2))
        XCTAssertTrue(disabledLaser.alerts.isEmpty)

        let disabledKu = session.projectedSample(TimedSample(
            offset: 3, phase: "ku", muted: false,
            alerts: [ReplayAlert(
                band: .ku, frequencyMHz: 13_450, strength: 6,
                direction: .front, isPriority: true)],
            sourceIndex: 3))
        XCTAssertTrue(disabledKu.alerts.isEmpty)
    }

    func testRejectedPartialSweepCommitLeavesActiveDefinitionsUnchanged() {
        var session = V1.Session()
        let onlyK = V1.Session.SweepDefinition(
            index: 0, lowerMHz: 24_100, upperMHz: 24_200)
        let outcome = session.receive(request(
            .reqWriteSweepDefinition,
            payload: sweepWritePayload(onlyK, commit: true)
        ))

        guard case .reply(let reply) = outcome[0].effects.first else {
            return XCTFail("committed write must report a result")
        }
        XCTAssertEqual(reply.bytes[3], V1.PacketID.respSweepWriteResult.rawValue)
        XCTAssertNotEqual(reply.bytes[5], 0)
        XCTAssertEqual(session.storedSweepDefinitions, defaultSweepDefinitions)

        let onlyKa = V1.Session.SweepDefinition(
            index: 1, lowerMHz: 34_600, upperMHz: 34_800)
        let secondFailure = session.receive(request(
            .reqWriteSweepDefinition,
            payload: sweepWritePayload(onlyKa, commit: true)
        ))
        guard case .reply(let secondReply) = secondFailure[0].effects.first else {
            return XCTFail("second committed write must report a result")
        }
        XCTAssertNotEqual(secondReply.bytes[5], 0,
                          "failed commit must clear the earlier staged K definition")
        XCTAssertEqual(session.storedSweepDefinitions, defaultSweepDefinitions)
    }

    func testLastUnsubscribeEndsAnUncommittedSweepTransaction() throws {
        var session = V1.Session()
        let first = try XCTUnwrap(
            UUID(uuidString: "00000000-0000-0000-0000-000000000011"))
        let second = try XCTUnwrap(
            UUID(uuidString: "00000000-0000-0000-0000-000000000012"))
        let k = V1.Session.SweepDefinition(
            index: 0, lowerMHz: 24_100, upperMHz: 24_200)
        let ka = V1.Session.SweepDefinition(
            index: 1, lowerMHz: 34_600, upperMHz: 34_800)

        session.subscribe(central: first, channel: .displayShort)
        XCTAssertEqual(session.receive(request(
            .reqWriteSweepDefinition,
            payload: sweepWritePayload(k, commit: false)
        ))[0].effects, [])
        XCTAssertEqual(
            session.unsubscribe(central: first, channel: .displayShort), 0)

        session.subscribe(central: second, channel: .displayShort)
        let outcome = session.receive(request(
            .reqWriteSweepDefinition,
            payload: sweepWritePayload(ka, commit: true)
        ))
        guard case .reply(let reply) = outcome[0].effects.first else {
            return XCTFail("commit after reconnect must report a result")
        }
        XCTAssertNotEqual(reply.bytes[5], 0,
                          "a new central must not inherit the prior staged K definition")
        XCTAssertEqual(session.storedSweepDefinitions, defaultSweepDefinitions)
    }

    func testPermissiveBenchStartStateDoesNotChangeAuthoredScenario() throws {
        let authored = BenchScenario.make()
        var config = V1.Session.Config()
        config.userBytes = [0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF]
        let session = V1.Session(config: config)
        let projected = Encounter(
            origin: .syntheticBench,
            samples: authored.samples.map(session.projectedSample)
        )

        XCTAssertEqual(
            try projected.resolvedScenarioEvidenceData(),
            try authored.resolvedScenarioEvidenceData()
        )
    }

    private var defaultSweepDefinitions: [V1.Session.SweepDefinition] {
        return [
            V1.Session.SweepDefinition(
                index: 0, lowerMHz: 24_050, upperMHz: 24_150),
            V1.Session.SweepDefinition(
                index: 1, lowerMHz: 34_100, upperMHz: 34_200),
        ]
    }

    private func request(
        _ id: V1.PacketID,
        payload: [UInt8] = []
    ) -> [UInt8] {
        var packet: [UInt8] = [
            0xAA, 0xDA, 0xE6, id.rawValue, UInt8(payload.count + 1),
        ]
        packet.append(contentsOf: payload)
        packet.append(packet.reduce(0, &+))
        packet.append(0xAB)
        return packet
    }

    private func sweepWritePayload(
        _ definition: V1.Session.SweepDefinition,
        commit: Bool
    ) -> [UInt8] {
        return [
            0x80 | definition.index | (commit ? 0x40 : 0x00),
            UInt8((definition.upperMHz >> 8) & 0xFF),
            UInt8(definition.upperMHz & 0xFF),
            UInt8((definition.lowerMHz >> 8) & 0xFF),
            UInt8(definition.lowerMHz & 0xFF),
        ]
    }
}

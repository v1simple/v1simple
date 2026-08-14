import Foundation
import XCTest
@testable import v1replay

final class V1HandshakeNotificationHoldTests: XCTestCase {
    func testZeroDelayPreservesImmediateFlushForOwnedStart() {
        var state = HandshakeNotificationHoldState(delayMilliseconds: 0)
        state.beginEpoch()

        XCTAssertNil(state.acceptedStart(belongsToActiveEpoch: true))
        XCTAssertFalse(state.blocksFlush)
        XCTAssertNil(state.acceptedStart(belongsToActiveEpoch: true))
    }

    func testFirstOwnedStartSchedulesOnceAndDuplicateDoesNotRestartHold() throws {
        var state = HandshakeNotificationHoldState(delayMilliseconds: 1_250)
        state.beginEpoch()

        XCTAssertNil(state.acceptedStart(belongsToActiveEpoch: false))
        XCTAssertFalse(state.blocksFlush)

        let scheduled = try XCTUnwrap(
            state.acceptedStart(belongsToActiveEpoch: true)
        )
        XCTAssertEqual(scheduled.delayMilliseconds, 1_250)
        XCTAssertTrue(state.blocksFlush)

        XCTAssertNil(state.acceptedStart(belongsToActiveEpoch: true))
        XCTAssertTrue(state.blocksFlush)
        XCTAssertTrue(state.release(scheduled))
        XCTAssertFalse(state.blocksFlush)

        XCTAssertNil(state.acceptedStart(belongsToActiveEpoch: true))
        XCTAssertFalse(state.blocksFlush)
    }

    func testEpochEndAndReplacementInvalidateStaleRelease() throws {
        var state = HandshakeNotificationHoldState(delayMilliseconds: 1_250)
        state.beginEpoch()
        let stale = try XCTUnwrap(
            state.acceptedStart(belongsToActiveEpoch: true)
        )

        state.endEpoch()
        XCTAssertFalse(state.blocksFlush)
        XCTAssertFalse(state.release(stale))

        state.beginEpoch()
        let current = try XCTUnwrap(
            state.acceptedStart(belongsToActiveEpoch: true)
        )
        XCTAssertNotEqual(current.epoch, stale.epoch)
        XCTAssertTrue(state.blocksFlush)
        XCTAssertFalse(state.release(stale))
        XCTAssertTrue(state.blocksFlush)
        XCTAssertTrue(state.release(current))
        XCTAssertFalse(state.blocksFlush)
    }

    func testCLIValidationIsStrictAndConfinedToHandshakeBench() throws {
        XCTAssertEqual(
            try parseHandshakeNotificationHoldMilliseconds(
                nil, bench: false, handshakeOnly: false
            ),
            0
        )
        XCTAssertEqual(
            try parseHandshakeNotificationHoldMilliseconds(
                "0", bench: true, handshakeOnly: true
            ),
            0
        )
        XCTAssertEqual(
            try parseHandshakeNotificationHoldMilliseconds(
                "1999", bench: true, handshakeOnly: true
            ),
            1_999
        )

        XCTAssertThrowsError(
            try parseHandshakeNotificationHoldMilliseconds(
                "1250", bench: false, handshakeOnly: true
            )
        )
        XCTAssertThrowsError(
            try parseHandshakeNotificationHoldMilliseconds(
                "1250", bench: true, handshakeOnly: false
            )
        )
        for invalid in ["-1", "2000", "1.5", "true"] {
            XCTAssertThrowsError(
                try parseHandshakeNotificationHoldMilliseconds(
                    invalid, bench: true, handshakeOnly: true
                ),
                invalid
            )
        }
    }

    func testPeripheralWiresHoldBeforeEffectsAndCancelsAtEpochBoundary() throws {
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let source = try String(
            contentsOf: packageRoot
                .appendingPathComponent("Sources/v1replay/Peripheral.swift"),
            encoding: .utf8
        )

        let acceptedStart = try XCTUnwrap(
            source.range(of: "let scheduled = handshakeNotificationHold.acceptedStart(")
        )
        let effects = try XCTUnwrap(
            source.range(of: "for effect in outcome.effects", range: acceptedStart.lowerBound..<source.endIndex)
        )
        XCTAssertLessThan(acceptedStart.lowerBound, effects.lowerBound)
        XCTAssertTrue(
            source.contains(
                "guard !isStopping, !handshakeNotificationHold.blocksFlush else { return }"
            )
        )
        XCTAssertTrue(
            source.contains(
                "handshakeSubscriberID = nil\n        handshakeNotificationHold.endEpoch()"
            )
        )
        XCTAssertTrue(
            source.contains(
                "handshakeLedger?.beginEpoch()\n            handshakeNotificationHold.beginEpoch()"
            )
        )
    }
}

import Foundation
import XCTest
@testable import v1replay

final class V1HandshakeNotificationHoldTests: XCTestCase {
    func testZeroDelayPreservesImmediateFlushForOwnedStart() {
        var state = HandshakeNotificationHoldState(delayMilliseconds: 0)
        state.beginEpoch()

        XCTAssertEqual(state.acceptedStart(belongsToActiveEpoch: true), .none)
        XCTAssertFalse(state.blocksFlush)
        XCTAssertEqual(state.acceptedStart(belongsToActiveEpoch: true), .none)
    }

    func testSecondOwnedStartReleasesWithoutRestartingDeadline() throws {
        var state = HandshakeNotificationHoldState(delayMilliseconds: 1_250)
        state.beginEpoch()

        XCTAssertEqual(state.acceptedStart(belongsToActiveEpoch: false), .none)
        XCTAssertFalse(state.blocksFlush)

        guard case .schedule(let scheduled) = state.acceptedStart(
            belongsToActiveEpoch: true
        ) else {
            return XCTFail("first owned START did not schedule its safety deadline")
        }
        XCTAssertEqual(scheduled.delayMilliseconds, 1_250)
        XCTAssertTrue(state.blocksFlush)

        XCTAssertEqual(state.acceptedStart(belongsToActiveEpoch: false), .none)
        XCTAssertTrue(state.blocksFlush, "foreign START released the owned hold")
        XCTAssertEqual(
            state.acceptedStart(belongsToActiveEpoch: true),
            .releaseHeldNotifications
        )
        XCTAssertFalse(state.blocksFlush)
        XCTAssertFalse(state.release(scheduled), "stale deadline released twice")

        XCTAssertEqual(state.acceptedStart(belongsToActiveEpoch: true), .none)
        XCTAssertFalse(state.blocksFlush)
    }

    func testDeadlineRemainsBoundedFallbackWhenSecondStartDoesNotArrive() throws {
        var state = HandshakeNotificationHoldState(delayMilliseconds: 1_999)
        state.beginEpoch()
        guard case .schedule(let scheduled) = state.acceptedStart(
            belongsToActiveEpoch: true
        ) else {
            return XCTFail("first owned START did not schedule its safety deadline")
        }

        XCTAssertEqual(scheduled.delayMilliseconds, 1_999)
        XCTAssertTrue(state.blocksFlush)
        XCTAssertTrue(state.release(scheduled))
        XCTAssertFalse(state.blocksFlush)
        XCTAssertEqual(state.acceptedStart(belongsToActiveEpoch: true), .none)
    }

    func testSecondStartReleaseKeepsClearPendingAcrossBackpressure() throws {
        var hold = HandshakeNotificationHoldState(delayMilliseconds: 1_250)
        var clear = HandshakeClearDeliveryState()
        hold.beginEpoch()
        guard case .schedule(let scheduled) = hold.acceptedStart(
            belongsToActiveEpoch: true
        ) else {
            return XCTFail("first owned START did not schedule its safety deadline")
        }
        XCTAssertEqual(clear.ensure(), .enqueue)

        XCTAssertEqual(
            hold.acceptedStart(belongsToActiveEpoch: true),
            .releaseHeldNotifications
        )
        XCTAssertFalse(hold.blocksFlush)

        // A false CoreBluetooth update leaves the queued clear unconfirmed;
        // the readiness callback retries that same row without rearming hold.
        XCTAssertEqual(clear.ensure(), .retryPending)
        XCTAssertTrue(clear.isPending)
        XCTAssertFalse(clear.isDeliveryConfirmed)
        XCTAssertTrue(clear.confirmDelivery())
        XCTAssertFalse(clear.isPending)
        XCTAssertTrue(clear.isDeliveryConfirmed)
        XCTAssertFalse(hold.release(scheduled))
        XCTAssertEqual(hold.acceptedStart(belongsToActiveEpoch: true), .none)
    }

    func testEpochEndAndReplacementInvalidateStaleRelease() throws {
        var state = HandshakeNotificationHoldState(delayMilliseconds: 1_250)
        state.beginEpoch()
        guard case .schedule(let stale) = state.acceptedStart(
            belongsToActiveEpoch: true
        ) else {
            return XCTFail("first epoch did not schedule its safety deadline")
        }

        state.endEpoch()
        XCTAssertFalse(state.blocksFlush)
        XCTAssertFalse(state.release(stale))
        XCTAssertEqual(state.acceptedStart(belongsToActiveEpoch: true), .none)

        state.beginEpoch()
        guard case .schedule(let current) = state.acceptedStart(
            belongsToActiveEpoch: true
        ) else {
            return XCTFail("replacement epoch did not schedule its safety deadline")
        }
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
            source.range(of: "switch handshakeNotificationHold.acceptedStart(")
        )
        let effects = try XCTUnwrap(
            source.range(of: "for effect in outcome.effects", range: acceptedStart.lowerBound..<source.endIndex)
        )
        XCTAssertLessThan(acceptedStart.lowerBound, effects.lowerBound)
        let releaseCase = try XCTUnwrap(
            source.range(
                of: "case .releaseHeldNotifications:",
                range: acceptedStart.lowerBound..<effects.lowerBound
            )
        )
        XCTAssertNotNil(
            source.range(
                of: "self.flush()",
                range: releaseCase.lowerBound..<effects.lowerBound
            ),
            "second START release does not flush before command effects"
        )
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
                "handshakeSubscriberID = central.identifier\n            handshakeNotificationHold.beginEpoch()"
            )
        )
    }
}

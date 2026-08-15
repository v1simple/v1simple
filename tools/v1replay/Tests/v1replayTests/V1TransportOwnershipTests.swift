import Foundation
import XCTest
@testable import v1replay

final class V1TransportOwnershipTests: XCTestCase {
    private func packageSource(_ relativePath: String) throws -> String {
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        return try String(
            contentsOf: packageRoot.appendingPathComponent(relativePath),
            encoding: .utf8
        )
    }

    func testTransportResetClearsSubscriptionsRequestsAndBufferedFrames() throws {
        var session = V1.Session()
        let first = try XCTUnwrap(
            UUID(uuidString: "00000000-0000-0000-0000-000000000001")
        )
        let second = try XCTUnwrap(
            UUID(uuidString: "00000000-0000-0000-0000-000000000002")
        )
        let volume: [UInt8] = [
            0xAA, 0xDA, 0xE6, 0x39, 0x04, 0x07, 0x02, 0x00, 0xB0, 0xAB,
        ]
        let startAlert: [UInt8] = [
            0xAA, 0xDA, 0xE6, 0x41, 0x01, 0xAC, 0xAB,
        ]
        let version: [UInt8] = [
            0xAA, 0xDA, 0xE6, 0x01, 0x01, 0x6C, 0xAB,
        ]
        let allVolume: [UInt8] = [
            0xAA, 0xDA, 0xE6, 0x3C, 0x01, 0xA7, 0xAB,
        ]

        session.subscribe(central: first, channel: .displayShort)
        session.subscribe(central: first, channel: .displayLong)
        session.subscribe(central: second, channel: .compatibilityNotify)
        _ = session.receive(volume + startAlert)
        session.append(version + Array(allVolume.prefix(4)))

        XCTAssertEqual(session.subscriberCount, 2)
        XCTAssertEqual(session.shortSubscriberCount, 1)
        XCTAssertTrue(session.alertDataRequested)
        XCTAssertEqual(session.bufferedByteCount, 4)

        session.resetTransport()

        XCTAssertEqual(session.subscriberCount, 0)
        XCTAssertEqual(session.shortSubscriberCount, 0)
        XCTAssertFalse(session.sessionTransportActive)
        XCTAssertFalse(session.readiness.displaySubscribed)
        XCTAssertFalse(session.readiness.longTrafficSubscribed)
        XCTAssertFalse(session.alertDataRequested)
        XCTAssertEqual(session.bufferedByteCount, 0)
        XCTAssertNil(session.nextOutcome(), "a frame queued by the prior transport survived reset")
        XCTAssertEqual(session.controlState, V1.Session.ControlState(
            mode: .advancedLogic,
            mainVolume: 7,
            mutedVolume: 2,
            savedMainVolume: 4,
            savedMutedVolume: 0
        ))
    }

    func testSessionTransportEmitsFalseWhenSecondShortSubscriberIsAmbiguous() throws {
        var session = V1.Session()
        let first = try XCTUnwrap(
            UUID(uuidString: "00000000-0000-0000-0000-000000000001")
        )
        let second = try XCTUnwrap(
            UUID(uuidString: "00000000-0000-0000-0000-000000000002")
        )
        var emitted: [Bool] = []
        let events = BooleanMachineEventEmitter { emitted.append($0) }

        events.emit(session.sessionTransportActive)
        session.subscribe(central: first, channel: .displayLong)
        events.emit(session.sessionTransportActive)
        session.subscribe(central: first, channel: .displayShort)
        events.emit(session.sessionTransportActive)
        session.subscribe(central: second, channel: .displayLong)
        events.emit(session.sessionTransportActive)
        session.subscribe(central: second, channel: .displayShort)
        events.emit(session.sessionTransportActive)

        XCTAssertEqual(session.shortSubscriberCount, 2)
        XCTAssertEqual(emitted, [false, true, false])

        _ = session.unsubscribe(central: second, channel: .displayShort)
        events.emit(session.sessionTransportActive)
        session.resetTransport()
        events.emit(session.sessionTransportActive)

        XCTAssertEqual(emitted, [false, true, false, true, false])
    }

    func testStoppingSnapshotRetainsPreResetTransportState() throws {
        var session = V1.Session()
        let owner = try XCTUnwrap(
            UUID(uuidString: "00000000-0000-0000-0000-000000000001")
        )
        session.subscribe(central: owner, channel: .displayShort)

        let stopping = StoppingMachineEvent(
            sessionTransportActive: session.sessionTransportActive
        )
        session.resetTransport()

        XCTAssertTrue(stopping.sessionTransportActive)
        XCTAssertEqual(
            stopping.line,
            "V1REPLAY_EVENT {\"state\":\"stopping\",\"sessionTransportActive\":true}"
        )
        XCTAssertFalse(session.sessionTransportActive)
    }

    func testPowerAndStopPathsResetTransportBeforeTheirFinalStateCallbacks() throws {
        let peripheralSource = try packageSource("Sources/v1replay/Peripheral.swift")
        let stateStart = try XCTUnwrap(
            peripheralSource.range(of: "    func peripheralManagerDidUpdateState(")
        )
        let stateEnd = try XCTUnwrap(
            peripheralSource.range(
                of: "\n    func peripheralManager(",
                range: stateStart.upperBound..<peripheralSource.endIndex
            )
        )
        let stateBody = String(peripheralSource[stateStart.lowerBound..<stateEnd.lowerBound])
        XCTAssertTrue(stateBody.contains("guard peripheral.state == .poweredOn else"))
        let powerReset = try XCTUnwrap(stateBody.range(of: "resetSessionTransport()"))
        let powerCallback = try XCTUnwrap(
            stateBody.range(of: "onStateChange?()", range: powerReset.upperBound..<stateBody.endIndex)
        )
        XCTAssertLessThan(powerReset.lowerBound, powerCallback.lowerBound)

        let stopStart = try XCTUnwrap(
            peripheralSource.range(of: "    func stop(onStopping: ((Bool) -> Void)? = nil) {")
        )
        let stopEnd = try XCTUnwrap(
            peripheralSource.range(
                of: "\n    // MARK: - Command handling",
                range: stopStart.upperBound..<peripheralSource.endIndex
            )
        )
        let stopBody = String(peripheralSource[stopStart.lowerBound..<stopEnd.lowerBound])
        let stoppingSnapshot = try XCTUnwrap(
            stopBody.range(of: "onStopping?(withState { $0.session.sessionTransportActive })")
        )
        let stopReset = try XCTUnwrap(stopBody.range(of: "resetSessionTransport()"))
        let stopCallback = try XCTUnwrap(
            stopBody.range(of: "onStateChange?()", range: stopReset.upperBound..<stopBody.endIndex)
        )
        XCTAssertLessThan(stoppingSnapshot.lowerBound, stopReset.lowerBound)
        XCTAssertLessThan(stopReset.lowerBound, stopCallback.lowerBound)

        let mainSource = try packageSource("Sources/v1replay/main.swift")
        XCTAssertTrue(
            mainSource.contains("sessionTransportEvents?.emit(peripheral.sessionTransportActive)")
        )
        XCTAssertFalse(
            mainSource.contains("sessionTransportEvents?.emit(peripheral.displaySubscribed)")
        )
    }
}

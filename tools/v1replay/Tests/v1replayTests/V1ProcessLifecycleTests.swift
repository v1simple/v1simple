import Darwin
import Foundation
import XCTest
@testable import v1replay

final class V1ProcessLifecycleTests: XCTestCase {
    func testBooleanTransitionStateEmitsInitialAndChangedValuesOnly() {
        var state = BooleanTransitionState()

        XCTAssertTrue(state.shouldEmit(false))
        XCTAssertFalse(state.shouldEmit(false))
        XCTAssertTrue(state.shouldEmit(true))
        XCTAssertFalse(state.shouldEmit(true))
        XCTAssertTrue(state.shouldEmit(false))
        XCTAssertEqual(state.lastValue, false)
    }

    func testBooleanMachineEventEmitterSuppressesDuplicateStates() {
        var values: [Bool] = []
        let emitter = BooleanMachineEventEmitter { values.append($0) }

        emitter.emit(false)
        emitter.emit(false)
        emitter.emit(true)
        emitter.emit(true)
        emitter.emit(false)

        XCTAssertEqual(values, [false, true, false])
    }

    func testBooleanMachineEventEmitterSerializesCallbackPublication() {
        let firstEntered = DispatchSemaphore(value: 0)
        let releaseFirst = DispatchSemaphore(value: 0)
        let secondAttempting = DispatchSemaphore(value: 0)
        let secondEntered = DispatchSemaphore(value: 0)
        let callsFinished = DispatchGroup()
        let emitter = BooleanMachineEventEmitter { value in
            if value {
                secondEntered.signal()
            } else {
                firstEntered.signal()
                _ = releaseFirst.wait(timeout: .now() + 2)
            }
        }

        callsFinished.enter()
        DispatchQueue.global().async {
            emitter.emit(false)
            callsFinished.leave()
        }
        XCTAssertEqual(firstEntered.wait(timeout: .now() + 1), .success)

        callsFinished.enter()
        DispatchQueue.global().async {
            secondAttempting.signal()
            emitter.emit(true)
            callsFinished.leave()
        }
        XCTAssertEqual(secondAttempting.wait(timeout: .now() + 1), .success)
        XCTAssertEqual(secondEntered.wait(timeout: .now() + 0.1), .timedOut)

        releaseFirst.signal()
        XCTAssertEqual(secondEntered.wait(timeout: .now() + 1), .success)
        XCTAssertEqual(callsFinished.wait(timeout: .now() + 1), .success)
    }

    func testStoppingMachineEventCarriesExactTransportSnapshot() {
        XCTAssertEqual(
            StoppingMachineEvent(sessionTransportActive: true).line,
            "V1REPLAY_EVENT {\"state\":\"stopping\",\"sessionTransportActive\":true}"
        )
        XCTAssertEqual(
            StoppingMachineEvent(sessionTransportActive: false).line,
            "V1REPLAY_EVENT {\"state\":\"stopping\",\"sessionTransportActive\":false}"
        )
    }

    func testOwnerPIDIsStrictlyValidatedAndLimitedToManagedModes() throws {
        XCTAssertNil(
            try parseProcessOwnerGuard(nil, command: "play", directParentPID: 123)
        )
        XCTAssertEqual(
            try parseProcessOwnerGuard("123", command: "bench", directParentPID: 123),
            ProcessOwnerGuard(ownerPID: 123)
        )
        XCTAssertEqual(
            try parseProcessOwnerGuard("123", command: "idle", directParentPID: 123),
            ProcessOwnerGuard(ownerPID: 123)
        )

        for invalid in ["0", "-1", "1.5", "true", "2147483648"] {
            XCTAssertThrowsError(
                try parseProcessOwnerGuard(invalid, command: "bench", directParentPID: 123),
                invalid
            )
        }
        XCTAssertThrowsError(
            try parseProcessOwnerGuard("124", command: "bench", directParentPID: 123)
        )
        for command in ["play", "demo", "proxy", "export", "help"] {
            XCTAssertThrowsError(
                try parseProcessOwnerGuard("123", command: command, directParentPID: 123),
                command
            )
        }
    }

    func testOwnerGuardPollIntervalMeetsOneHundredMillisecondLimit() {
        let guardState = ProcessOwnerGuard(ownerPID: 123)

        XCTAssertTrue(guardState.isDirectParent(123))
        XCTAssertFalse(guardState.isDirectParent(1))
        XCTAssertLessThanOrEqual(ProcessOwnerGuard.pollIntervalSeconds, 0.1)
    }

    func testSignalAndTeardownWiringRemainGracefulAndOrdered() throws {
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let consoleSource = try String(
            contentsOf: packageRoot.appendingPathComponent("Sources/v1replay/Console.swift"),
            encoding: .utf8
        )
        let lifecycleSource = try String(
            contentsOf: packageRoot.appendingPathComponent("Sources/v1replay/ProcessLifecycle.swift"),
            encoding: .utf8
        )
        let mainSource = try String(
            contentsOf: packageRoot.appendingPathComponent("Sources/v1replay/main.swift"),
            encoding: .utf8
        )

        XCTAssertFalse(consoleSource.contains("_exit("))
        XCTAssertTrue(lifecycleSource.contains("DispatchSource.makeSignalSource"))
        for signalName in ["SIGINT", "SIGTERM", "SIGHUP"] {
            XCTAssertTrue(lifecycleSource.contains(signalName), signalName)
        }
        XCTAssertTrue(mainSource.contains("let signalMonitor = GracefulSignalMonitor"))
        XCTAssertTrue(mainSource.contains("sessionTransportEvents?.emit(peripheral.sessionTransportActive)"))

        let playerStop = try XCTUnwrap(mainSource.range(of: "    player.stop()\n"))
        let peripheralStop = try XCTUnwrap(
            mainSource.range(
                of: "    peripheral.stop { sessionTransportActive in\n",
                range: playerStop.upperBound..<mainSource.endIndex
            )
        )
        let stoppingEvent = try XCTUnwrap(
            mainSource.range(
                of: "StoppingMachineEvent(sessionTransportActive: sessionTransportActive).line",
                range: peripheralStop.upperBound..<mainSource.endIndex
            )
        )
        let stoppedEvent = try XCTUnwrap(
            mainSource.range(
                of: "V1REPLAY_EVENT {\\\"state\\\":\\\"stopped\\\"}",
                range: stoppingEvent.upperBound..<mainSource.endIndex
            )
        )
        XCTAssertLessThan(playerStop.lowerBound, peripheralStop.lowerBound)
        XCTAssertLessThan(peripheralStop.lowerBound, stoppingEvent.lowerBound)
        XCTAssertLessThan(stoppingEvent.lowerBound, stoppedEvent.lowerBound)
    }
}

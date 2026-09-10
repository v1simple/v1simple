import Foundation
import XCTest

final class V1PlayerTimingTests: XCTestCase {
    private enum ProbeError: Error { case failed(String) }
    private struct Recording: Decodable {
        struct Packet: Decodable {
            let bytes: [UInt8]
            let phase: String
            let sequence: Int
            let ordinal: Int
        }
        let offsets: [Double]
        let times: [Double]
        let intendedOffsets: [Double]
        let starts: [Double]
        let packets: [Packet]
    }

    // Compile the real engine once, replacing only its hardware adapter. Keeping
    // this outside the product avoids a BLE/scheduler abstraction solely for tests.
    private static let probe: Result<URL, Error> = Result {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        let source = directory.appendingPathComponent("TimingProbe.swift")
        try probeSource.write(to: source, atomically: true, encoding: .utf8)
        let package = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
            .deletingLastPathComponent().deletingLastPathComponent()
        let sources = ["V1Protocol", "Session", "Encounter", "PlaybackPacketPlan", "Player",
                       "ReplayEvidence", "ReplayStimulusEvent"].map {
            package.appendingPathComponent("Sources/v1replay/\($0).swift").path
        }
        do {
            _ = try run(URL(fileURLWithPath: "/usr/bin/xcrun"),
                        ["swiftc", "-module-cache-path", directory.appendingPathComponent("cache").path]
                        + sources + [source.path, "-o", directory.appendingPathComponent("probe").path],
                        directory: directory, timeout: 60)
            return directory
        } catch {
            try? FileManager.default.removeItem(at: directory)
            throw error
        }
    }

    override class func tearDown() {
        if case let .success(directory) = probe { try? FileManager.default.removeItem(at: directory) }
        super.tearDown()
    }

    private static func run(_ executable: URL, _ arguments: [String], directory: URL,
                            timeout: Double) throws -> String {
        let output = directory.appendingPathComponent(UUID().uuidString)
        _ = FileManager.default.createFile(atPath: output.path, contents: nil)
        let handle = try FileHandle(forWritingTo: output)
        defer { try? handle.close(); try? FileManager.default.removeItem(at: output) }
        let process = Process()
        process.executableURL = executable
        process.arguments = arguments
        process.standardOutput = handle
        process.standardError = handle
        let finished = DispatchSemaphore(value: 0)
        process.terminationHandler = { _ in finished.signal() }
        try process.run()
        guard finished.wait(timeout: .now() + timeout) == .success else {
            process.terminate()
            throw ProbeError.failed("timing probe exceeded its bounded runtime")
        }
        let text = try String(contentsOf: output, encoding: .utf8)
        guard process.terminationStatus == 0 else { throw ProbeError.failed(text) }
        return text
    }

    private func record(_ mode: String) throws -> Recording {
        let directory = try Self.probe.get()
        let text = try Self.run(directory.appendingPathComponent("probe"), [mode],
                                directory: directory, timeout: 5)
        print("Player timing \(mode): \(text.trimmingCharacters(in: .whitespacesAndNewlines))")
        return try JSONDecoder().decode(Recording.self, from: Data(text.utf8))
    }

    func testInitialOffsetUsesSpeedAndPreservesZeroOffset() throws {
        for (mode, delay, gap) in [("normal", 0.4, 0.2), ("double", 0.2, 0.1), ("zero", 0.0, 0.2)] {
            let result = try record(mode)
            XCTAssertEqual(result.times.count, 2)
            let first = try XCTUnwrap(result.times.first)
            let last = try XCTUnwrap(result.times.last)
            XCTAssertGreaterThanOrEqual(first, delay - 0.025, mode)
            XCTAssertGreaterThanOrEqual(last, first, mode)
            XCTAssertEqual(result.intendedOffsets.count, 2, mode)
            XCTAssertEqual(try XCTUnwrap(result.intendedOffsets.first), 0,
                           accuracy: 0.000_001, mode)
            XCTAssertEqual(try XCTUnwrap(result.intendedOffsets.last), gap,
                           accuracy: 0.000_001, mode)
            XCTAssertEqual(result.starts.count, 1)
            XCTAssertEqual(try XCTUnwrap(result.starts.first), first, accuracy: 0.01)
        }
    }

    func testIdleLeadRestartAndLoopEachHonorTheInitialOffset() throws {
        let lead = try record("lead")
        XCTAssertGreaterThanOrEqual(try XCTUnwrap(lead.times.first), 0.475)
        let restart = try record("restart")
        XCTAssertEqual(restart.offsets, [0.4, 0.4, 0.6])
        XCTAssertGreaterThanOrEqual(restart.times[1] - restart.times[0], 0.475)
        XCTAssertEqual(restart.starts.count, 1)
        let loop = try record("loop")
        XCTAssertEqual(loop.offsets, [0.4, 0.6, 0.4, 0.6])
        XCTAssertGreaterThanOrEqual(loop.times[2] - loop.times[1], 0.575)
        XCTAssertEqual(loop.starts.count, 1)
        let duringWait = try record("restart-wait")
        XCTAssertGreaterThanOrEqual(try XCTUnwrap(duringWait.times.first), 0.425)
    }

    func testExplicitSeeksStepsAndEmissionRetryStayImmediate() throws {
        for mode in ["seek-zero", "seek-one", "seek-during", "step", "retry"] {
            let result = try record(mode)
            XCTAssertEqual(result.offsets, [mode == "seek-one" || mode == "seek-during" ? 0.6 : 0.4], mode)
            XCTAssertLessThan(try XCTUnwrap(result.times.first), 0.2, mode)
            XCTAssertEqual(result.starts.count, 1, mode)
        }
    }

    func testInitialWaitFreezesForPauseReadinessAndCanBeStopped() throws {
        for mode in ["pause", "readiness"] {
            let result = try record(mode)
            XCTAssertGreaterThanOrEqual(try XCTUnwrap(result.times.first), 0.525, mode)
            XCTAssertEqual(result.offsets, [0.4, 0.6])
        }
        for mode in ["stop", "empty"] {
            let result = try record(mode)
            XCTAssertTrue(result.times.isEmpty, mode)
            XCTAssertTrue(result.starts.isEmpty, mode)
        }
    }

    func testIdleTailClearsLiveTableBeforeIdleDisplayAndPreservesTimelineEvidence() throws {
        for mode in ["tail", "zero-tail"] {
            let result = try record(mode)
            let timeline = result.packets.filter { $0.sequence > 0 }
            XCTAssertEqual(timeline.map { $0.bytes[3] }, [0x43, 0x31, 0x43, 0x31], mode)
            XCTAssertEqual(timeline.map { $0.sequence }, [1, 1, 2, 2], mode)
            XCTAssertEqual(timeline.map { $0.ordinal }, [0, 1, 0, 1], mode)
            let lastLiveRow = try XCTUnwrap(timeline.last { $0.bytes[3] == 0x43 })
            XCTAssertEqual(lastLiveRow.bytes[5], 0x11, "Final sample must retain a live alert")

            let clears = result.packets.indices.filter {
                result.packets[$0].bytes[3] == 0x43 && result.packets[$0].bytes[5] == 0
            }
            XCTAssertEqual(clears.count, 1, mode)
            let clear = try XCTUnwrap(clears.first)
            let firstIdle = try XCTUnwrap(result.packets.firstIndex {
                $0.bytes[3] == 0x31 && $0.sequence == -1
            })
            XCTAssertLessThan(clear, firstIdle, "The previous live table must clear before idle display traffic")
            XCTAssertEqual(result.packets[clear].sequence, -1)
            XCTAssertEqual(result.offsets, [0.0, 0.05], mode)
            XCTAssertEqual(result.starts.count, 1, mode)
        }
    }

    func testIdleTailPreservesAlertStreamOptions() throws {
        for mode in ["tail-display-only", "tail-no-start"] {
            let result = try record(mode)
            XCTAssertFalse(result.packets.isEmpty, mode)
            XCTAssertTrue(result.packets.allSatisfy { $0.bytes[3] == 0x31 }, mode)
        }
    }
}

private let probeSource = #"""
import Foundation

// No CoreBluetooth imports or manager: only the actual Player's adapter surface.
final class V1Peripheral {
    private let lock = NSLock()
    private var session = V1.Session()
    private var ready = true
    private var checks = 0
    var failThirdCheck = false
    let raced = DispatchSemaphore(value: 0)
    var displaySubscribed: Bool { true }
    var alertDataRequested: Bool {
        lock.lock(); defer { lock.unlock() }
        checks += 1
        // A step forces waitUntil's second readiness check to fire; the third
        // check is emit's race guard. Hold it unavailable until the test releases it.
        if failThirdCheck && checks == 3 { ready = false; raced.signal() }
        return ready
    }
    func setReady(_ value: Bool) { lock.lock(); ready = value; lock.unlock() }
    var controlState: V1.Session.ControlState {
        lock.lock(); defer { lock.unlock() }; return session.controlState
    }
    func applyDetectorMode(_ mode: V1.ModeGlyph) -> V1.Session.ControlState {
        lock.lock(); defer { lock.unlock() }; return session.applyDetectorMode(mode)
    }
    func applyDetectorCurrentVolume(_ value: DetectorVolume) -> V1.Session.ControlState {
        lock.lock(); defer { lock.unlock() }
        return session.applyDetectorCurrentVolume(main: value.mainVolume, muted: value.muteVolume)
    }
    var capture: (([UInt8], Int?, Int?) -> Void)?
    func ensureHandshakeClear(_ bytes: [UInt8]) {}
    func sendDisplay(_ bytes: [UInt8], stimulusSequence: Int? = nil,
                     emissionOrdinal: Int? = nil, intendedHostMonotonicNs: UInt64? = nil) {
        capture?(bytes, stimulusSequence, emissionOrdinal)
    }
    func sendLong(_ bytes: [UInt8], stimulusSequence: Int? = nil,
                  emissionOrdinal: Int? = nil, intendedHostMonotonicNs: UInt64? = nil) {}
}

@main struct TimingProbe {
    static func waitForTimeline(_ player: Player) {
        let limit = nowSeconds() + 1
        while player.snapshot.phase != .playing && player.snapshot.phase != .paused {
            precondition(nowSeconds() < limit, "timeline did not start")
            Thread.sleep(forTimeInterval: 0.001)
        }
    }

    static func main() throws {
        let mode = CommandLine.arguments[1]
        let tailMode = ["tail", "zero-tail", "tail-display-only", "tail-no-start"].contains(mode)
        let offsets = tailMode ? [0.0, 0.05] :
            (mode == "empty" ? [] : (mode == "zero" ? [0.0, 0.2] : [0.4, 0.6]))
        let encounter = Encounter(origin: .externalInput, samples: offsets.enumerated().map {
            TimedSample(offset: $0.element, phase: "test", muted: false,
                        alerts: [ReplayAlert(band: .ka, frequencyMHz: 34_700, strength: 3,
                                             direction: .front, isPriority: true)], sourceIndex: $0.offset)
        })
        var options = Player.Options()
        options.idleLead = ["lead", "restart", "loop"].contains(mode) ? 0.1 : 0
        options.idleTail = mode == "loop" || (tailMode && mode != "zero-tail") ? 0.1 : 0
        options.idleHz = 20
        options.waitForAlertData = mode != "tail-no-start"
        options.sendAlerts = mode != "tail-display-only"
        options.speed = mode == "double" ? 2 : 1
        options.loop = mode == "loop"
        options.startPaused = ["pause", "step"].contains(mode)
        let peripheral = V1Peripheral()
        if mode == "tail-no-start" { peripheral.setReady(false) }
        peripheral.failThirdCheck = mode == "retry"
        let player = Player(encounter: encounter, peripheral: peripheral, options: options)
        if mode == "seek-zero" { player.seek(to: 0) }
        if mode == "seek-one" { player.seek(to: 1) }
        if mode == "retry" { player.step() }
        let lock = NSLock()
        let arrived = DispatchSemaphore(value: 0)
        let finishedIdle = DispatchSemaphore(value: 0)
        var times: [Double] = []
        var intendedOffsets: [Double] = []
        var firstIntendedNanoseconds: UInt64?
        var emittedOffsets: [Double] = []
        var starts: [Double] = []
        var packets: [[String: Any]] = []
        let started = nowSeconds()
        if tailMode {
            peripheral.capture = { bytes, sequence, ordinal in
                let phase = player.snapshot.phase.rawValue
                lock.lock()
                packets.append(["bytes": bytes, "phase": phase,
                                "sequence": sequence ?? -1, "ordinal": ordinal ?? -1])
                lock.unlock()
                if phase == Player.Phase.finished.rawValue && bytes[3] == 0x31 {
                    finishedIdle.signal()
                }
            }
        }
        player.onReplayStarted = { time in
            lock.lock(); starts.append(time - started); lock.unlock()
        }
        player.onStimulusRequested = { event in
            lock.lock()
            times.append(nowSeconds() - started)
            let firstIntended = firstIntendedNanoseconds ?? event.intendedHostMonotonicNs
            firstIntendedNanoseconds = firstIntended
            intendedOffsets.append(
                Double(event.intendedHostMonotonicNs - firstIntended) / 1_000_000_000.0
            )
            emittedOffsets.append(event.replayOffsetSeconds)
            let count = times.count
            lock.unlock()
            if mode == "restart" && count == 1 { player.restart() }
            arrived.signal()
        }
        player.start()
        switch mode {
        case "pause", "readiness":
            waitForTimeline(player)
            if mode == "readiness" { peripheral.setReady(false) }
            Thread.sleep(forTimeInterval: 0.15)
            if mode == "pause" { player.togglePause() } else { peripheral.setReady(true) }
        case "seek-during":
            waitForTimeline(player); player.seek(to: 1)
        case "step":
            waitForTimeline(player); player.step()
        case "stop", "restart-wait":
            waitForTimeline(player)
            Thread.sleep(forTimeInterval: 0.05)
            if mode == "stop" {
                player.stop(); Thread.sleep(forTimeInterval: 0.45)
            } else { player.restart() }
        case "retry":
            precondition(peripheral.raced.wait(timeout: .now() + 1) == .success, "emit race was not exercised")
            player.togglePause()
            peripheral.setReady(true)
        case "empty":
            Thread.sleep(forTimeInterval: 0.1)
        default: break
        }
        let expected: Int
        if ["stop", "empty"].contains(mode) { expected = 0 }
        else if mode == "loop" { expected = 4 }
        else if mode == "restart" { expected = 3 }
        else if ["seek-zero", "seek-one", "seek-during", "step", "retry"].contains(mode) { expected = 1 }
        else { expected = 2 }
        for _ in 0..<expected {
            precondition(arrived.wait(timeout: .now() + 2) == .success, "missing emission")
        }
        if tailMode {
            precondition(finishedIdle.wait(timeout: .now() + 2) == .success, "missing finished idle")
        }
        player.stop()
        Thread.sleep(forTimeInterval: 0.03)
        lock.lock()
        let output: [String: Any] = ["times": times, "offsets": emittedOffsets,
                                     "intendedOffsets": intendedOffsets, "starts": starts,
                                     "packets": packets]
        lock.unlock()
        let data = try JSONSerialization.data(withJSONObject: output, options: [.sortedKeys])
        print(String(decoding: data, as: UTF8.self))
    }
}
"""#

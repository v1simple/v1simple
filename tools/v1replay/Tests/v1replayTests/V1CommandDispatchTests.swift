import Foundation
import XCTest

final class V1CommandDispatchTests: XCTestCase {
    private enum ProbeError: Error { case failed(String) }
    private struct Result {
        let status: Int32
        let output: String
    }

    // Compile the real parser, option guards, and dispatch with inert handlers.
    // No BLE, playback, capture, state-store, or terminal owner enters this probe.
    private static let probe: Swift.Result<URL, Error> = Swift.Result {
        let package = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
            .deletingLastPathComponent().deletingLastPathComponent()
        let sources = package.appendingPathComponent("Sources/v1replay")
        let main = try String(contentsOf: sources.appendingPathComponent("main.swift"), encoding: .utf8)
        let lifecycle = try String(contentsOf: sources.appendingPathComponent("ProcessLifecycle.swift"),
                                   encoding: .utf8)
        func section(_ source: String, from start: String, to end: String? = nil) throws -> String {
            let startRange = try XCTUnwrap(source.range(of: start))
            let remainder = source[startRange.lowerBound...]
            guard let end else { return String(remainder) }
            let endRange = try XCTUnwrap(remainder.range(of: end))
            return String(remainder[..<endRange.lowerBound])
        }
        let code = try [
            section(main, from: "import Foundation", to: "/// Thread-safe boolean"),
            section(main, from: "let args = Arguments", to: "// MARK: - Shared option construction"),
            section(lifecycle, from: "struct ProcessOwnerGuard", to: "/// Suppresses duplicate"),
            section(main, from: "func parseHandshakeNotificationHoldMilliseconds", to: "// MARK: - Commands"),
            inertHandlers,
            section(main, from: "// MARK: - Dispatch"),
        ].joined(separator: "\n")
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        do {
            let source = directory.appendingPathComponent("main.swift")
            try code.write(to: source, atomically: true, encoding: .utf8)
            let result = try run(URL(fileURLWithPath: "/usr/bin/xcrun"),
                                 ["swiftc", "-module-cache-path", directory.appendingPathComponent("cache").path,
                                  source.path, "-o", directory.appendingPathComponent("probe").path],
                                 directory: directory, timeout: 60)
            guard result.status == 0 else { throw ProbeError.failed(result.output) }
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
                            timeout: Double) throws -> Result {
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
            throw ProbeError.failed("dispatch probe exceeded its bounded runtime")
        }
        return Result(status: process.terminationStatus,
                      output: try String(contentsOf: output, encoding: .utf8)
                        .trimmingCharacters(in: .whitespacesAndNewlines))
    }

    private func dispatch(_ arguments: [String]) throws -> Result {
        let directory = try Self.probe.get()
        return try Self.run(directory.appendingPathComponent("probe"), arguments,
                            directory: directory, timeout: 5)
    }

    func testHelpFlagsSkipEveryCommandHandlerAndOptionValidation() throws {
        for command in [[], ["play"], ["demo"], ["bench"], ["idle"], ["proxy"], ["export"],
                        ["crib"], ["help"], ["version"]] {
            for flag in ["--help", "-h"] {
                for options in [[], ["--owner-pid", "invalid"],
                                ["--handshake-notification-hold-ms", "invalid"],
                                ["--speed"], ["--name"], ["--capture"], ["--owner-pid"],
                                ["--handshake-notification-hold-ms"]] {
                    let arguments = command + options + [flag]
                    let result = try dispatch(arguments)
                    XCTAssertEqual(result.status, 0, arguments.joined(separator: " "))
                    XCTAssertEqual(result.output, "help", arguments.joined(separator: " "))
                }
            }
        }
    }

    func testVersionFlagsSkipEveryCommandHandlerAndOptionValidation() throws {
        let version = try dispatch(["version"])
        XCTAssertEqual(version.status, 0)
        XCTAssertTrue(version.output.hasPrefix("v1replay "))
        for command in [[], ["play"], ["demo"], ["bench"], ["idle"], ["proxy"], ["export"], ["crib"]] {
            for options in [[], ["--owner-pid", "invalid"],
                            ["--handshake-notification-hold-ms", "invalid"],
                            ["--speed"], ["--name"], ["--capture"], ["--owner-pid"],
                            ["--handshake-notification-hold-ms"]] {
                let arguments = command + options + ["--version"]
                let result = try dispatch(arguments)
                XCTAssertEqual(result.status, 0, arguments.joined(separator: " "))
                XCTAssertEqual(result.output, version.output, arguments.joined(separator: " "))
            }
        }
        XCTAssertEqual(try dispatch(["bench", "--version", "--help"]).output, "help")
        XCTAssertEqual(try dispatch(["help", "--owner-pid", "invalid"]).output, "help")
        XCTAssertEqual(try dispatch(["version", "--owner-pid", "invalid"]).output, version.output)
    }

    func testOrdinaryCommandsStillDispatchToTheirHandlers() throws {
        let cases: [([String], String)] = [
            ([], "help"), (["play"], "help"), (["help"], "help"),
            (["play", "input.json"], "play idle=false synthetic=false bench=false hold=0 owner=false"),
            (["input.json"], "play idle=false synthetic=false bench=false hold=0 owner=false"),
            (["demo"], "play idle=false synthetic=true bench=false hold=0 owner=false"),
            (["bench"], "play idle=false synthetic=false bench=true hold=0 owner=false"),
            (["idle"], "play idle=true synthetic=false bench=false hold=0 owner=false"),
            (["bench", "--handshake-only", "--handshake-notification-hold-ms", "12"],
             "play idle=false synthetic=false bench=true hold=12 owner=false"),
            (["idle", "--owner-pid", String(ProcessInfo.processInfo.processIdentifier)],
             "play idle=true synthetic=false bench=false hold=0 owner=true"),
            (["proxy"], "proxy"), (["export"], "export"), (["crib"], "crib"),
        ]
        for (arguments, expected) in cases {
            let result = try dispatch(arguments)
            XCTAssertEqual(result.status, 0, arguments.joined(separator: " "))
            XCTAssertEqual(result.output, expected, arguments.joined(separator: " "))
        }
    }

    func testOrdinaryCommandsStillRejectInvalidGuardOptions() throws {
        for arguments in [["bench", "--owner-pid", "invalid"], ["proxy", "--owner-pid", "1"],
                          ["bench", "--handshake-notification-hold-ms", "1"],
                          ["bench", "--handshake-only", "--handshake-notification-hold-ms", "invalid"]] {
            let result = try dispatch(arguments)
            XCTAssertEqual(result.status, 2, arguments.joined(separator: " "))
            XCTAssertTrue(result.output.hasPrefix("v1replay: --"), result.output)
        }
    }

    func testCommandAckFaultDispatchIsExplicitAndExclusive() throws {
        for delay in ["0", "7000"] {
            let result = try dispatch(["bench", "--command-ack-hold-ms", delay])
            XCTAssertEqual(result.status, 0)
            XCTAssertTrue(result.output.hasSuffix(" ackHold=" + delay), result.output)
        }
        for arguments in [["idle", "--command-ack-hold-ms", "7000"],
                          ["bench", "--command-ack-hold-ms"],
                          ["bench", "--command-ack-hold-ms", "10001"],
                          ["bench", "--handshake-only", "--handshake-notification-hold-ms", "1",
                           "--command-ack-hold-ms", "7000"]] {
            XCTAssertEqual(try dispatch(arguments).status, 2, arguments.joined(separator: " "))
        }
    }
}

private let inertHandlers = #"""
enum ReplayError: Error {
    case message(String)
    var description: String { switch self { case .message(let text): return text } }
}
struct HandshakeNotificationHoldState { static let upperBoundMilliseconds = 2000 }
struct Console {
    func print(_ text: String) { Swift.print(text) }
    func restore() {}
}
func runHelp() { print("help") }
func runCrib() { print("crib") }
func runExport() throws { print("export") }
func runProxy() throws { print("proxy") }
func runPlay(idleOnly: Bool, synthetic: Bool = false, bench: Bool = false,
             handshakeNotificationHoldMs: Int = 0, commandAckHoldMs: Int? = nil,
             ownerProcess: ProcessOwnerGuard? = nil) throws {
    let ack = commandAckHoldMs.map { " ackHold=\($0)" } ?? ""
    print("play idle=\(idleOnly) synthetic=\(synthetic) bench=\(bench) hold=\(handshakeNotificationHoldMs) owner=\(ownerProcess != nil)\(ack)")
}
"""#

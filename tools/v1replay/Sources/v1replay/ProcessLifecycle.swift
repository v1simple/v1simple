import Foundation
import Darwin

/// Converts process signals into ordinary control flow so BLE teardown can run.
final class GracefulSignalMonitor {
    private let sources: [DispatchSourceSignal]

    init(signals: [Int32] = [SIGINT, SIGTERM, SIGHUP], onSignal: @escaping () -> Void) {
        var installed: [DispatchSourceSignal] = []
        for signalNumber in signals {
            _ = Darwin.signal(signalNumber, SIG_IGN)
            let source = DispatchSource.makeSignalSource(
                signal: signalNumber,
                queue: DispatchQueue.global(qos: .userInitiated)
            )
            source.setEventHandler(handler: onSignal)
            installed.append(source)
        }
        sources = installed
        sources.forEach { $0.resume() }
    }

    func cancel() {
        sources.forEach { $0.cancel() }
    }

    deinit {
        cancel()
    }
}

/// Managed bench processes must remain direct children of the runner that owns them.
struct ProcessOwnerGuard: Equatable {
    static let pollIntervalSeconds = 0.05

    let ownerPID: pid_t

    func isDirectParent(_ currentParentPID: pid_t = getppid()) -> Bool {
        currentParentPID == ownerPID
    }
}

func parseProcessOwnerGuard(
    _ raw: String?,
    command: String,
    directParentPID: pid_t = getppid()
) throws -> ProcessOwnerGuard? {
    guard let raw else { return nil }
    guard command == "bench" || command == "idle" else {
        throw ReplayError.message("--owner-pid is available only in bench or idle mode")
    }
    guard let parsed = Int(raw), parsed > 0, parsed <= Int(Int32.max) else {
        throw ReplayError.message("--owner-pid must be a positive process ID")
    }

    let owner = ProcessOwnerGuard(ownerPID: pid_t(parsed))
    guard owner.isDirectParent(directParentPID) else {
        throw ReplayError.message("--owner-pid must identify v1replay's direct parent")
    }
    return owner
}

/// Suppresses duplicate boolean machine events while retaining the first observation.
struct BooleanTransitionState {
    private(set) var lastValue: Bool?

    mutating func shouldEmit(_ value: Bool) -> Bool {
        guard lastValue != value else { return false }
        lastValue = value
        return true
    }
}

final class BooleanMachineEventEmitter {
    private let lock = NSLock()
    private var state = BooleanTransitionState()
    private let emitEvent: (Bool) -> Void

    init(emitEvent: @escaping (Bool) -> Void) {
        self.emitEvent = emitEvent
    }

    func emit(_ value: Bool) {
        lock.lock()
        defer { lock.unlock() }
        if state.shouldEmit(value) { emitEvent(value) }
    }
}

struct StoppingMachineEvent: Equatable {
    let sessionTransportActive: Bool

    var line: String {
        let active = sessionTransportActive ? "true" : "false"
        return "V1REPLAY_EVENT {\"state\":\"stopping\","
            + "\"sessionTransportActive\":\(active)}"
    }
}

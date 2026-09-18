import Foundation

/// Durable state for the emulated detector. Writes run off the BLE queue so
/// profile persistence can never delay alert delivery or command responses.
final class V1DetectorStateStore {
    private let url: URL
    private let queue = DispatchQueue(label: "com.v1simple.v1replay.detector-state")
    private let lock = NSLock()
    private var firstError: Error?

    init(path: String) {
        self.url = URL(fileURLWithPath: path).standardizedFileURL
    }

    func load(expectedVersion: String) throws -> V1.Session.PersistentState? {
        guard FileManager.default.fileExists(atPath: url.path) else { return nil }
        let data: Data
        do {
            data = try Data(contentsOf: url)
        } catch {
            throw ReplayError.message("could not read emulator detector state")
        }
        let state: V1.Session.PersistentState
        do {
            state = try JSONDecoder().decode(V1.Session.PersistentState.self, from: data)
        } catch {
            throw ReplayError.message("emulator detector state is malformed")
        }
        guard state.v1Version == expectedVersion else {
            throw ReplayError.message("emulator state belongs to a different V1 version")
        }
        return state
    }

    func save(_ state: V1.Session.PersistentState) {
        queue.async {
            do {
                let directory = self.url.deletingLastPathComponent()
                try FileManager.default.createDirectory(
                    at: directory,
                    withIntermediateDirectories: true
                )
                let encoder = JSONEncoder()
                encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
                let data = try encoder.encode(state)
                try data.write(to: self.url, options: .atomic)
            } catch {
                self.lock.lock()
                if self.firstError == nil { self.firstError = error }
                self.lock.unlock()
            }
        }
    }

    func flush() throws {
        queue.sync {}
        lock.lock()
        let error = firstError
        lock.unlock()
        if error != nil {
            throw ReplayError.message("could not persist emulator detector state")
        }
    }
}

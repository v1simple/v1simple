import Foundation

/// Bounded, anonymous evidence for the automatic V1Simple startup handshake.
///
/// This is intentionally not a packet transcript. It records only the short
/// subscription, the three startup requests accepted by the emulator, their
/// two targeted replies after CoreBluetooth accepts them for delivery, and the
/// first delivered alert row in each logical session.
final class HandshakeLedger {
    static let schemaVersion = 1
    static let maximumEpochs = 4
    static let maximumEventsPerEpoch = 12

    private let lock = NSLock()
    private let handle: FileHandle
    private var epoch = 0
    private var currentEpoch: Int?
    private var eventCountByEpoch: [Int: Int] = [:]
    private var streamRecordedEpochs: Set<Int> = []
    private var writeFailed = false

    init(path: String) throws {
        let url = URL(fileURLWithPath: path)
        let header: [String: Any] = [
            "schema_version": HandshakeLedger.schemaVersion,
            "kind": "v1replay_handshake_ledger",
        ]
        let headerData = try HandshakeLedger.lineData(header)
        do {
            try headerData.write(to: url, options: .withoutOverwriting)
        } catch let error as CocoaError where error.code == .fileWriteFileExists {
            throw ReplayError.message("refusing to overwrite existing handshake evidence")
        }
        self.handle = try FileHandle(forWritingTo: url)
        self.handle.seekToEndOfFile()
    }

    deinit {
        try? handle.close()
    }

    /// Begin a new anonymous logical session. The returned epoch is a local
    /// sequence number only; no CoreBluetooth identifier is persisted.
    @discardableResult
    func beginEpoch() -> Int? {
        lock.lock()
        defer { lock.unlock() }
        guard epoch < HandshakeLedger.maximumEpochs, !writeFailed else {
            currentEpoch = nil
            return nil
        }
        epoch += 1
        currentEpoch = epoch
        eventCountByEpoch[epoch] = 0
        writeEvent([
            "event": "subscribe",
            "epoch": epoch,
            "channel": "B2CE",
        ], epoch: epoch)
        return epoch
    }

    func endEpoch() {
        lock.lock()
        currentEpoch = nil
        lock.unlock()
    }

    /// Snapshot the epoch when a notification is queued. Delivery is credited
    /// only to that same epoch after CoreBluetooth accepts the notification.
    var activeEpoch: Int? {
        lock.lock()
        defer { lock.unlock() }
        return currentEpoch
    }

    func recordAcceptedRequest(bytes: [UInt8],
                               channel: String,
                               belongsToEpochSubscriber: Bool) {
        lock.lock()
        defer { lock.unlock() }
        guard let epoch = currentEpoch else { return }
        guard belongsToEpochSubscriber else {
            // A command from any other central makes subsequent broadcast
            // notification attribution ambiguous for the rest of this epoch.
            currentEpoch = nil
            return
        }
        guard let packetID = HandshakeLedger.packetID(bytes),
              [UInt8(0x41), UInt8(0x01), UInt8(0x3C)].contains(packetID) else {
            return
        }
        writeEvent([
            "event": "request",
            "epoch": epoch,
            "channel": channel,
            "bytes": bytes.map(Int.init),
        ], epoch: epoch)
    }

    func recordDelivered(bytes: [UInt8], channel: String, epoch queuedEpoch: Int?) {
        guard let packetID = HandshakeLedger.packetID(bytes),
              packetID == 0x02 || packetID == 0x3D || packetID == 0x43 else {
            return
        }
        lock.lock()
        defer { lock.unlock() }
        guard let queuedEpoch,
              queuedEpoch == currentEpoch else { return }
        let event: String
        if packetID == 0x43 {
            guard !streamRecordedEpochs.contains(queuedEpoch) else { return }
            streamRecordedEpochs.insert(queuedEpoch)
            event = "stream_started"
        } else {
            event = "response"
        }
        writeEvent([
            "event": event,
            "epoch": queuedEpoch,
            "channel": channel,
            "bytes": bytes.map(Int.init),
            "delivery": "delivered",
        ], epoch: queuedEpoch)
    }

    private func writeEvent(_ object: [String: Any], epoch: Int) {
        guard !writeFailed else { return }
        let count = eventCountByEpoch[epoch, default: 0]
        guard count < HandshakeLedger.maximumEventsPerEpoch else { return }
        do {
            handle.write(try HandshakeLedger.lineData(object))
            try handle.synchronize()
            eventCountByEpoch[epoch] = count + 1
        } catch {
            writeFailed = true
            currentEpoch = nil
        }
    }

    private static func packetID(_ bytes: [UInt8]) -> UInt8? {
        guard bytes.count > 3 else { return nil }
        return bytes[3]
    }

    private static func lineData(_ object: [String: Any]) throws -> Data {
        var data = try JSONSerialization.data(withJSONObject: object, options: [.sortedKeys])
        data.append(0x0A)
        return data
    }

}

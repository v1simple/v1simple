import Foundation
import CoreBluetooth

// =============================================================================
// Proxy mode: the Mac sits between a real Valentine One and v1simple.
//
//   real V1  <--BLE-->  [ V1ProxyCentral | V1Peripheral ]  <--BLE-->  v1simple
//
// Every frame in both directions is written to a JSONL capture file. This is a
// protocol-learning tool, not a timing tool: two links share one radio, so
// latency measured through the proxy is meaningless. Semantics only.
// =============================================================================

// MARK: - Capture log

/// Append-only JSONL writer. All BLE callbacks land on the shared BLE queue,
/// but `note` can be called from the main thread, so writes are locked.
final class PacketLog {

    let path: String
    private let lock = NSLock()
    private var handle: FileHandle?
    private let started = Date()
    private let tag: String?

    /// Rolling reassembly buffers, one per direction, so a frame split across
    /// two BLE writes still decodes.
    private var buffers: [String: [UInt8]] = [:]

    init(path: String, tag: String?) throws {
        self.path = path
        self.tag = tag

        let directory = (path as NSString).deletingLastPathComponent
        if !directory.isEmpty {
            try FileManager.default.createDirectory(atPath: directory,
                                                    withIntermediateDirectories: true)
        }
        if !FileManager.default.fileExists(atPath: path) {
            FileManager.default.createFile(atPath: path, contents: nil)
        }
        guard let handle = FileHandle(forWritingAtPath: path) else {
            throw ReplayError.message("cannot open capture file \(path)")
        }
        handle.seekToEndOfFile()
        self.handle = handle
    }

    private static let wallFormatter: DateFormatter = {
        let formatter = DateFormatter()
        formatter.dateFormat = "yyyy-MM-dd'T'HH:mm:ss.SSS'Z'"
        formatter.timeZone = TimeZone(identifier: "UTC")
        formatter.locale = Locale(identifier: "en_US_POSIX")
        return formatter
    }()

    private func emit(_ fields: [String: Any]) {
        var object = fields
        let now = Date()
        object["t"] = (now.timeIntervalSince(started) * 1000).rounded() / 1000
        object["wall"] = PacketLog.wallFormatter.string(from: now)
        if let tag = tag { object["tag"] = tag }

        guard let data = try? JSONSerialization.data(withJSONObject: object,
                                                     options: [.sortedKeys]) else { return }
        lock.lock()
        defer { lock.unlock() }
        handle?.write(data)
        handle?.write(Data("\n".utf8))
    }

    /// Record a raw BLE chunk, then any complete V1 frames it completed.
    func record(direction: String, characteristic: CBUUID, bytes: [UInt8]) {
        let charName = V1Peripheral.shortName(characteristic)
        emit([
            "kind": "chunk",
            "dir": direction,
            "char": charName,
            "len": bytes.count,
            "hex": bytes.hexString
        ])

        var buffer = buffers[direction] ?? []
        buffer.append(contentsOf: bytes)
        if buffer.count > 1024 { buffer.removeFirst(buffer.count - 1024) }
        let frames = V1.drainFrames(from: &buffer)
        buffers[direction] = buffer

        for frame in frames {
            emit([
                "kind": "frame",
                "dir": direction,
                "char": charName,
                "id": String(format: "0x%02X", frame.id),
                "name": V1.name(forPacketID: frame.id),
                "len": frame.raw.count,
                "payload": frame.payload.hexString,
                "hex": frame.raw.hexString
            ])
        }
    }

    func note(_ event: String, _ detail: [String: String] = [:]) {
        var object: [String: Any] = ["kind": "event", "event": event]
        for (key, value) in detail { object[key] = value }
        emit(object)
    }

    func close() {
        lock.lock()
        defer { lock.unlock() }
        try? handle?.close()
        handle = nil
    }
}

// MARK: - Central half

/// Connects to a real V1 and mirrors its notify characteristics.
final class V1ProxyCentral: NSObject {

    struct Config {
        /// Substring match on the advertised local name. nil accepts any device
        /// advertising the V1 service UUID.
        var nameFilter: String?
        /// Never connect to our own peripheral advertisement.
        var excludeName: String
    }

    var onLog: ((String) -> Void)?
    var onNotify: ((CBUUID, [UInt8]) -> Void)?
    var onConnected: ((String, String) -> Void)?
    var onDisconnected: ((String?) -> Void)?

    private let config: Config
    private let queue: DispatchQueue
    private var manager: CBCentralManager!
    private var target: CBPeripheral?

    private let stateLock = NSLock()
    private var discovered: [CBUUID: CBCharacteristic] = [:]
    private var subscribedCount = 0
    private var connected = false
    private var peerNameValue: String?

    private func withLock<T>(_ body: () -> T) -> T {
        stateLock.lock()
        defer { stateLock.unlock() }
        return body()
    }

    var isConnected: Bool { return withLock { connected } }
    var subscriptions: Int { return withLock { subscribedCount } }
    var peerName: String? { return withLock { peerNameValue } }

    /// Notify characteristics on the V1 we mirror back to v1simple.
    static let notifyUUIDs: [CBUUID] = [
        CBUUID(string: V1.displayShortUUID),
        CBUUID(string: V1.displayLongUUID),
        CBUUID(string: V1.notifyAltUUID)
    ]

    /// Write characteristics we forward v1simple's commands onto.
    static let writeUUIDs: [CBUUID] = [
        CBUUID(string: V1.commandUUID),
        CBUUID(string: V1.commandLongUUID),
        CBUUID(string: V1.commandAltUUID)
    ]

    init(config: Config, queue: DispatchQueue) {
        self.config = config
        self.queue = queue
        super.init()
        manager = CBCentralManager(delegate: self, queue: queue)
    }

    /// Forward a command onto the V1. Falls back to the primary command
    /// characteristic if the V1 does not expose the one v1simple wrote to.
    func write(_ bytes: [UInt8], to uuid: CBUUID) {
        queue.async {
            guard let peripheral = self.target else { return }
            let characteristic = self.withLock { () -> CBCharacteristic? in
                if let exact = self.discovered[uuid] { return exact }
                for fallback in V1ProxyCentral.writeUUIDs {
                    if let match = self.discovered[fallback] { return match }
                }
                return nil
            }
            guard let characteristic = characteristic else {
                self.onLog?("no writable characteristic on the V1 for \(V1Peripheral.shortName(uuid))")
                return
            }
            let type: CBCharacteristicWriteType =
                characteristic.properties.contains(.writeWithoutResponse) ? .withoutResponse : .withResponse
            peripheral.writeValue(Data(bytes), for: characteristic, type: type)
        }
    }

    func stop() {
        queue.sync {
            if manager.isScanning { manager.stopScan() }
            if let peripheral = target { manager.cancelPeripheralConnection(peripheral) }
        }
    }

    private func startScan() {
        // Scan unfiltered and match manually: not every V1 puts the service
        // UUID in the advertising packet, and we also need the local name to
        // exclude our own peripheral.
        manager.scanForPeripherals(withServices: nil, options: nil)
        onLog?("scanning for a real V1…")
    }
}

// MARK: - CBCentralManagerDelegate

extension V1ProxyCentral: CBCentralManagerDelegate {

    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        switch central.state {
        case .poweredOn:
            startScan()
        case .unauthorized:
            onLog?("Bluetooth permission denied. System Settings → Privacy & Security → Bluetooth, "
                   + "and enable the terminal application that launched v1replay.")
        case .poweredOff:
            onLog?("Bluetooth is off — turn it on to reach the V1")
        default:
            break
        }
    }

    func centralManager(_ central: CBCentralManager,
                        didDiscover peripheral: CBPeripheral,
                        advertisementData: [String: Any],
                        rssi RSSI: NSNumber) {
        let advertisedName = (advertisementData[CBAdvertisementDataLocalNameKey] as? String)
            ?? peripheral.name
            ?? ""

        // Never loop back onto our own advertisement.
        if !advertisedName.isEmpty, advertisedName == config.excludeName { return }

        let services = (advertisementData[CBAdvertisementDataServiceUUIDsKey] as? [CBUUID]) ?? []
        let advertisesV1 = services.contains(CBUUID(string: V1.serviceUUID))

        if let filter = config.nameFilter, !filter.isEmpty {
            guard advertisedName.localizedCaseInsensitiveContains(filter) else { return }
        } else {
            guard advertisesV1 else { return }
        }

        central.stopScan()
        target = peripheral
        peripheral.delegate = self
        withLock { peerNameValue = advertisedName.isEmpty ? peripheral.identifier.uuidString : advertisedName }
        onLog?("found \"\(advertisedName)\" rssi \(RSSI) — connecting")
        central.connect(peripheral, options: nil)
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        onLog?("connected to the V1 — discovering services")
        peripheral.discoverServices([CBUUID(string: V1.serviceUUID)])
    }

    func centralManager(_ central: CBCentralManager,
                        didFailToConnect peripheral: CBPeripheral,
                        error: Error?) {
        onLog?("connect failed: \(error?.localizedDescription ?? "unknown") — rescanning")
        target = nil
        startScan()
    }

    func centralManager(_ central: CBCentralManager,
                        didDisconnectPeripheral peripheral: CBPeripheral,
                        error: Error?) {
        let name = peerName
        withLock {
            connected = false
            subscribedCount = 0
            discovered.removeAll()
        }
        onLog?("V1 disconnected\(error.map { ": \($0.localizedDescription)" } ?? "") — rescanning")
        onDisconnected?(name)
        target = nil
        startScan()
    }
}

// MARK: - CBPeripheralDelegate

extension V1ProxyCentral: CBPeripheralDelegate {

    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        if let error = error {
            onLog?("service discovery failed: \(error.localizedDescription)")
            return
        }
        guard let service = peripheral.services?.first(where: {
            $0.uuid == CBUUID(string: V1.serviceUUID)
        }) else {
            onLog?("connected device does not expose the V1 service — rescanning")
            manager.cancelPeripheralConnection(peripheral)
            return
        }
        peripheral.discoverCharacteristics(nil, for: service)
    }

    func peripheral(_ peripheral: CBPeripheral,
                    didDiscoverCharacteristicsFor service: CBService,
                    error: Error?) {
        if let error = error {
            onLog?("characteristic discovery failed: \(error.localizedDescription)")
            return
        }
        let characteristics = service.characteristics ?? []
        withLock {
            for characteristic in characteristics { discovered[characteristic.uuid] = characteristic }
        }

        let names = characteristics.map { V1Peripheral.shortName($0.uuid) }.joined(separator: " ")
        onLog?("V1 characteristics: \(names)")

        for characteristic in characteristics where characteristic.properties.contains(.notify) {
            peripheral.setNotifyValue(true, for: characteristic)
        }

        withLock { connected = true }
        onConnected?(peerName ?? "V1", names)
    }

    func peripheral(_ peripheral: CBPeripheral,
                    didUpdateNotificationStateFor characteristic: CBCharacteristic,
                    error: Error?) {
        if let error = error {
            onLog?("subscribe to \(V1Peripheral.shortName(characteristic.uuid)) failed: "
                   + error.localizedDescription)
            return
        }
        if characteristic.isNotifying {
            withLock { subscribedCount += 1 }
            onLog?("subscribed to \(V1Peripheral.shortName(characteristic.uuid))")
        }
    }

    func peripheral(_ peripheral: CBPeripheral,
                    didUpdateValueFor characteristic: CBCharacteristic,
                    error: Error?) {
        if let error = error {
            onLog?("notify error on \(V1Peripheral.shortName(characteristic.uuid)): "
                   + error.localizedDescription)
            return
        }
        guard let value = characteristic.value else { return }
        onNotify?(characteristic.uuid, [UInt8](value))
    }

    func peripheral(_ peripheral: CBPeripheral,
                    didWriteValueFor characteristic: CBCharacteristic,
                    error: Error?) {
        if let error = error {
            onLog?("write to \(V1Peripheral.shortName(characteristic.uuid)) failed: "
                   + error.localizedDescription)
        }
    }
}

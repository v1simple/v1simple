import Foundation
import CoreBluetooth

// =============================================================================
// The fake V1: a CBPeripheralManager advertising the V1 service UUID with the
// six characteristics v1simple's subscribe state machine looks for.
//
// Critical path in the firmware (ble_connection.cpp executeSubscribeStep):
//   1. service 92A0AFF4                       — required
//   2. characteristic B2CE, must notify       — required
//   3. B6D4 or BAD4, must be writable         — required
//   4. B8D2                                   — optional
//   5. subscribe B2CE                         — required
//   6. B4E0 if it can notify                  — optional, carries alert rows
// BCE0 is never used by the firmware but companion apps expect it to exist.
// =============================================================================

final class V1Peripheral: NSObject {

    struct Config {
        var localName: String = "V1G-REPLAY"
        var header: V1.Header = .repoConvention
        var checksum: Bool = true
        var version: String = "4.1038"
        var mainVolume: UInt8 = 4
        var mutedVolume: UInt8 = 0
        var userBytes: [UInt8] = Array(repeating: 0, count: 6)
        var logPackets: Bool = false
        /// Proxy mode publishes the service but holds off advertising until the
        /// real V1 is connected, so v1simple cannot win the race to it.
        var deferAdvertising: Bool = false
    }

    // Callbacks (always delivered on `queue`).
    var onLog: ((String) -> Void)?
    var onStateChange: (() -> Void)?
    var onStartAlertData: (() -> Void)?
    var onMuteCommand: ((Bool) -> Void)?
    var onDisplayPowerCommand: ((Bool) -> Void)?

    /// Proxy hook. Receives every inbound write before it is parsed, with the
    /// characteristic it arrived on preserved. Returning true suppresses the
    /// built-in emulator reply for that write. Nil in every non-proxy mode.
    var onRawWrite: ((CBUUID, [UInt8]) -> Bool)?

    /// Everything the console and player threads poll. CoreBluetooth callbacks
    /// land on `queue`; the status line reads from the main thread; the player
    /// polls `displaySubscribed` — so all of it lives behind one lock.
    private struct Subscription: Hashable {
        let central: UUID
        let characteristic: String
    }

    private struct State {
        var isPoweredOn = false
        var isAdvertising = false
        var subscriptions: Set<Subscription> = []
        var notifiesSent = 0
        var notifiesDropped = 0
        var commandsReceived = 0
        var alertDataRequested = false
        var userBytes = V1.UserBytesStore()
    }

    private let stateLock = NSLock()
    private var state = State()

    private func withState<T>(_ body: (inout State) -> T) -> T {
        stateLock.lock()
        defer { stateLock.unlock() }
        return body(&state)
    }

    var isPoweredOn: Bool { return withState { $0.isPoweredOn } }
    var isAdvertising: Bool { return withState { $0.isAdvertising } }
    var subscriberCount: Int { return withState { Set($0.subscriptions.map(\.central)).count } }
    var displaySubscribed: Bool {
        return withState { $0.subscriptions.contains { $0.characteristic == V1.displayShortUUID } }
    }
    var alertSubscribed: Bool {
        return withState { $0.subscriptions.contains { $0.characteristic == V1.displayLongUUID } }
    }
    var notifiesSent: Int { return withState { $0.notifiesSent } }
    var notifiesDropped: Int { return withState { $0.notifiesDropped } }
    var commandsReceived: Int { return withState { $0.commandsReceived } }
    var alertDataRequested: Bool { return withState { $0.alertDataRequested } }

    let queue = DispatchQueue(label: "com.v1simple.v1replay.ble")

    private let config: Config
    private var manager: CBPeripheralManager!

    private var displayChar: CBMutableCharacteristic!   // B2CE
    private var alertChar: CBMutableCharacteristic!     // B4E0
    private var altNotifyChar: CBMutableCharacteristic! // BCE0
    private var commandChar: CBMutableCharacteristic!   // B6D4
    private var commandLongChar: CBMutableCharacteristic! // B8D2
    private var commandAltChar: CBMutableCharacteristic!  // BAD4

    private var pending: [(Data, CBMutableCharacteristic)] = []
    private static let pendingCap = 96
    private var lastValues: [CBUUID: Data] = [:]
    private var rxBuffer: [UInt8] = []
    private var serviceAdded = false

    init(config: Config) {
        self.config = config
        super.init()
        withState { $0.userBytes = V1.UserBytesStore(config.userBytes) }
        manager = CBPeripheralManager(delegate: self, queue: queue, options: nil)
    }

    // MARK: - Transmission

    func sendDisplay(_ bytes: [UInt8]) {
        send(bytes, to: displayChar)
    }

    func sendAlert(_ bytes: [UInt8]) {
        send(bytes, to: alertChar)
    }

    /// Mirror a notification from the real V1 onto the matching characteristic.
    func forward(_ bytes: [UInt8], toCharacteristicUUID uuid: CBUUID) {
        if uuid == CBUUID(string: V1.displayShortUUID) {
            send(bytes, to: displayChar)
        } else if uuid == CBUUID(string: V1.displayLongUUID) {
            send(bytes, to: alertChar)
        } else if uuid == CBUUID(string: V1.notifyAltUUID) {
            send(bytes, to: altNotifyChar)
        } else {
            onLog?("dropped notify on unknown characteristic \(V1Peripheral.shortName(uuid))")
        }
    }

    private func send(_ bytes: [UInt8], to characteristic: CBMutableCharacteristic?) {
        guard let characteristic = characteristic else { return }
        let data = Data(bytes)
        queue.async {
            self.lastValues[characteristic.uuid] = data
            if self.pending.count >= V1Peripheral.pendingCap {
                self.pending.removeFirst()
                self.withState { $0.notifiesDropped += 1 }
            }
            self.pending.append((data, characteristic))
            self.flush()
            if self.config.logPackets {
                self.onLog?("TX \(Self.shortName(characteristic.uuid)) \(bytes.hexString)")
            }
        }
    }

    /// Drain the notify queue until CoreBluetooth pushes back, then wait for
    /// `peripheralManagerIsReady(toUpdateSubscribers:)`.
    private func flush() {
        while let item = pending.first {
            guard manager.updateValue(item.0, for: item.1, onSubscribedCentrals: nil) else { return }
            pending.removeFirst()
            withState { $0.notifiesSent += 1 }
        }
    }

    // MARK: - Setup

    private func buildService() -> CBMutableService {
        func notifyCharacteristic(_ uuid: String) -> CBMutableCharacteristic {
            // Value must be nil for notify/write characteristics — CoreBluetooth
            // treats a non-nil value as a cached read-only constant.
            // CoreBluetooth creates the 0x2902 CCCD for notify characteristics;
            // adding one manually causes an exception.
            return CBMutableCharacteristic(
                type: CBUUID(string: uuid),
                properties: [.read, .notify],
                value: nil,
                permissions: [.readable]
            )
        }

        displayChar = notifyCharacteristic(V1.displayShortUUID)
        alertChar = notifyCharacteristic(V1.displayLongUUID)
        altNotifyChar = notifyCharacteristic(V1.notifyAltUUID)

        commandChar = CBMutableCharacteristic(
            type: CBUUID(string: V1.commandUUID),
            properties: [.writeWithoutResponse],
            value: nil,
            permissions: [.writeable]
        )
        commandLongChar = CBMutableCharacteristic(
            type: CBUUID(string: V1.commandLongUUID),
            properties: [.writeWithoutResponse],
            value: nil,
            permissions: [.writeable]
        )
        commandAltChar = CBMutableCharacteristic(
            type: CBUUID(string: V1.commandAltUUID),
            properties: [.write, .writeWithoutResponse],
            value: nil,
            permissions: [.writeable]
        )

        let service = CBMutableService(type: CBUUID(string: V1.serviceUUID), primary: true)
        service.characteristics = [
            displayChar, alertChar, commandChar,
            commandLongChar, altNotifyChar, commandAltChar
        ]
        return service
    }

    /// Proxy mode calls this once the real V1 is connected.
    func beginAdvertising() {
        queue.async { self.startAdvertising() }
    }

    private func startAdvertising() {
        guard !isAdvertising else { return }
        manager.startAdvertising([
            CBAdvertisementDataLocalNameKey: config.localName,
            CBAdvertisementDataServiceUUIDsKey: [CBUUID(string: V1.serviceUUID)]
        ])
    }

    func stop() {
        queue.sync {
            if manager.isAdvertising { manager.stopAdvertising() }
            manager.removeAllServices()
        }
    }

    // MARK: - Command handling

    private func handle(_ packet: V1.InboundPacket) {
        withState { $0.commandsReceived += 1 }
        let commandName = V1.name(forPacketID: packet.id)
        if config.logPackets {
            onLog?("RX \(commandName) \(packet.raw.hexString)")
        }

        switch packet.id {
        case V1.PacketID.reqVersion.rawValue:
            let reply = V1.versionPacket(header: config.header, version: config.version, checksum: config.checksum)
            onLog?("→ respVersion v\(config.version)")
            send(reply, to: alertChar)

        case V1.PacketID.reqAllVolume.rawValue:
            let reply = V1.allVolumePacket(header: config.header,
                                           main: config.mainVolume,
                                           muted: config.mutedVolume,
                                           checksum: config.checksum)
            onLog?("→ respAllVolume main=\(config.mainVolume) muted=\(config.mutedVolume)")
            send(reply, to: alertChar)

        case V1.PacketID.reqUserBytes.rawValue:
            let storedUserBytes = withState { $0.userBytes.bytes }
            let reply = V1.userBytesPacket(header: config.header, bytes: storedUserBytes, checksum: config.checksum)
            onLog?("→ respUserBytes \(storedUserBytes.hexString)")
            send(reply, to: alertChar)

        case V1.PacketID.reqStartAlertData.rawValue:
            withState { $0.alertDataRequested = true }
            onLog?("← reqStartAlertData — alert table enabled")
            onStartAlertData?()
            onStateChange?()

        case V1.PacketID.reqStopAlertData.rawValue:
            withState { $0.alertDataRequested = false }
            onLog?("← reqStopAlertData")
            onStateChange?()

        case V1.PacketID.muteOn.rawValue:
            onMuteCommand?(true)
            ack(packet.id)

        case V1.PacketID.muteOff.rawValue:
            onMuteCommand?(false)
            ack(packet.id)

        case V1.PacketID.turnOffDisplay.rawValue:
            onDisplayPowerCommand?(false)
            ack(packet.id)

        case V1.PacketID.turnOnDisplay.rawValue:
            onDisplayPowerCommand?(true)
            ack(packet.id)

        case V1.PacketID.reqWriteUserBytes.rawValue:
            let stored = withState { $0.userBytes.write(packet.payload) }
            guard stored else {
                onLog?("← rejected reqWriteUserBytes payload length \(packet.payload.count)")
                return
            }
            onLog?("← stored user bytes \(packet.payload.hexString)")
            ack(packet.id)

        case V1.PacketID.changeMode.rawValue,
             V1.PacketID.reqWriteVolume.rawValue:
            ack(packet.id)

        default:
            onLog?("← unhandled \(commandName)")
        }
    }

    private func ack(_ id: UInt8) {
        let reply = V1.ackPacket(header: config.header, id: id, checksum: config.checksum)
        send(reply, to: alertChar)
    }

    static func shortName(_ uuid: CBUUID) -> String {
        let text = uuid.uuidString.uppercased()
        if text.count >= 8 {
            let start = text.index(text.startIndex, offsetBy: 4)
            let end = text.index(text.startIndex, offsetBy: 8)
            return String(text[start..<end])
        }
        return text
    }
}

// MARK: - CBPeripheralManagerDelegate

extension V1Peripheral: CBPeripheralManagerDelegate {

    func peripheralManagerDidUpdateState(_ peripheral: CBPeripheralManager) {
        switch peripheral.state {
        case .poweredOn:
            withState { $0.isPoweredOn = true }
            if !serviceAdded {
                serviceAdded = true
                peripheral.add(buildService())
            }
        case .poweredOff:
            withState { $0.isPoweredOn = false }
            onLog?("Bluetooth is off — turn it on to advertise")
        case .unauthorized:
            withState { $0.isPoweredOn = false }
            onLog?("Bluetooth permission denied. System Settings → Privacy & Security → Bluetooth, "
                   + "and enable the terminal application that launched v1replay.")
        case .unsupported:
            withState { $0.isPoweredOn = false }
            onLog?("This Mac reports no BLE peripheral support")
        default:
            withState { $0.isPoweredOn = false }
        }
        onStateChange?()
    }

    func peripheralManager(_ peripheral: CBPeripheralManager,
                           didAdd service: CBService,
                           error: Error?) {
        if let error = error {
            onLog?("Failed to publish service: \(error.localizedDescription)")
            return
        }
        onLog?("Service 92A0AFF4 published with 6 characteristics")
        if config.deferAdvertising {
            onLog?("advertising held until the real V1 is connected")
            return
        }
        startAdvertising()
    }

    func peripheralManagerDidStartAdvertising(_ peripheral: CBPeripheralManager, error: Error?) {
        if let error = error {
            onLog?("Advertising failed: \(error.localizedDescription)")
            return
        }
        withState { $0.isAdvertising = true }
        onLog?("Advertising as \"\(config.localName)\" — waiting for v1simple to connect")
        onStateChange?()
    }

    func peripheralManager(_ peripheral: CBPeripheralManager,
                           central: CBCentral,
                           didSubscribeTo characteristic: CBCharacteristic) {
        let name = V1Peripheral.shortName(characteristic.uuid)
        let subscription = Subscription(
            central: central.identifier,
            characteristic: characteristic.uuid.uuidString
        )
        _ = withState { $0.subscriptions.insert(subscription) }
        onLog?("Central subscribed to \(name) (MTU \(central.maximumUpdateValueLength))")
        onStateChange?()
    }

    func peripheralManager(_ peripheral: CBPeripheralManager,
                           central: CBCentral,
                           didUnsubscribeFrom characteristic: CBCharacteristic) {
        let name = V1Peripheral.shortName(characteristic.uuid)
        let subscription = Subscription(
            central: central.identifier,
            characteristic: characteristic.uuid.uuidString
        )
        let remaining = withState { current -> Int in
            current.subscriptions.remove(subscription)
            let count = Set(current.subscriptions.map(\.central)).count
            if count == 0 { current.alertDataRequested = false }
            return count
        }
        onLog?("Central unsubscribed from \(name)")
        if remaining == 0 {
            pending.removeAll()
            rxBuffer.removeAll()
        }
        onStateChange?()
    }

    func peripheralManagerIsReady(toUpdateSubscribers peripheral: CBPeripheralManager) {
        flush()
    }

    func peripheralManager(_ peripheral: CBPeripheralManager,
                           didReceiveRead request: CBATTRequest) {
        let value = lastValues[request.characteristic.uuid] ?? Data()
        guard request.offset <= value.count else {
            peripheral.respond(to: request, withResult: .invalidOffset)
            return
        }
        request.value = value.subdata(in: request.offset..<value.count)
        peripheral.respond(to: request, withResult: .success)
    }

    func peripheralManager(_ peripheral: CBPeripheralManager,
                           didReceiveWrite requests: [CBATTRequest]) {
        // Proxy mode consumes writes raw, keeping the characteristic they
        // arrived on, and suppresses the built-in emulator replies.
        if let hook = onRawWrite {
            var consumed = false
            for request in requests {
                guard let value = request.value else { continue }
                withState { $0.commandsReceived += 1 }
                if hook(request.characteristic.uuid, [UInt8](value)) { consumed = true }
            }
            if consumed {
                onStateChange?()
                if let first = requests.first {
                    peripheral.respond(to: first, withResult: .success)
                }
                return
            }
        }

        for request in requests {
            guard let value = request.value else { continue }
            rxBuffer.append(contentsOf: [UInt8](value))
        }
        // Keep the resync buffer bounded if a peer ever writes garbage.
        if rxBuffer.count > 512 { rxBuffer.removeFirst(rxBuffer.count - 512) }

        let packets = V1.drainFrames(from: &rxBuffer)
        for packet in packets { handle(packet) }
        onStateChange?()

        if let first = requests.first {
            peripheral.respond(to: first, withResult: .success)
        }
    }
}

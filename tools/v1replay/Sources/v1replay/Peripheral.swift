import Foundation
import CoreBluetooth

/// Delivery state for the one canonical clear row used by handshake-only mode.
/// The peripheral queue owns mutation; this value keeps retry and duplicate
/// suppression independently testable.
struct HandshakeClearDeliveryState {
    enum EnsureAction: Equatable {
        case enqueue
        case retryPending
        case alreadyDelivered
    }

    private(set) var isPending = false
    private(set) var isDeliveryConfirmed = false

    mutating func ensure() -> EnsureAction {
        if isDeliveryConfirmed { return .alreadyDelivered }
        if isPending { return .retryPending }
        isPending = true
        return .enqueue
    }

    /// Returns true only for the first accepted delivery.
    mutating func confirmDelivery() -> Bool {
        guard isPending, !isDeliveryConfirmed else { return false }
        isPending = false
        isDeliveryConfirmed = true
        return true
    }

    mutating func discardPending() {
        guard !isDeliveryConfirmed else { return }
        isPending = false
    }

}

/// Session-scoped gate for the stress-only outbound notification hold.
/// CoreBluetooth timing stays in `V1Peripheral`; this value only decides
/// whether a scheduled release still belongs to the active short subscription.
struct HandshakeNotificationHoldState {
    struct ScheduledRelease: Equatable {
        let epoch: UInt64
        let delayMilliseconds: Int
    }

    enum AcceptedStartAction: Equatable {
        case none
        case schedule(ScheduledRelease)
        case releaseHeldNotifications
    }

    static let upperBoundMilliseconds = 2_000

    private let delayMilliseconds: Int
    private var nextEpoch: UInt64 = 0
    private var activeEpoch: UInt64?
    private var startSeenEpoch: UInt64?
    private var heldEpoch: UInt64?

    init(delayMilliseconds: Int) {
        precondition(
            (0..<HandshakeNotificationHoldState.upperBoundMilliseconds)
                .contains(delayMilliseconds)
        )
        self.delayMilliseconds = delayMilliseconds
    }

    mutating func beginEpoch() {
        nextEpoch &+= 1
        activeEpoch = nextEpoch
        startSeenEpoch = nil
        heldEpoch = nil
    }

    mutating func endEpoch() {
        activeEpoch = nil
        startSeenEpoch = nil
        heldEpoch = nil
    }

    /// The first accepted START owned by the active subscriber creates the
    /// safety deadline. A second owned START releases the queued notifications
    /// immediately; later duplicates cannot restart the hold in this epoch.
    mutating func acceptedStart(
        belongsToActiveEpoch: Bool
    ) -> AcceptedStartAction {
        guard belongsToActiveEpoch, let epoch = activeEpoch else { return .none }
        if startSeenEpoch == epoch {
            guard heldEpoch == epoch else { return .none }
            heldEpoch = nil
            return .releaseHeldNotifications
        }
        startSeenEpoch = epoch
        guard delayMilliseconds > 0 else { return .none }
        heldEpoch = epoch
        return .schedule(
            ScheduledRelease(
                epoch: epoch,
                delayMilliseconds: delayMilliseconds
            )
        )
    }

    var blocksFlush: Bool {
        guard let activeEpoch else { return false }
        return heldEpoch == activeEpoch
    }

    /// Returns true only when the live epoch owns this exact scheduled release.
    /// Epoch end, replacement, or stop makes a stale timer a no-op.
    mutating func release(_ scheduled: ScheduledRelease) -> Bool {
        guard activeEpoch == scheduled.epoch,
              heldEpoch == scheduled.epoch else { return false }
        heldEpoch = nil
        return true
    }
}

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
//   6. B4E0 if it can notify                  — optional, reserved for packets
//                                               over the short-packet limit
// BCE0 is never used by the firmware but companion apps expect it to exist.
// =============================================================================

final class V1Peripheral: NSObject {

    struct Config {
        var localName: String = "V1G-REPLAY"
        var header: V1.Header = .v1ToApp
        /// Controls generated targeted responses; inbound checksums stay required.
        var checksum: Bool = true
        var version: String = "4.1038"
        var mode: V1.ModeGlyph = .advancedLogic
        var mainVolume: UInt8 = 4
        var mutedVolume: UInt8 = 0
        /// Nil means saved initially mirrors current.
        var savedMainVolume: UInt8?
        var savedMutedVolume: UInt8?
        var userBytes: [UInt8] = Array(repeating: 0, count: 6)
        var logPackets: Bool = false
        /// Optional bounded, anonymous startup-handshake evidence.
        var handshakeLedger: HandshakeLedger?
        /// Stress-only maximum hold after the first epoch-owned START. A second
        /// owned START releases sooner; zero preserves immediate notifications.
        var handshakeNotificationHoldMs: Int = 0
        /// Proxy mode publishes the service but holds off advertising until the
        /// real V1 is connected, so v1simple cannot win the race to it.
        var deferAdvertising: Bool = false
    }

    // Callbacks (always delivered on `queue`).
    var onLog: ((String) -> Void)?
    var onStateChange: (() -> Void)?
    var onStartAlertData: (() -> Void)?
    var onHandshakeClearDelivered: (() -> Void)?
    var onMuteCommand: ((Bool) -> Void)?
    var onDisplayPowerCommand: ((Bool) -> Void)?

    /// Proxy hook. Receives every inbound write before it is parsed, with the
    /// characteristic it arrived on preserved. Returning true suppresses the
    /// built-in emulator reply for that write. Nil in every non-proxy mode.
    var onRawWrite: ((CBUUID, [UInt8]) -> Bool)?

    /// Everything the console and player threads poll. CoreBluetooth callbacks
    /// land on `queue`; the status line reads from the main thread; the player
    /// polls `displaySubscribed` — so all of it lives behind one lock.
    private struct State {
        var isPoweredOn = false
        var isAdvertising = false
        var notifiesSent = 0
        var notifiesDropped = 0
        var commandsReceived = 0
        var session: V1.Session
    }

    private let stateLock = NSLock()
    private var state: State

    private func withState<T>(_ body: (inout State) -> T) -> T {
        stateLock.lock()
        defer { stateLock.unlock() }
        return body(&state)
    }

    var isPoweredOn: Bool { return withState { $0.isPoweredOn } }
    var isAdvertising: Bool { return withState { $0.isAdvertising } }
    var subscriberCount: Int { return withState { $0.session.subscriberCount } }
    var displaySubscribed: Bool {
        return withState { $0.session.readiness.displaySubscribed }
    }
    var longSubscribed: Bool {
        return withState { $0.session.readiness.longTrafficSubscribed }
    }
    var notifiesSent: Int { return withState { $0.notifiesSent } }
    var notifiesDropped: Int { return withState { $0.notifiesDropped } }
    var commandsReceived: Int { return withState { $0.commandsReceived } }
    var alertDataRequested: Bool { return withState { $0.session.alertDataRequested } }
    var controlState: V1.Session.ControlState {
        return withState { $0.session.controlState }
    }

    let queue = DispatchQueue(label: "com.v1simple.v1replay.ble")
    private let queueIdentityKey = DispatchSpecificKey<UInt8>()
    private let queueIdentityValue: UInt8 = 1

    private let config: Config
    private var manager: CBPeripheralManager!

    private var displayChar: CBMutableCharacteristic!   // B2CE
    private var longNotifyChar: CBMutableCharacteristic! // B4E0
    private var altNotifyChar: CBMutableCharacteristic! // BCE0
    private var commandChar: CBMutableCharacteristic!   // B6D4
    private var commandLongChar: CBMutableCharacteristic! // B8D2
    private var commandAltChar: CBMutableCharacteristic!  // BAD4

    private enum NotificationPurpose {
        case ordinary
        case handshakeClear
    }

    private struct PendingNotification {
        let data: Data
        let characteristic: CBMutableCharacteristic
        let handshakeEpoch: Int?
        let purpose: NotificationPurpose
    }

    private var pending: [PendingNotification] = []
    private static let pendingCap = 96
    private var lastValues: [CBUUID: Data] = [:]
    /// In-memory only. Bench evidence models one logical short-notify session;
    /// central identifiers are never written to the ledger.
    private var shortSubscriberIDs: Set<UUID> = []
    private var handshakeSubscriberID: UUID?
    private var handshakeClearDelivery = HandshakeClearDeliveryState()
    private var serviceAdded = false
    private var isStopping = false
    private let handshakeLedger: HandshakeLedger?
    private var handshakeNotificationHold: HandshakeNotificationHoldState

    init(config: Config) {
        self.config = config
        self.handshakeLedger = config.handshakeLedger
        self.handshakeNotificationHold = HandshakeNotificationHoldState(
            delayMilliseconds: config.handshakeNotificationHoldMs
        )
        var sessionConfig = V1.Session.Config()
        sessionConfig.header = config.header
        sessionConfig.outboundChecksum = config.checksum
        sessionConfig.version = config.version
        sessionConfig.mode = config.mode
        sessionConfig.mainVolume = config.mainVolume
        sessionConfig.mutedVolume = config.mutedVolume
        sessionConfig.savedMainVolume = config.savedMainVolume
        sessionConfig.savedMutedVolume = config.savedMutedVolume
        sessionConfig.userBytes = config.userBytes
        self.state = State(session: V1.Session(config: sessionConfig))
        super.init()
        queue.setSpecific(key: queueIdentityKey, value: queueIdentityValue)
        manager = CBPeripheralManager(delegate: self, queue: queue, options: nil)
    }

    // MARK: - Transmission

    func sendDisplay(_ bytes: [UInt8]) {
        send(bytes, to: displayChar)
    }

    func sendLong(_ bytes: [UInt8]) {
        send(bytes, to: longNotifyChar)
    }

    /// Mirror a notification from the real V1 onto the matching characteristic.
    func forward(_ bytes: [UInt8], toCharacteristicUUID uuid: CBUUID) {
        if uuid == CBUUID(string: V1.displayShortUUID) {
            send(bytes, to: displayChar)
        } else if uuid == CBUUID(string: V1.displayLongUUID) {
            send(bytes, to: longNotifyChar)
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
            guard !self.isStopping else { return }
            self.lastValues[characteristic.uuid] = data
            self.appendPending(PendingNotification(
                data: data,
                characteristic: characteristic,
                handshakeEpoch: self.handshakeLedger?.activeEpoch,
                purpose: .ordinary
            ))
            self.flush()
            if self.config.logPackets {
                self.onLog?("TX \(Self.shortName(characteristic.uuid)) \(bytes.hexString)")
            }
        }
    }

    /// Ensure handshake-only mode has one canonical clear row pending. Calls
    /// from the start-request callback run inline on the BLE queue, ahead of a
    /// later write callback. Calls from the Player polling fallback dispatch
    /// onto that same queue. Repeated calls retry without appending a duplicate.
    func ensureHandshakeClear(_ bytes: [UInt8]) {
        let data = Data(bytes)
        let ensure = {
            guard !self.isStopping,
                  self.handshakeSubscriberID != nil,
                  let characteristic = self.displayChar else { return }
            switch self.handshakeClearDelivery.ensure() {
            case .alreadyDelivered:
                return
            case .retryPending:
                break
            case .enqueue:
                self.lastValues[characteristic.uuid] = data
                self.appendPending(PendingNotification(
                    data: data,
                    characteristic: characteristic,
                    handshakeEpoch: self.handshakeLedger?.activeEpoch,
                    purpose: .handshakeClear
                ))
                if self.config.logPackets {
                    self.onLog?("TX \(Self.shortName(characteristic.uuid)) \(bytes.hexString)")
                }
            }
            self.flush()
        }
        if DispatchQueue.getSpecific(key: queueIdentityKey) == queueIdentityValue {
            ensure()
        } else {
            queue.async(execute: ensure)
        }
    }

    private func appendPending(_ notification: PendingNotification) {
        if pending.count >= V1Peripheral.pendingCap {
            let dropped = pending.removeFirst()
            if dropped.purpose == .handshakeClear {
                handshakeClearDelivery.discardPending()
            }
            withState { $0.notifiesDropped += 1 }
        }
        pending.append(notification)
    }

    private func discardPendingHandshakeClear() {
        pending.removeAll { $0.purpose == .handshakeClear }
        handshakeClearDelivery.discardPending()
    }

    private func endHandshakeEpoch() {
        handshakeSubscriberID = nil
        handshakeNotificationHold.endEpoch()
        discardPendingHandshakeClear()
        handshakeLedger?.endEpoch()
    }

    private func send(_ decision: V1.ReplyDecision) {
        switch decision.channel {
        case .displayShort:
            send(decision.bytes, to: displayChar)
        case .displayLong:
            send(decision.bytes, to: longNotifyChar)
        }
    }

    /// Drain the notify queue until CoreBluetooth pushes back, then wait for
    /// `peripheralManagerIsReady(toUpdateSubscribers:)`.
    private func flush() {
        guard !isStopping, !handshakeNotificationHold.blocksFlush else { return }
        while let item = pending.first {
            guard manager.updateValue(
                item.data,
                for: item.characteristic,
                onSubscribedCentrals: nil
            ) else { return }
            pending.removeFirst()
            withState { $0.notifiesSent += 1 }
            let firstHandshakeClearDelivery = item.purpose == .handshakeClear
                && handshakeClearDelivery.confirmDelivery()
            handshakeLedger?.recordDelivered(
                bytes: [UInt8](item.data),
                channel: Self.shortName(item.characteristic.uuid),
                epoch: item.handshakeEpoch
            )
            if firstHandshakeClearDelivery {
                onHandshakeClearDelivered?()
            }
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
        longNotifyChar = notifyCharacteristic(V1.displayLongUUID)
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
            displayChar, longNotifyChar, commandChar,
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
            isStopping = true
            pending.removeAll()
            handshakeClearDelivery.discardPending()
            if manager.isAdvertising { manager.stopAdvertising() }
            manager.removeAllServices()
            shortSubscriberIDs.removeAll()
            endHandshakeEpoch()
        }
    }

    // MARK: - Command handling

    private func handle(_ outcome: V1.Session.CommandOutcome,
                        inboundCharacteristic: CBUUID,
                        belongsToHandshakeSubscriber: Bool) {
        withState { $0.commandsReceived += 1 }
        let packet = outcome.packet
        let commandName = V1.name(forPacketID: packet.id)
        if config.logPackets {
            onLog?("RX \(commandName) \(packet.raw.hexString)")
        }

        let accepted = !outcome.effects.contains { effect in
            if case .rejected = effect { return true }
            if case .unhandled = effect { return true }
            return false
        }
        if accepted {
            handshakeLedger?.recordAcceptedRequest(
                bytes: packet.raw,
                channel: Self.shortName(inboundCharacteristic),
                belongsToEpochSubscriber: belongsToHandshakeSubscriber
            )
            if packet.id == V1.PacketID.reqStartAlertData.rawValue {
                switch handshakeNotificationHold.acceptedStart(
                    belongsToActiveEpoch: belongsToHandshakeSubscriber
                ) {
                case .none:
                    break
                case .schedule(let scheduled):
                    queue.asyncAfter(
                        deadline: .now() + .milliseconds(scheduled.delayMilliseconds)
                    ) { [weak self] in
                        guard let self = self,
                              !self.isStopping,
                              self.handshakeNotificationHold.release(scheduled) else {
                            return
                        }
                        self.flush()
                    }
                case .releaseHeldNotifications:
                    self.flush()
                }
            }
        }

        for effect in outcome.effects {
            switch effect {
            case .reply(let decision):
                let responseName = decision.bytes.count > 3
                    ? V1.name(forPacketID: decision.bytes[3])
                    : "response"
                onLog?("→ \(responseName)")
                send(decision)

            case .alertDataChanged(true):
                onLog?("← reqStartAlertData — alert table enabled")
                onStartAlertData?()
                onStateChange?()

            case .alertDataChanged(false):
                onLog?("← reqStopAlertData")
                onStateChange?()

            case .muteChanged(let muted):
                onMuteCommand?(muted)

            case .displayPowerChanged(let enabled):
                onDisplayPowerCommand?(enabled)

            case .userBytesStored(let bytes):
                onLog?("← stored user bytes \(bytes.hexString)")

            case .modeChanged(let mode):
                onLog?("← mode changed to \(mode)")
                onStateChange?()

            case .volumeChanged(let control):
                onLog?("← volume changed to \(control.mainVolume)/\(control.mutedVolume)")
                onStateChange?()

            case .rejected(let reason):
                onLog?("← rejected \(commandName): \(reason)")

            case .unhandled:
                onLog?("← unhandled \(commandName)")
            }
        }
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

    /// Pure ownership check for anonymous handshake evidence. The identifier
    /// remains process-local and is never passed to the ledger or serialized.
    static func handshakeEvidenceOwnerMatches(subscriber: UUID?, writer: UUID) -> Bool {
        guard let subscriber else { return false }
        return subscriber == writer
    }

    private static func sessionChannel(
        for uuid: CBUUID
    ) -> V1.Session.SubscriptionChannel? {
        if uuid == CBUUID(string: V1.displayShortUUID) { return .displayShort }
        if uuid == CBUUID(string: V1.displayLongUUID) { return .displayLong }
        if uuid == CBUUID(string: V1.notifyAltUUID) { return .compatibilityNotify }
        return nil
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
            shortSubscriberIDs.removeAll()
            endHandshakeEpoch()
            onLog?("Bluetooth is off — turn it on to advertise")
        case .unauthorized:
            withState { $0.isPoweredOn = false }
            shortSubscriberIDs.removeAll()
            endHandshakeEpoch()
            onLog?("Bluetooth permission denied. System Settings → Privacy & Security → Bluetooth, "
                   + "and enable the terminal application that launched v1replay.")
        case .unsupported:
            withState { $0.isPoweredOn = false }
            shortSubscriberIDs.removeAll()
            endHandshakeEpoch()
            onLog?("This Mac reports no BLE peripheral support")
        default:
            withState { $0.isPoweredOn = false }
            shortSubscriberIDs.removeAll()
            endHandshakeEpoch()
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
        var beganShortSession = false
        var addedSecondShortSubscriber = false
        if let channel = V1Peripheral.sessionChannel(for: characteristic.uuid) {
            withState {
                $0.session.subscribe(central: central.identifier, channel: channel)
            }
            if channel == .displayShort {
                let inserted = shortSubscriberIDs.insert(central.identifier).inserted
                beganShortSession = inserted && shortSubscriberIDs.count == 1
                addedSecondShortSubscriber = inserted && shortSubscriberIDs.count > 1
            }
        }
        if beganShortSession {
            discardPendingHandshakeClear()
            handshakeSubscriberID = central.identifier
            handshakeLedger?.beginEpoch()
            handshakeNotificationHold.beginEpoch()
        } else if addedSecondShortSubscriber {
            // Notification delivery is broadcast, so attribution becomes
            // ambiguous until every short subscriber leaves.
            endHandshakeEpoch()
        }
        onLog?("Central subscribed to \(name) (MTU \(central.maximumUpdateValueLength))")
        onStateChange?()
    }

    func peripheralManager(_ peripheral: CBPeripheralManager,
                           central: CBCentral,
                           didUnsubscribeFrom characteristic: CBCharacteristic) {
        let name = V1Peripheral.shortName(characteristic.uuid)
        let remaining: Int
        var endedShortSession = false
        if let channel = V1Peripheral.sessionChannel(for: characteristic.uuid) {
            remaining = withState {
                let count = $0.session.unsubscribe(
                    central: central.identifier,
                    channel: channel
                )
                return count
            }
            if channel == .displayShort {
                let removed = shortSubscriberIDs.remove(central.identifier) != nil
                endedShortSession = removed && shortSubscriberIDs.isEmpty
            }
        } else {
            remaining = subscriberCount
        }
        onLog?("Central unsubscribed from \(name)")
        if remaining == 0 {
            pending.removeAll()
        } else if endedShortSession {
            pending.removeAll {
                $0.characteristic.uuid == CBUUID(string: V1.displayShortUUID)
            }
        }
        if endedShortSession || remaining == 0 {
            endHandshakeEpoch()
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
            let belongsToHandshakeSubscriber = Self.handshakeEvidenceOwnerMatches(
                subscriber: handshakeSubscriberID,
                writer: request.central.identifier
            )
            if !belongsToHandshakeSubscriber {
                // Fail closed before appending bytes: otherwise fragments from
                // two centrals could be reassembled into one credited request.
                endHandshakeEpoch()
            }
            withState { $0.session.append([UInt8](value)) }
            while let outcome = withState({ $0.session.nextOutcome() }) {
                handle(
                    outcome,
                    inboundCharacteristic: request.characteristic.uuid,
                    belongsToHandshakeSubscriber: belongsToHandshakeSubscriber
                )
            }
        }
        onStateChange?()

        if let first = requests.first {
            peripheral.respond(to: first, withResult: .success)
        }
    }
}

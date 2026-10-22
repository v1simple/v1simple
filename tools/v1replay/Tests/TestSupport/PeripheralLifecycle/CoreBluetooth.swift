// Software-only CoreBluetooth surface for production replay transport callbacks.
// This module shadows the framework only in test_peripheral_lifecycle.py's
// isolated compiler invocation. No Bluetooth manager or radio is opened.
import Foundation

public let CBAdvertisementDataLocalNameKey = "localName"
public let CBAdvertisementDataServiceUUIDsKey = "serviceUUIDs"
public enum CBManagerState: Int { case unknown, resetting, unsupported, unauthorized, poweredOff, poweredOn }
public enum CBATTError { public enum Code { case invalidOffset, success } }
public struct CBCharacteristicProperties: OptionSet {
    public let rawValue: Int
    public init(rawValue: Int) { self.rawValue = rawValue }
    public static let read = Self(rawValue: 1)
    public static let notify = Self(rawValue: 2)
    public static let write = Self(rawValue: 4)
    public static let writeWithoutResponse = Self(rawValue: 8)
}
public struct CBAttributePermissions: OptionSet {
    public let rawValue: Int
    public init(rawValue: Int) { self.rawValue = rawValue }
    public static let readable = Self(rawValue: 1)
    public static let writeable = Self(rawValue: 2)
}
public final class CBUUID: Hashable {
    public let uuidString: String
    public init(string: String) { uuidString = string.uppercased() }
    public static func == (a: CBUUID, b: CBUUID) -> Bool { a.uuidString == b.uuidString }
    public func hash(into hasher: inout Hasher) { hasher.combine(uuidString) }
}
open class CBCharacteristic {
    public let uuid: CBUUID
    public let properties: CBCharacteristicProperties
    public var value: Data?
    public var isNotifying = false
    public init(type: CBUUID, properties: CBCharacteristicProperties,
                value: Data?, permissions: CBAttributePermissions) {
        uuid = type; self.properties = properties; self.value = value
    }
}
public final class CBMutableCharacteristic: CBCharacteristic {}
open class CBService {
    public let uuid: CBUUID
    public var characteristics: [CBCharacteristic]?
    public init(type: CBUUID, primary: Bool) { uuid = type }
}
public final class CBMutableService: CBService {}
public enum CBCharacteristicWriteType { case withoutResponse, withResponse }
public protocol CBPeripheralDelegate: AnyObject {}
public final class CBPeripheral {
    public weak var delegate: CBPeripheralDelegate?
    public var name: String? = "V1-test"
    public let identifier = UUID()
    public var services: [CBService]?
    public var serviceRequests = 0
    public var characteristicRequests = 0
    public var notificationRequests: [CBCharacteristic] = []
    public init() {}
    public func discoverServices(_ values: [CBUUID]?) { serviceRequests += 1 }
    public func discoverCharacteristics(_ values: [CBUUID]?, for service: CBService) {
        characteristicRequests += 1
    }
    public func setNotifyValue(_ value: Bool, for characteristic: CBCharacteristic) {
        if value { notificationRequests.append(characteristic) }
    }
    public func writeValue(_ data: Data, for characteristic: CBCharacteristic,
                           type: CBCharacteristicWriteType) {}
}
public protocol CBCentralManagerDelegate: AnyObject {
    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral,
                        error: Error?)
}
public final class CBCentralManager {
    public static var latest: CBCentralManager!
    public weak var delegate: CBCentralManagerDelegate?
    public var state: CBManagerState = .poweredOn
    public var isScanning = false
    public var scans = 0
    public var connections: [CBPeripheral] = []
    public var cancellations: [CBPeripheral] = []
    private var pendingCancellations: [CBPeripheral] = []
    public init(delegate: CBCentralManagerDelegate?, queue: DispatchQueue?) {
        self.delegate = delegate; Self.latest = self
    }
    public func scanForPeripherals(withServices: [CBUUID]?, options: [String: Any]?) {
        scans += 1; isScanning = true
    }
    public func stopScan() { isScanning = false }
    public func connect(_ peripheral: CBPeripheral, options: [String: Any]?) {
        connections.append(peripheral)
    }
    public func cancelPeripheralConnection(_ peripheral: CBPeripheral) {
        cancellations.append(peripheral)
        if !pendingCancellations.contains(where: { $0 === peripheral }) {
            pendingCancellations.append(peripheral)
        }
    }
    // Cancellation completion is a separate delegate event. An ordinary
    // request error does not synthesize a disconnect by itself.
    public func completeCancellation() {
        guard !pendingCancellations.isEmpty else { return }
        let peripheral = pendingCancellations.removeFirst()
        delegate?.centralManager(self, didDisconnectPeripheral: peripheral, error: nil)
    }
}
public final class CBCentral {
    public let identifier = UUID()
    public let maximumUpdateValueLength = 20
    public init() {}
}
public final class CBATTRequest {
    public let characteristic: CBCharacteristic
    public let central: CBCentral
    public var value: Data?
    public var offset = 0
    public init(characteristic: CBCharacteristic, central: CBCentral, value: Data?) {
        self.characteristic = characteristic; self.central = central; self.value = value
    }
}
public protocol CBPeripheralManagerDelegate: AnyObject {}
public final class CBPeripheralManager {
    public struct Notification {
        public let bytes: [UInt8]
        public let characteristic: CBUUID
        public let recipients: Set<UUID>
    }
    public static var latest: CBPeripheralManager!
    public var state: CBManagerState = .unknown
    public var isAdvertising = false
    public var services: [CBMutableService] = []
    public var subscribers: [CBUUID: Set<UUID>] = [:]
    public var acceptsNotifications = true
    public var notifications: [Notification] = []
    public var responses: [CBATTRequest] = []
    public var invalidOperations: [String] = []
    public var addCount = 0
    public var startCount = 0
    private var advertisingPaused = false
    public init(delegate: CBPeripheralManagerDelegate?, queue: DispatchQueue?, options: [String: Any]?) {
        Self.latest = self
    }
    // CBPeripheralManagerDelegate's SDK contract: below poweredOn disconnects
    // centrals; below poweredOff also clears services and stops advertising.
    public func transition(to newState: CBManagerState) {
        if newState.rawValue < CBManagerState.poweredOff.rawValue {
            services.removeAll()
            advertisingPaused = false
        } else if newState == .poweredOff {
            advertisingPaused = isAdvertising
        }
        state = newState
        isAdvertising = newState == .poweredOn && advertisingPaused
        if newState != .poweredOn { subscribers.removeAll() }
    }
    public func add(_ service: CBMutableService) {
        if state != .poweredOn { invalidOperations.append("add while unavailable") }
        services.append(service); addCount += 1
    }
    public func startAdvertising(_ data: [String: Any]) {
        if state != .poweredOn { invalidOperations.append("advertise while unavailable") }
        isAdvertising = true; startCount += 1
    }
    public func stopAdvertising() { isAdvertising = false; advertisingPaused = false }
    public func removeAllServices() { services.removeAll() }
    public func respond(to: CBATTRequest, withResult: CBATTError.Code) { responses.append(to) }
    public func updateValue(_ data: Data, for characteristic: CBMutableCharacteristic,
                            onSubscribedCentrals: [CBCentral]?) -> Bool {
        if state != .poweredOn { invalidOperations.append("notify while unavailable") }
        if !services.contains(where: { service in
            service.characteristics?.contains(where: { $0 === characteristic }) == true
        }) { invalidOperations.append("notify on obsolete characteristic") }
        guard acceptsNotifications else { return false }
        notifications.append(Notification(bytes: [UInt8](data), characteristic: characteristic.uuid,
                                          recipients: subscribers[characteristic.uuid] ?? []))
        return true
    }
}

// Software-only CoreBluetooth surface for the production peripheral callbacks.
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
    public init(type: CBUUID, properties: CBCharacteristicProperties,
                value: Data?, permissions: CBAttributePermissions) { uuid = type }
}
public final class CBMutableCharacteristic: CBCharacteristic {}
open class CBService {
    public let uuid: CBUUID
    public var characteristics: [CBCharacteristic]?
    public init(type: CBUUID, primary: Bool) { uuid = type }
}
public final class CBMutableService: CBService {}
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
    public func respond(to: CBATTRequest, withResult: CBATTError.Code) {}
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

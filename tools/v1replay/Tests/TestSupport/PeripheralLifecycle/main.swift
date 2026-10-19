import Foundation
import CoreBluetooth
import XCTest

private final class Fixture {
    let peripheral: V1Peripheral
    let manager: CBPeripheralManager
    let first = CBCentral()
    let second = CBCentral()

    init(deferred: Bool = false, publish: Bool = true) {
        var config = V1Peripheral.Config()
        config.deferAdvertising = deferred
        peripheral = V1Peripheral(config: config)
        manager = CBPeripheralManager.latest!
        peripheral.queue.sync {
            transition(.poweredOn)
            if publish { completePublication() }
        }
    }

    func transition(_ state: CBManagerState) {
        manager.transition(to: state)
        peripheral.peripheralManagerDidUpdateState(manager)
    }

    func completePublication() {
        guard let service = manager.services.last else {
            XCTFail("powered-on peripheral did not add its service")
            return
        }
        peripheral.peripheralManager(manager, didAdd: service, error: nil)
        if manager.isAdvertising {
            peripheral.peripheralManagerDidStartAdvertising(manager, error: nil)
        }
    }

    func characteristic(_ uuid: String) -> CBCharacteristic {
        manager.services.last!.characteristics!.first { $0.uuid == CBUUID(string: uuid) }!
    }

    func subscribe(_ central: CBCentral, to uuid: String = V1.displayShortUUID) {
        let characteristic = characteristic(uuid)
        manager.subscribers[characteristic.uuid, default: []].insert(central.identifier)
        peripheral.peripheralManager(manager, central: central, didSubscribeTo: characteristic)
    }

    func unsubscribe(_ central: CBCentral, from uuid: String = V1.displayShortUUID) {
        let characteristic = characteristic(uuid)
        manager.subscribers[characteristic.uuid]?.remove(central.identifier)
        peripheral.peripheralManager(manager, central: central, didUnsubscribeFrom: characteristic)
    }

    func write(_ central: CBCentral, id: UInt8, payload: [UInt8] = []) {
        let bytes = V1.frame(header: .init(dest: 0xDA, src: 0xE6), id: id, payload: payload)
        peripheral.peripheralManager(manager, didReceiveWrite: [CBATTRequest(
            characteristic: characteristic(V1.commandUUID), central: central, value: Data(bytes)
        )])
    }

    func received(_ central: CBCentral) -> [[UInt8]] {
        manager.notifications.filter { $0.recipients.contains(central.identifier) }.map(\.bytes)
    }

    // Every callback is queued before the first one can run, reproducing a
    // legal delegate backlog rather than racing unsynchronized test mutation.
    func backlog(_ callbacks: [() -> Void]) {
        blocked {
            for callback in callbacks { peripheral.queue.async(execute: callback) }
        }
    }

    func blocked(_ enqueue: () -> Void) {
        let release = DispatchSemaphore(value: 0)
        peripheral.queue.async { release.wait() }
        enqueue()
        release.signal()
        drain()
    }

    func drain() {
        peripheral.queue.sync {}
        // Also drain work deferred by a callback (the original failure).
        peripheral.queue.sync {}
    }
}

final class PeripheralLifecycleTests: XCTestCase {
    @objc func testReplyCannotCrossQueuedLastUnsubscribeAndNewOwner() {
        let f = Fixture()
        f.peripheral.queue.sync { f.subscribe(f.first) }
        f.backlog([
            { f.write(f.first, id: 0x3C) },
            { f.unsubscribe(f.first) },
            { f.subscribe(f.second) },
            { f.write(f.second, id: 0x39, payload: [9, 0, 0]) },
            { f.write(f.second, id: 0x3C) },
        ])
        XCTAssertEqual(f.received(f.first).map { $0[5] }, [4])
        XCTAssertEqual(f.received(f.second).map { $0[5] }, [9])
        XCTAssertEqual(f.peripheral.controlState.mainVolume, 9)
    }

    @objc func testBackpressuredOldReplyIsDiscardedAtLastUnsubscribe() {
        let f = Fixture()
        f.peripheral.queue.sync {
            f.subscribe(f.first)
            f.manager.acceptsNotifications = false
        }
        f.backlog([
            { f.write(f.first, id: 0x3C) },
            { f.unsubscribe(f.first) },
            { f.subscribe(f.second); f.manager.acceptsNotifications = true },
            { f.write(f.second, id: 0x39, payload: [9, 0, 0]); f.write(f.second, id: 0x3C) },
            { f.peripheral.peripheralManagerIsReady(toUpdateSubscribers: f.manager) },
        ])
        XCTAssertEqual(f.received(f.second).map { $0[5] }, [9])
    }

    @objc func testExternalSendsCaptureLifetimeBeforeQueueHandoff() {
        let f = Fixture()
        f.peripheral.queue.sync { f.subscribe(f.first) }
        f.blocked {
            f.peripheral.queue.async {
                f.unsubscribe(f.first)
                f.subscribe(f.second)
                f.subscribe(f.second, to: V1.displayLongUUID)
            }
            f.peripheral.sendDisplay([1])
            f.peripheral.sendLong([2])
            f.peripheral.ensureHandshakeClear([3])
        }
        XCTAssertEqual(f.received(f.second), [])
        f.peripheral.sendDisplay([4])
        f.peripheral.sendLong([5])
        f.drain()
        XCTAssertEqual(f.received(f.second), [[4], [5]])
    }

    @objc func testExternalSendCannotRetainResetCharacteristic() {
        let f = Fixture()
        f.peripheral.queue.sync { f.subscribe(f.first) }
        f.blocked {
            f.peripheral.queue.async {
                f.transition(.resetting)
                f.transition(.poweredOn)
                f.completePublication()
                if !f.manager.services.isEmpty { f.subscribe(f.second) }
            }
            f.peripheral.sendDisplay([1])
        }
        XCTAssertEqual(f.manager.notifications.count, 0)
        XCTAssertEqual(f.manager.invalidOperations, [])
        f.peripheral.sendDisplay([2])
        f.drain()
        XCTAssertEqual(f.received(f.second), [[2]])
    }

    @objc func testShortOwnerReplacementDoesNotInvalidateRemainingLongOwner() {
        let f = Fixture()
        f.peripheral.queue.sync {
            f.subscribe(f.first)
            f.subscribe(f.first, to: V1.displayLongUUID)
        }
        f.blocked {
            f.peripheral.queue.async {
                f.unsubscribe(f.first)
                f.subscribe(f.second)
            }
            f.peripheral.sendDisplay([1])
            f.peripheral.ensureHandshakeClear([2])
            f.peripheral.sendLong([3])
        }
        XCTAssertEqual(f.received(f.second), [])
        XCTAssertEqual(f.received(f.first), [[3]])
        f.peripheral.sendDisplay([4])
        f.drain()
        XCTAssertEqual(f.received(f.second), [[4]])
    }

    @objc func testOtherChannelReplacementRejectsQueuedSendsAndKeepsShortOwner() {
        for uuid in [V1.displayLongUUID, V1.notifyAltUUID] {
            let f = Fixture()
            f.peripheral.queue.sync {
                f.subscribe(f.first)
                f.subscribe(f.first, to: uuid)
            }
            f.blocked {
                f.peripheral.queue.async {
                    f.unsubscribe(f.first, from: uuid)
                    f.subscribe(f.second, to: uuid)
                }
                f.peripheral.forward([1], toCharacteristicUUID: CBUUID(string: uuid))
                f.peripheral.sendDisplay([2])
            }
            XCTAssertEqual(f.received(f.second), [], "channel: \(uuid)")
            XCTAssertEqual(f.received(f.first), [[2]], "keep the remaining short owner")
            f.peripheral.forward([3], toCharacteristicUUID: CBUUID(string: uuid))
            f.drain()
            XCTAssertEqual(f.received(f.second), [[3]], "the replacement can receive new work")
        }
    }

    @objc func testOtherChannelReplacementDiscardsBackpressureAndKeepsShortPending() {
        for uuid in [V1.displayLongUUID, V1.notifyAltUUID] {
            let f = Fixture()
            f.peripheral.queue.sync {
                f.subscribe(f.first)
                f.subscribe(f.first, to: uuid)
                f.manager.acceptsNotifications = false
                f.peripheral.forward([1], toCharacteristicUUID: CBUUID(string: uuid))
                f.peripheral.sendDisplay([2])
                f.unsubscribe(f.first, from: uuid)
                f.subscribe(f.second, to: uuid)
                f.manager.acceptsNotifications = true
                f.peripheral.peripheralManagerIsReady(toUpdateSubscribers: f.manager)
            }
            XCTAssertEqual(f.received(f.second), [], "channel: \(uuid)")
            XCTAssertEqual(f.received(f.first), [[2]], "keep the remaining short owner's retry")
        }
    }

    @objc func testRemainingSubscriberKeepsItsChannelGenerationAndBackpressure() {
        for uuid in [V1.displayShortUUID, V1.displayLongUUID, V1.notifyAltUUID] {
            let f = Fixture()
            f.peripheral.queue.sync {
                f.subscribe(f.first, to: uuid)
                f.subscribe(f.second, to: uuid)
                f.manager.acceptsNotifications = false
                f.peripheral.forward([1], toCharacteristicUUID: CBUUID(string: uuid))
            }
            f.blocked {
                f.peripheral.queue.async {
                    f.unsubscribe(f.first, from: uuid)
                    f.unsubscribe(f.first, from: uuid) // duplicate callback cannot end a live channel
                }
                f.peripheral.forward([2], toCharacteristicUUID: CBUUID(string: uuid))
            }
            f.peripheral.queue.sync {
                f.manager.acceptsNotifications = true
                f.peripheral.peripheralManagerIsReady(toUpdateSubscribers: f.manager)
            }
            XCTAssertEqual(f.received(f.first), [])
            XCTAssertEqual(f.received(f.second), [[1], [2]], "channel: \(uuid)")
        }
    }

    @objc func testSameLifetimePlaybackAndProxyNotificationsRetainOrderAndRetry() {
        let f = Fixture()
        f.peripheral.queue.sync {
            for uuid in [V1.displayShortUUID, V1.displayLongUUID, V1.notifyAltUUID] {
                f.subscribe(f.first, to: uuid)
            }
            f.manager.acceptsNotifications = false
        }
        f.peripheral.sendDisplay([1])
        f.peripheral.sendLong([2])
        f.peripheral.forward([3], toCharacteristicUUID: CBUUID(string: V1.notifyAltUUID))
        f.drain()
        XCTAssertEqual(f.received(f.first), [])
        f.peripheral.queue.sync {
            f.manager.acceptsNotifications = true
            f.peripheral.peripheralManagerIsReady(toUpdateSubscribers: f.manager)
        }
        XCTAssertEqual(f.received(f.first), [[1], [2], [3]])
        XCTAssertEqual(f.manager.notifications.map { $0.characteristic.uuidString },
                       [V1.displayShortUUID, V1.displayLongUUID, V1.notifyAltUUID].map { $0.uppercased() })
        XCTAssertEqual(f.manager.invalidOperations, [])
    }

    @objc func testHandshakeAndProxyForwardKeepInlineOrderingAndRetry() {
        let f = Fixture()
        var confirmed = 0
        f.peripheral.queue.sync {
            f.subscribe(f.first)
            f.peripheral.onHandshakeClearDelivered = { confirmed += 1 }
            f.peripheral.onStartAlertData = { f.peripheral.ensureHandshakeClear([7]) }
            f.manager.acceptsNotifications = false
            f.write(f.first, id: 0x41)
            f.peripheral.ensureHandshakeClear([7])
            f.manager.acceptsNotifications = true
            f.peripheral.peripheralManagerIsReady(toUpdateSubscribers: f.manager)
            f.peripheral.ensureHandshakeClear([7])
            f.peripheral.forward([8], toCharacteristicUUID: CBUUID(string: V1.displayShortUUID))
            f.unsubscribe(f.first)
            f.subscribe(f.second)
        }
        f.drain()
        XCTAssertEqual(confirmed, 1)
        XCTAssertEqual(f.received(f.first), [[7], [8]])
        XCTAssertEqual(f.received(f.second), [])
    }

    @objc func testDatabaseClearingStatesRepublishAndRestartAdvertising() {
        for state in [CBManagerState.unknown, .resetting, .unsupported, .unauthorized] {
            let f = Fixture()
            f.peripheral.queue.sync {
                f.subscribe(f.first)
                f.write(f.first, id: 0x39, payload: [9, 0, 0])
                f.transition(state)
                XCTAssertFalse(f.peripheral.isPoweredOn)
                XCTAssertFalse(f.peripheral.isAdvertising)
                XCTAssertEqual(f.peripheral.subscriberCount, 0)
                f.transition(.poweredOn)
                XCTAssertEqual(f.manager.addCount, 2, "state: \(state)")
                XCTAssertEqual(f.manager.startCount, 1, "wait for didAdd before advertising")
                f.completePublication()
                XCTAssertEqual(f.manager.startCount, 2, "state: \(state)")
                XCTAssertTrue(f.peripheral.isAdvertising)
                XCTAssertEqual(f.peripheral.controlState.mainVolume, 9)
                XCTAssertEqual(f.manager.invalidOperations, [])
            }
        }
    }

    @objc func testPoweredOffPreservesDatabaseAndResumesAdvertising() {
        let f = Fixture()
        f.peripheral.queue.sync {
            let original = f.manager.services[0]
            f.transition(.poweredOff)
            f.transition(.poweredOn)
            XCTAssertTrue(f.manager.services[0] === original)
            XCTAssertEqual(f.manager.addCount, 1)
            XCTAssertEqual(f.manager.startCount, 1)
            XCTAssertTrue(f.peripheral.isAdvertising)
            XCTAssertEqual(f.manager.invalidOperations, [])
        }
    }

    @objc func testPublicationCompletingWhilePoweredOffCanAdvertiseAfterResume() {
        for deferred in [false, true] {
            let f = Fixture(deferred: deferred, publish: false)
            f.peripheral.queue.sync {
                let service = f.manager.services[0]
                f.transition(.poweredOff)
                f.peripheral.peripheralManager(f.manager, didAdd: service, error: nil)
                XCTAssertEqual(f.manager.startCount, 0, "cannot advertise while powered off")
                f.transition(.poweredOn)
                XCTAssertEqual(f.manager.addCount, 1, "normal power-off keeps the database")
                XCTAssertEqual(f.manager.startCount, deferred ? 0 : 1)
                if deferred {
                    f.peripheral.beginAdvertising()
                    XCTAssertEqual(f.manager.startCount, 1, "retain deferred advertising ownership")
                }
                XCTAssertEqual(f.manager.invalidOperations, [])
            }
        }
    }

    @objc func testPublicationFailingWhilePoweredOffCanRetryAfterResume() {
        let f = Fixture(publish: false)
        f.peripheral.queue.sync {
            let service = f.manager.services[0]
            f.transition(.poweredOff)
            f.peripheral.peripheralManager(f.manager, didAdd: service,
                                          error: NSError(domain: "PublicationTest", code: 1))
            XCTAssertEqual(f.manager.addCount, 1, "cannot retry while powered off")
            f.transition(.poweredOn)
            XCTAssertEqual(f.manager.addCount, 2, "failed registration must be retried")
            f.completePublication()
            XCTAssertEqual(f.manager.startCount, 1)
            XCTAssertEqual(f.manager.invalidOperations, [])
        }
    }

    @objc func testDeferredAdvertisingWaitsForRequestAndPublicationAfterReset() {
        let f = Fixture(deferred: true)
        XCTAssertEqual(f.manager.startCount, 0)
        f.peripheral.beginAdvertising()
        f.drain()
        XCTAssertEqual(f.manager.startCount, 1)
        f.peripheral.queue.sync {
            f.transition(.resetting)
            f.transition(.poweredOn)
            f.completePublication()
        }
        XCTAssertEqual(f.manager.startCount, 1, "reset must await the proxy's new connection")
        f.peripheral.beginAdvertising()
        f.drain()
        XCTAssertEqual(f.manager.startCount, 2)
        XCTAssertEqual(f.manager.invalidOperations, [])

        let early = Fixture(deferred: true, publish: false)
        early.peripheral.beginAdvertising()
        early.drain()
        XCTAssertEqual(early.manager.startCount, 0, "service publication is still pending")
        early.peripheral.queue.sync { early.completePublication() }
        XCTAssertEqual(early.manager.startCount, 1, "retain the request until publication")

        let held = Fixture(deferred: true)
        held.peripheral.queue.sync {
            held.transition(.resetting)
            held.transition(.poweredOn)
            held.completePublication()
        }
        XCTAssertEqual(held.manager.startCount, 0, "reset cannot authorize a held proxy")

        held.blocked {
            held.peripheral.queue.async {
                held.transition(.resetting)
                held.transition(.poweredOn)
                held.completePublication()
            }
            held.peripheral.beginAdvertising()
        }
        XCTAssertEqual(held.manager.startCount, 0, "an older queued request cannot authorize recovery")
    }

    @objc func testUnavailableAndStoppedManagersRejectDelayedWork() {
        let f = Fixture(deferred: true)
        f.peripheral.queue.sync { f.transition(.resetting) }
        f.peripheral.beginAdvertising()
        f.peripheral.sendDisplay([1])
        f.drain()
        XCTAssertEqual(f.manager.invalidOperations, [])
        XCTAssertEqual(f.manager.startCount, 0)
        f.peripheral.queue.sync { f.transition(.poweredOn); f.completePublication() }
        XCTAssertEqual(f.manager.startCount, 1)
        f.peripheral.stop()
        f.peripheral.beginAdvertising()
        f.peripheral.sendDisplay([2])
        f.drain()
        f.peripheral.queue.sync { f.peripheral.peripheralManagerDidUpdateState(f.manager) }
        XCTAssertEqual(f.manager.startCount, 1)
        XCTAssertEqual(f.manager.services.count, 0)
    }

    @objc func testStaleServicePublicationDoesNotStartAdvertising() {
        let f = Fixture(publish: false)
        f.peripheral.queue.sync {
            let stale = f.manager.services[0]
            f.transition(.resetting)
            f.transition(.poweredOn)
            f.peripheral.peripheralManager(f.manager, didAdd: stale, error: nil)
            XCTAssertEqual(f.manager.startCount, 0)
            f.completePublication()
            XCTAssertEqual(f.manager.startCount, 1)
        }
    }

}

let suite = XCTestSuite(forTestCaseClass: PeripheralLifecycleTests.self)
suite.run()
exit(suite.testRun?.hasSucceeded == true ? 0 : 1)

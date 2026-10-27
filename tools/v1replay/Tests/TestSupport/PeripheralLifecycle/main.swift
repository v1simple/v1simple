import Foundation
import CoreBluetooth
import XCTest

final class Fixture {
    let peripheral: V1Peripheral
    let manager: CBPeripheralManager
    let first = CBCentral()
    let second = CBCentral()

    init(deferred: Bool = false, publish: Bool = true, commandAckHoldMs: Int? = nil) {
        var config = V1Peripheral.Config()
        config.deferAdvertising = deferred
        config.commandAckHoldMs = commandAckHoldMs
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
    @objc func testCommandAckFaultIsOneShotWithoutBlockingNotifications() {
        let f = Fixture(commandAckHoldMs: 50)
        let released = expectation(description: "held START ACK is released")
        var events: [ReplayCommandAckFaultEvent.State] = []
        var deliveredWhileHeld = false
        f.peripheral.onCommandAckFaultEvent = {
            events.append($0.state)
            if $0.state == .released { released.fulfill() }
        }
        f.peripheral.onNotificationEvent = { event in
            if event.state == ReplayNotificationEvent.State.accepted.rawValue && event.payloadHex == "010203" {
                deliveredWhileHeld = events == [.held] && f.manager.responses.count == 1
            }
        }
        f.peripheral.queue.sync {
            f.subscribe(f.first)
            XCTAssertTrue(f.characteristic(V1.commandUUID).properties.contains(.write))
            f.write(f.first, id: 0x3C)
            XCTAssertEqual(f.manager.responses.count, 1, "unrelated commands ACK immediately")
            f.write(f.first, id: 0x41)
            XCTAssertTrue(f.peripheral.alertDataRequested)
            XCTAssertEqual(f.manager.responses.count, 1, "START ACK is held")
            XCTAssertEqual(events, [.held])
            f.peripheral.sendDisplay([1, 2, 3])
            XCTAssertTrue(f.received(f.first).contains([1, 2, 3]), "hold must not stop notification delivery")
            XCTAssertTrue(deliveredWhileHeld, "notification must be accepted before START ACK")
        }
        // A dispatch deadline does not guarantee when a busy host runs the timer.
        wait(for: [released], timeout: 2)
        f.peripheral.queue.sync {
            XCTAssertEqual(events, [.held, .released])
            XCTAssertEqual(f.manager.responses.count, 2)
            f.write(f.first, id: 0x41)
            XCTAssertEqual(f.manager.responses.count, 3, "fault cannot recur")
        }
    }

    @objc func testCommandAckFaultCancelsAtDisconnectOwnerLossPowerLossAndStop() {
        for ending in ["disconnect", "ownerLoss", "powerLoss", "stop"] {
            let f = Fixture(commandAckHoldMs: 50)
            var events: [ReplayCommandAckFaultEvent.State] = []
            f.peripheral.onCommandAckFaultEvent = { events.append($0.state) }
            f.peripheral.queue.sync { f.subscribe(f.first); f.write(f.first, id: 0x41) }
            if ending == "stop" {
                f.peripheral.stop()
            } else if ending == "ownerLoss" {
                f.peripheral.queue.sync { f.subscribe(f.second) }
            } else if ending == "powerLoss" {
                f.peripheral.queue.sync { f.transition(.poweredOff) }
            } else {
                f.peripheral.queue.sync {
                    f.unsubscribe(f.first)
                    f.subscribe(f.second)
                    f.write(f.second, id: 0x41)
                    XCTAssertEqual(f.manager.responses.count, 1, "reconnect ACK is healthy")
                }
            }
            Thread.sleep(forTimeInterval: 0.08)
            f.peripheral.queue.sync {
                XCTAssertEqual(events, [.held, .cancelled])
                XCTAssertEqual(f.manager.responses.count, ending == "disconnect" ? 1 : 0,
                               "stale timer must not respond: \(ending)")
            }
        }
    }

    @objc func testCommandAckDefaultsAndZeroHoldControl() {
        for delay: Int? in [nil, 0] {
            let f = Fixture(commandAckHoldMs: delay)
            f.peripheral.queue.sync {
                f.subscribe(f.first)
                XCTAssertEqual(f.characteristic(V1.commandUUID).properties.contains(.write), delay != nil)
                f.write(f.first, id: 0x41)
                XCTAssertEqual(f.manager.responses.count, 1)
            }
        }
    }

    @objc func testInvalidAndForeignStartCannotConsumeCommandAckFault() {
        let f = Fixture(commandAckHoldMs: 50)
        var events: [ReplayCommandAckFaultEvent.State] = []
        f.peripheral.onCommandAckFaultEvent = { events.append($0.state) }
        f.peripheral.queue.sync {
            f.subscribe(f.first)
            f.write(f.first, id: 0x41, payload: [1])
            XCTAssertTrue(events.isEmpty)
            f.write(f.second, id: 0x41)
            XCTAssertTrue(events.isEmpty)
            f.unsubscribe(f.first)
            f.subscribe(f.first)
            f.write(f.first, id: 0x41)
            XCTAssertEqual(events, [.held])
        }
        f.peripheral.stop()
    }

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

// Real Player -> packet plan -> Peripheral -> Session, with only the Bluetooth
// framework replaced. Firmware START admission is modeled below: the central
// cannot issue it until this subscription has accepted a canonical idle ID31.
private final class PlaybackFixture {
    let transport = Fixture()
    let player: Player
    private let lock = NSLock()
    private var requested: [ReplayStimulusEvent] = []
    private var accepted: [ReplayNotificationEvent] = []
    var onSample: ((Int) -> Void)?

    init(muted: [Bool], gap: Double = 0.04, loop: Bool = false, managed: Bool = false,
         idleTail: Double = 0) {
        let encounter = Encounter(origin: .externalInput, samples: muted.enumerated().map {
            TimedSample(offset: Double($0.offset) * gap, phase: "test", muted: $0.element,
                        alerts: [ReplayAlert(band: .ka, frequencyMHz: 34_700, strength: 3,
                                             direction: .front, isPriority: true)], sourceIndex: $0.offset)
        })
        var options = Player.Options()
        options.idleLead = 0
        options.idleTail = idleTail
        options.idleHz = 20
        options.loop = loop
        options.waitForAlertData = managed
        options.quietAfterComplete = true
        player = Player(encounter: encounter, peripheral: transport.peripheral, options: options)
        player.onStimulusRequested = { [weak self] event in
            guard let self else { return }
            self.lock.lock()
            self.requested.append(event)
            let count = self.requested.count
            self.lock.unlock()
            self.onSample?(count)
        }
        transport.peripheral.onNotificationEvent = { [weak self] event in
            guard let self, event.state == "notification_accepted" else { return }
            self.lock.lock(); self.accepted.append(event); self.lock.unlock()
        }
        // Match main.swift's actual command wiring.
        transport.peripheral.onMuteCommand = { [weak player] in player?.setMuteOverride($0) }
    }

    var events: [ReplayStimulusEvent] {
        lock.lock(); defer { lock.unlock() }; return requested
    }

    func subscribe(_ central: CBCentral, channels: [String] = [V1.displayShortUUID]) {
        transport.peripheral.queue.sync { for channel in channels { transport.subscribe(central, to: channel) } }
    }

    func startOrdinary() {
        subscribe(transport.first)
        transport.peripheral.queue.sync { transport.write(transport.first, id: 0x41) }
        player.start()
    }

    func baselineCount(_ central: CBCentral, muted: Bool = false) -> Int {
        let packet = V1.PlaybackPacketPlan.idleDisplayPacket(
            controlState: transport.peripheral.controlState, displayOn: true, muted: muted)
        return transport.peripheral.queue.sync { transport.received(central).filter { $0 == packet }.count }
    }

    @discardableResult
    func admitStart(_ central: CBCentral) -> Bool {
        guard baselineCount(central) > 0 else { return false }
        transport.peripheral.queue.sync { transport.write(central, id: 0x41) }
        return transport.peripheral.alertDataRequested
    }

    func stop() {
        player.stop()
        Thread.sleep(forTimeInterval: 0.06)
        transport.drain()
        onSample = nil
    }

    func assertRequestedBytesAccepted(file: StaticString = #filePath, line: UInt = #line) {
        transport.drain()
        lock.lock(); let delivery = accepted; let samples = requested; lock.unlock()
        for sample in samples {
            let actual = delivery.filter { $0.stimulusSequence == sample.stimulusSequence }
            XCTAssertEqual(actual.map(\.emissionOrdinal), sample.notifications.map { Optional($0.ordinal) },
                           file: file, line: line)
            XCTAssertEqual(actual.map(\.payloadHex), sample.notifications.map(\.bytesHex), file: file, line: line)
        }
        // Also compare the owner's acceptance witness to the software adapter's
        // actual payloads, including baseline and clear packets outside samples.
        let adapter = transport.peripheral.queue.sync {
            transport.manager.notifications.map { $0.bytes.map { String(format: "%02X", $0) }.joined() }
        }
        XCTAssertEqual(delivery.map(\.payloadHex), adapter, file: file, line: line)
    }
}

final class PlayerLifecycleTests: XCTestCase {
    private func assertFinishedNavigation(target: Int, action: (Player) -> Void) {
        let f = PlaybackFixture(muted: [false, true, false, true], gap: 0.015)
        defer { f.stop() }
        f.onSample = { if $0 == 5 { f.player.stop() } }
        f.startOrdinary()
        guard until("completed replay", condition: { f.player.snapshot.phase == .finished }) else { return }
        XCTAssertEqual(f.events.map(\.sourceIndex), [0, 1, 2, 3])
        action(f.player)
        guard until("selected sample after completion", condition: { f.events.count == 5 }) else { return }
        f.stop()
        XCTAssertEqual(f.events[4].sourceIndex, target)
        XCTAssertEqual(f.events[4].expected.muted, target % 2 == 1)
        f.assertRequestedBytesAccepted()
    }

    @objc func testPreviousChangeAfterCompletionRetainsTarget() {
        assertFinishedNavigation(target: 2) { $0.jumpToPreviousChange() }
    }

    @objc func testDirectSeekAfterCompletionRetainsTarget() {
        assertFinishedNavigation(target: 1) { $0.seek(to: 1) }
    }

    @objc func testRestartAfterCompletionStartsAtZero() {
        assertFinishedNavigation(target: 0) { $0.restart() }
    }

    @objc func testSeekDuringPlaybackRetainsTarget() {
        let f = PlaybackFixture(muted: [false, true, false, true], gap: 0.3)
        defer { f.stop() }
        f.onSample = {
            if $0 == 1 { f.player.seek(to: 2) }
            else if $0 == 2 { f.player.stop() }
        }
        f.startOrdinary()
        guard until("selected sample during playback", condition: { f.events.count == 2 }) else { return }
        f.stop()
        XCTAssertEqual(f.events.map(\.sourceIndex), [0, 2])
        f.assertRequestedBytesAccepted()
    }

    @discardableResult
    private func until(_ description: String, timeout: Double = 2,
                       file: StaticString = #filePath, line: UInt = #line,
                       condition: () -> Bool) -> Bool {
        let deadline = nowSeconds() + timeout
        while !condition() && nowSeconds() < deadline { Thread.sleep(forTimeInterval: 0.002) }
        let result = condition()
        XCTAssertTrue(result, description, file: file, line: line)
        return result
    }

    @objc func testInitialAndReplacementStartRequireCanonicalBaselineAndFreezeDeadline() {
        for channels in [[V1.displayShortUUID], [V1.displayShortUUID, V1.displayLongUUID, V1.notifyAltUUID]] {
            let f = PlaybackFixture(muted: [false, false, false], gap: 0.6, managed: true)
            defer { f.stop() }
            f.subscribe(f.transport.first, channels: channels)
            XCTAssertFalse(f.admitStart(f.transport.first), "START is held until canonical ID31")
            f.player.start()
            guard until("initial ID31", condition: { f.baselineCount(f.transport.first) > 0 }) else { continue }
            Thread.sleep(forTimeInterval: 0.12)
            XCTAssertTrue(f.events.isEmpty)
            XCTAssertEqual(f.player.currentIndex, 0)
            XCTAssertLessThanOrEqual(f.baselineCount(f.transport.first), 4, "baseline cadence must be bounded")
            XCTAssertTrue(f.admitStart(f.transport.first))
            guard until("first authored sample", condition: { f.player.currentIndex == 1 }) else { continue }
            Thread.sleep(forTimeInterval: 0.10)
            let lostAt = hostMonotonicNanoseconds()
            f.transport.peripheral.queue.sync {
                for (index, channel) in channels.enumerated() {
                    f.transport.unsubscribe(f.transport.first, from: channel)
                    XCTAssertEqual(f.transport.peripheral.alertDataRequested, index < channels.count - 1,
                                   "only the final channel departure clears START")
                }
            }
            guard until("recovery wait", condition: { f.player.snapshot.phase == .waiting }) else { continue }
            Thread.sleep(forTimeInterval: 0.12)
            XCTAssertFalse(f.admitStart(f.transport.second))
            f.subscribe(f.transport.second, channels: channels)
            guard until("replacement subscription must receive ID31", condition: { f.baselineCount(f.transport.second) > 0 }) else { continue }
            Thread.sleep(forTimeInterval: 0.12)
            XCTAssertEqual(f.player.currentIndex, 1)
            XCTAssertEqual(f.events.count, 1, "waiting cannot advance the authored encounter")
            XCTAssertLessThanOrEqual(f.baselineCount(f.transport.second), 4)
            XCTAssertTrue(f.admitStart(f.transport.second))
            let restoredAt = hostMonotonicNanoseconds()
            guard until("pending and following samples", condition: { f.events.count == 3 }) else { continue }
            f.stop()
            let events = f.events
            XCTAssertEqual(events.map(\.sourceIndex), [0, 1, 2])
            let frozenGap = Double(events[1].intendedHostMonotonicNs - events[0].intendedHostMonotonicNs) / 1e9
            XCTAssertGreaterThanOrEqual(frozenGap, 0.6 + Double(restoredAt - lostAt) / 1e9 - 0.04)
            XCTAssertEqual(Double(events[2].intendedHostMonotonicNs - events[1].intendedHostMonotonicNs) / 1e9,
                           0.6, accuracy: 0.000001, "recovery must not introduce catch-up samples")
            f.assertRequestedBytesAccepted()
        }
    }

    @objc func testRecoveryWaitHonorsRestartSeekAndStop() {
        for action in ["restart", "seek", "stop"] {
            let f = PlaybackFixture(muted: [false, false, false, false], gap: 0.3, managed: true)
            defer { f.stop() }
            f.startOrdinary()
            guard until("initial sample", condition: { f.player.currentIndex == 1 }) else { continue }
            f.transport.peripheral.queue.sync { f.transport.unsubscribe(f.transport.first) }
            guard until("readiness lost", condition: { f.player.snapshot.phase == .waiting }) else { continue }
            if action == "restart" { f.player.restart() }
            else if action == "seek" { f.player.seek(to: 2) }
            else { f.player.stop() }
            if action != "stop" {
                guard until("transport control consumed while disconnected", condition: {
                    f.player.currentIndex == (action == "seek" ? 2 : 0)
                }) else { continue }
            }
            f.subscribe(f.transport.second)
            if action == "stop" {
                Thread.sleep(forTimeInterval: 0.12)
                XCTAssertEqual(f.baselineCount(f.transport.second), 0)
                XCTAssertEqual(f.events.count, 1)
            } else {
                guard until("controlled recovery baseline", condition: { f.baselineCount(f.transport.second) > 0 }) else { continue }
                XCTAssertTrue(f.admitStart(f.transport.second))
                guard until("controlled recovery sample", condition: { f.events.count >= 2 }) else { continue }
                XCTAssertEqual(f.events[1].sourceIndex, action == "seek" ? 2 : 0)
            }
            f.stop()
            f.assertRequestedBytesAccepted()
        }
    }

    @objc func testStoppedPeripheralCannotReceiveAdmissionTraffic() {
        let f = PlaybackFixture(muted: [false, false], gap: 0.3, managed: true)
        defer { f.stop() }
        f.startOrdinary()
        guard until("initial sample", condition: { f.player.currentIndex == 1 }) else { return }
        f.transport.peripheral.stop()
        guard until("stopped transport wait", condition: { f.player.snapshot.phase == .waiting }) else { return }
        let count = f.transport.peripheral.queue.sync { f.transport.manager.notifications.count }
        Thread.sleep(forTimeInterval: 0.15)
        XCTAssertEqual(f.events.count, 1)
        XCTAssertEqual(f.transport.peripheral.queue.sync { f.transport.manager.notifications.count }, count)
        XCTAssertEqual(f.transport.manager.invalidOperations, [])
    }

    @objc func testAuthoredMuteLoopsDoNotCreateExternalOverrides() {
        for authored in [[false, true], [false, true, false], [false, false], [true, true]] {
            let f = PlaybackFixture(muted: authored, loop: true)
            defer { f.stop() }
            let count = authored.count * 3
            f.onSample = { if $0 == count { f.player.stop() } }
            f.startOrdinary()
            guard until("three authored passes", condition: { f.events.count == count }) else { continue }
            f.stop()
            XCTAssertEqual(f.events.map(\.expected.muted), Array(repeating: authored, count: 3).flatMap { $0 })
            XCTAssertNil(f.player.snapshot.muteOverride, "authored state must not become a command override")
            f.assertRequestedBytesAccepted()
        }
    }

    @objc func testAuthoredMuteRebasesOnRestartAndBackwardOrForwardSeek() {
        for action in ["restart", "backward", "forward"] {
            let f = PlaybackFixture(muted: [false, false, true, true, false], gap: 0.04)
            defer { f.stop() }
            f.onSample = { count in
                if count == 3 {
                    if action == "restart" { f.player.restart() }
                    else { f.player.seek(to: action == "backward" ? 1 : 4) }
                }
                if count == 4 { f.player.stop() }
            }
            f.startOrdinary()
            guard until("repositioned authored sample", condition: { f.events.count == 4 }) else { continue }
            f.stop()
            XCTAssertEqual(f.events.map(\.sourceIndex), [0, 1, 2, action == "restart" ? 0 : action == "backward" ? 1 : 4])
            XCTAssertEqual(f.events.map(\.expected.muted), [false, false, true, false])
            f.assertRequestedBytesAccepted()
        }
    }

    @objc func testIdleTailRetainsAuthoredMuteUntilTheNextPass() {
        let f = PlaybackFixture(muted: [false, true], loop: true, idleTail: 0.12)
        defer { f.stop() }
        f.onSample = { if $0 == 3 { f.player.stop() } }
        f.startOrdinary()
        guard until("next pass after muted idle tail", condition: { f.events.count == 3 }) else { return }
        f.stop()
        XCTAssertGreaterThan(f.baselineCount(f.transport.first, muted: true), 0,
                             "idle tail must report the last authored state")
        XCTAssertEqual(f.events.map(\.expected.muted), [false, true, false])
        f.assertRequestedBytesAccepted()
    }

    @objc func testDUTAndManualCommandsPersistBetweenAuthoredEdges() {
        for dut in [false, true] {
            let f = PlaybackFixture(muted: [false, false, true, true, false, false])
            defer { f.stop() }
            f.onSample = { count in
                if count == 1 || count == 3 {
                    let muted = count == 1
                    if dut {
                        f.transport.peripheral.queue.sync {
                            f.transport.write(f.transport.first, id: muted ? 0x34 : 0x35)
                        }
                    } else { f.player.setMuteOverride(muted) }
                }
                if count == 6 { f.player.stop() }
            }
            f.startOrdinary()
            guard until("command and checkpoint precedence", condition: { f.events.count == 6 }) else { continue }
            f.stop()
            XCTAssertEqual(f.events.map(\.expected.muted), [false, true, true, false, false, false])
            f.assertRequestedBytesAccepted()
        }
    }

    @objc func testExternalMuteSurvivesLoopRestartAndSeekUntilAnAuthoredEdge() {
        for dut in [false, true] {
            for action in ["loop", "restart", "seek"] {
                let f = PlaybackFixture(muted: [false, false, true, true], loop: true)
                defer { f.stop() }
                f.onSample = { count in
                    if count == 3 {
                        if dut {
                            f.transport.peripheral.queue.sync { f.transport.write(f.transport.first, id: 0x35) }
                        } else { f.player.setMuteOverride(false) }
                    }
                    if count == 4 {
                        if action == "restart" { f.player.restart() }
                        else if action == "seek" { f.player.seek(to: 1) }
                    }
                    if count == (action == "seek" ? 6 : 7) { f.player.stop() }
                }
                f.startOrdinary()
                let count = action == "seek" ? 6 : 7
                guard until("command retained across reposition", condition: { f.events.count == count }) else { continue }
                f.stop()
                XCTAssertEqual(f.events.map(\.expected.muted),
                               action == "seek" ? [false, false, true, false, false, true]
                                                : [false, false, true, false, false, false, true])
                f.assertRequestedBytesAccepted()
            }
        }
    }

    @objc func testExternalMuteOnSurvivesRepositionWithoutAuthoredEdges() {
        for dut in [false, true] {
            for action in ["loop", "restart", "seek"] {
                let f = PlaybackFixture(muted: [false, false], loop: true)
                defer { f.stop() }
                f.onSample = { count in
                    if count == 1 {
                        if dut {
                            f.transport.peripheral.queue.sync { f.transport.write(f.transport.first, id: 0x34) }
                        } else { f.player.setMuteOverride(true) }
                    }
                    if count == 2 {
                        if action == "restart" { f.player.restart() }
                        else if action == "seek" { f.player.seek(to: 1) }
                    }
                    if count == 4 { f.player.stop() }
                }
                f.startOrdinary()
                guard until("external ON survives reposition", condition: { f.events.count == 4 }) else { continue }
                f.stop()
                XCTAssertEqual(f.events.map(\.expected.muted), [false, true, true, true])
                XCTAssertEqual(f.player.snapshot.muteOverride, true)
                f.assertRequestedBytesAccepted()
            }
        }
    }

    @objc func testManualToggleCanReleaseOverrideBackToAuthoredMute() {
        let f = PlaybackFixture(muted: [true, true, true, true])
        defer { f.stop() }
        f.onSample = { count in
            if count < 4 { f.player.toggleMuteOverride() }
            else { f.player.stop() }
        }
        f.startOrdinary()
        guard until("manual toggle ownership", condition: { f.events.count == 4 }) else { return }
        f.stop()
        XCTAssertEqual(f.events.map(\.expected.muted), [true, true, false, true])
        XCTAssertNil(f.player.snapshot.muteOverride)
        f.assertRequestedBytesAccepted()
    }
}

private final class ProxyFixture {
    let queue = DispatchQueue(label: "v1replay.proxy-test")
    let proxy: V1ProxyCentral
    let manager: CBCentralManager
    var connectedCallbacks = 0
    var disconnectedCallbacks = 0

    init() {
        proxy = V1ProxyCentral(config: .init(excludeName: "V1-REPLAY"), queue: queue)
        manager = CBCentralManager.latest!
        proxy.onConnected = { [weak self] _, _ in self?.connectedCallbacks += 1 }
        proxy.onDisconnected = { [weak self] _ in self?.disconnectedCallbacks += 1 }
        queue.sync { proxy.centralManagerDidUpdateState(manager) }
    }

    func connect() -> CBPeripheral {
        let peer = CBPeripheral()
        let service = CBService(type: CBUUID(string: V1.serviceUUID), primary: true)
        service.characteristics = V1ProxyCentral.notifyUUIDs.map {
            CBCharacteristic(type: $0, properties: [.notify], value: nil, permissions: [])
        } + [CBCharacteristic(type: CBUUID(string: V1.commandUUID),
                              properties: [.writeWithoutResponse], value: nil, permissions: [])]
        peer.services = [service]
        proxy.centralManager(manager, didDiscover: peer,
                             advertisementData: [CBAdvertisementDataServiceUUIDsKey: [service.uuid]],
                             rssi: NSNumber(value: -40))
        proxy.centralManager(manager, didConnect: peer)
        return peer
    }

    func discover(_ peer: CBPeripheral, failing stage: String? = nil) {
        let error = NSError(domain: "CBATTErrorDomain", code: 0x11)
        proxy.peripheral(peer, didDiscoverServices: stage == "services" ? error : nil)
        if stage == "services" { return }
        proxy.peripheral(peer, didDiscoverCharacteristicsFor: peer.services![0],
                         error: stage == "characteristics" ? error : nil)
        if stage != nil { return }
        for characteristic in peer.notificationRequests {
            characteristic.isNotifying = true
            proxy.peripheral(peer, didUpdateNotificationStateFor: characteristic, error: nil)
        }
    }

    func assertConnected(_ peer: CBPeripheral, file: StaticString = #filePath, line: UInt = #line) {
        XCTAssertTrue(proxy.isConnected, file: file, line: line)
        XCTAssertEqual(connectedCallbacks, 1, file: file, line: line)
        XCTAssertEqual(peer.serviceRequests, 1, file: file, line: line)
        XCTAssertEqual(peer.characteristicRequests, 1, file: file, line: line)
        XCTAssertEqual(peer.notificationRequests.map(\.uuid), V1ProxyCentral.notifyUUIDs,
                       file: file, line: line)
        XCTAssertEqual(proxy.subscriptions, 3, file: file, line: line)
        XCTAssertFalse(manager.isScanning, file: file, line: line)
    }
}

final class ProxyLifecycleTests: XCTestCase {
    private func assertDiscoveryRecovery(stage: String) {
        let f = ProxyFixture()
        defer { f.proxy.stop() }
        f.queue.sync {
            let failed = f.connect()
            f.discover(failed, failing: stage)
            XCTAssertFalse(f.proxy.isConnected)
            XCTAssertEqual(f.connectedCallbacks, 0, "a failed request cannot advertise a usable proxy")
            XCTAssertEqual(f.proxy.subscriptions, 0)
            XCTAssertEqual(failed.notificationRequests.count, 0)
            XCTAssertEqual(failed.characteristicRequests, stage == "services" ? 0 : 1)
            XCTAssertEqual(f.manager.cancellations.count, 1)
            XCTAssertTrue(f.manager.cancellations.first === failed)
            XCTAssertFalse(f.manager.isScanning, "wait for cancellation completion before rescanning")
            XCTAssertEqual(f.manager.scans, 1)
            guard f.manager.cancellations.first === failed else { return }

            // The fake delivers a disconnect only for an actual cancellation.
            f.manager.completeCancellation()
            XCTAssertEqual(f.disconnectedCallbacks, 1)
            XCTAssertTrue(f.manager.isScanning)
            XCTAssertEqual(f.manager.scans, 2)
            let recovered = f.connect()
            XCTAssertFalse(recovered === failed)
            XCTAssertEqual(f.manager.connections.count, 2)
            f.discover(recovered)
            f.assertConnected(recovered)
        }
    }

    @objc func testServiceDiscoveryFailureCancelsAndReconnects() {
        assertDiscoveryRecovery(stage: "services")
    }

    @objc func testCharacteristicDiscoveryFailureCancelsAndReconnects() {
        assertDiscoveryRecovery(stage: "characteristics")
    }

    @objc func testSuccessfulDiscoverySubscribesWithoutCancellation() {
        let f = ProxyFixture()
        defer { f.proxy.stop() }
        f.queue.sync {
            let peer = f.connect()
            f.discover(peer)
            f.assertConnected(peer)
            XCTAssertEqual(f.manager.cancellations.count, 0)
            XCTAssertEqual(f.manager.scans, 1)
        }
    }

    @objc func testMissingServiceStillCancelsAndRescans() {
        let f = ProxyFixture()
        defer { f.proxy.stop() }
        f.queue.sync {
            let peer = f.connect()
            peer.services = []
            f.proxy.peripheral(peer, didDiscoverServices: nil)
            XCTAssertEqual(f.manager.cancellations.count, 1)
            XCTAssertEqual(f.connectedCallbacks, 0)
            f.manager.completeCancellation()
            XCTAssertTrue(f.manager.isScanning)
            XCTAssertEqual(f.manager.scans, 2)
        }
    }

    @objc func testStopDuringDiscoveryCancellationDoesNotResumeScanning() {
        for stage in ["services", "characteristics"] {
            let f = ProxyFixture()
            f.queue.sync { f.discover(f.connect(), failing: stage) }
            f.proxy.stop()
            f.queue.sync {
                f.manager.completeCancellation()
                f.proxy.centralManagerDidUpdateState(f.manager)
                XCTAssertFalse(f.manager.isScanning)
                XCTAssertEqual(f.manager.scans, 1)
                XCTAssertEqual(f.manager.connections.count, 1)
                XCTAssertEqual(f.connectedCallbacks, 0)
            }
        }
    }
}

let suite = XCTestSuite(name: "Replay software transport")
suite.addTest(XCTestSuite(forTestCaseClass: PeripheralLifecycleTests.self))
suite.addTest(XCTestSuite(forTestCaseClass: PlayerLifecycleTests.self))
suite.addTest(XCTestSuite(forTestCaseClass: ProxyLifecycleTests.self))
suite.run()
exit(suite.testRun?.hasSucceeded == true ? 0 : 1)

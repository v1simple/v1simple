import Foundation

extension V1 {

    /// Deterministic protocol-session state. CoreBluetooth is only an adapter
    /// around this type; notification delivery remains an integration concern.
    struct Session {

        struct Config {
            var header: Header = .v1ToApp
            var checksum = true
            var version = "4.1038"
            var mainVolume: UInt8 = 4
            var mutedVolume: UInt8 = 0
            /// Nil preserves the existing behavior: saved mirrors current.
            var savedMainVolume: UInt8?
            var savedMutedVolume: UInt8?
            var userBytes: [UInt8] = Array(repeating: 0, count: 6)
        }

        enum SubscriptionChannel: Hashable {
            case displayShort
            case displayLong
            case compatibilityNotify
        }

        struct Readiness: Equatable {
            let displaySubscribed: Bool
            let alertSubscribed: Bool
            let alertDataRequested: Bool

            /// Enough logical state for short replies and display packets.
            var shortTrafficReady: Bool { return displaySubscribed }

            /// The current bench requires both the long subscription and an
            /// explicit start request before it emits alert rows.
            var alertStreamReady: Bool {
                return alertSubscribed && alertDataRequested
            }
        }

        enum Rejection: Equatable {
            case invalidRequestHeader
            case invalidChecksum
            case unexpectedPayload(packetID: UInt8, count: Int)
            case invalidUserBytesLength(Int)
        }

        enum Effect: Equatable {
            case reply(ReplyDecision)
            case compatibilityAcknowledgement(ReplyDecision)
            case alertDataChanged(Bool)
            case muteChanged(Bool)
            case displayPowerChanged(Bool)
            case userBytesStored([UInt8])
            case rejected(Rejection)
            case unhandled
        }

        struct CommandOutcome: Equatable {
            let packet: InboundPacket
            let effects: [Effect]
        }

        private struct Subscription: Hashable {
            let central: UUID
            let channel: SubscriptionChannel
        }

        private let config: Config
        private var subscriptions: Set<Subscription> = []
        private var receiveBuffer: [UInt8] = []
        private var pendingPackets: [InboundPacket] = []
        private(set) var alertDataRequested = false
        private var userBytes: UserBytesStore

        init(config: Config = Config()) {
            self.config = config
            self.userBytes = UserBytesStore(
                Session.normalizedUserBytes(config.userBytes, version: config.version)
            )
        }

        var readiness: Readiness {
            return Readiness(
                displaySubscribed: subscriptions.contains { $0.channel == .displayShort },
                alertSubscribed: subscriptions.contains { $0.channel == .displayLong },
                alertDataRequested: alertDataRequested
            )
        }

        var subscriberCount: Int {
            return Set(subscriptions.map(\.central)).count
        }

        var bufferedByteCount: Int { return receiveBuffer.count }
        var storedUserBytes: [UInt8] { return userBytes.bytes }

        mutating func subscribe(central: UUID, channel: SubscriptionChannel) {
            subscriptions.insert(Subscription(central: central, channel: channel))
        }

        @discardableResult
        mutating func unsubscribe(central: UUID, channel: SubscriptionChannel) -> Int {
            subscriptions.remove(Subscription(central: central, channel: channel))
            let remaining = subscriberCount
            if remaining == 0 {
                alertDataRequested = false
                receiveBuffer.removeAll()
                pendingPackets.removeAll()
            }
            return remaining
        }

        /// Accept a raw byte-stream chunk. An incomplete tail stays buffered;
        /// complete frames are decided in wire order.
        mutating func receive(_ bytes: [UInt8]) -> [CommandOutcome] {
            append(bytes)
            var outcomes: [CommandOutcome] = []
            while let outcome = nextOutcome() { outcomes.append(outcome) }
            return outcomes
        }

        /// Queue complete frames without applying their state transitions yet.
        /// The peripheral drains them one at a time so callbacks observe the
        /// state belonging to each wire-order outcome.
        mutating func append(_ bytes: [UInt8]) {
            receiveBuffer.append(contentsOf: bytes)
            if receiveBuffer.count > 512 {
                receiveBuffer.removeFirst(receiveBuffer.count - 512)
            }
            pendingPackets.append(contentsOf: V1.drainFrames(from: &receiveBuffer))
        }

        mutating func nextOutcome() -> CommandOutcome? {
            guard !pendingPackets.isEmpty else { return nil }
            return decide(pendingPackets.removeFirst())
        }

        private mutating func decide(_ packet: InboundPacket) -> CommandOutcome {
            if packet.raw[1] != 0xDA || packet.raw[2] != 0xE6 {
                return CommandOutcome(packet: packet, effects: [
                    .rejected(.invalidRequestHeader)
                ])
            }

            let checksumIndex = packet.raw.count - 2
            let expectedChecksum = packet.raw[..<checksumIndex].reduce(UInt8(0), &+)
            if packet.raw[checksumIndex] != expectedChecksum {
                return CommandOutcome(packet: packet, effects: [
                    .rejected(.invalidChecksum)
                ])
            }

            let effects: [Effect]
            switch packet.id {
            case PacketID.reqVersion.rawValue:
                guard let reply = V1.replyDecision(
                    for: packet,
                    version: config.version,
                    header: config.header,
                    checksum: config.checksum
                ) else {
                    return CommandOutcome(packet: packet, effects: [
                        .rejected(.unexpectedPayload(
                            packetID: packet.id,
                            count: packet.payload.count
                        ))
                    ])
                }
                effects = [.reply(reply)]

            case PacketID.reqAllVolume.rawValue:
                guard packet.payload.isEmpty else {
                    return rejectUnexpectedPayload(packet)
                }
                effects = [.reply(ReplyDecision(
                    channel: .displayShort,
                    bytes: V1.allVolumePacket(
                        header: config.header,
                        main: config.mainVolume,
                        muted: config.mutedVolume,
                        savedMain: config.savedMainVolume,
                        savedMuted: config.savedMutedVolume,
                        checksum: config.checksum
                    )
                ))]

            case PacketID.reqUserBytes.rawValue:
                guard packet.payload.isEmpty else {
                    return rejectUnexpectedPayload(packet)
                }
                effects = [.reply(ReplyDecision(
                    channel: .displayShort,
                    bytes: V1.userBytesPacket(
                        header: config.header,
                        bytes: userBytes.bytes,
                        checksum: config.checksum
                    )
                ))]

            case PacketID.reqStartAlertData.rawValue:
                guard packet.payload.isEmpty else {
                    return rejectUnexpectedPayload(packet)
                }
                alertDataRequested = true
                effects = [.alertDataChanged(true)]

            case PacketID.reqStopAlertData.rawValue:
                guard packet.payload.isEmpty else {
                    return rejectUnexpectedPayload(packet)
                }
                alertDataRequested = false
                effects = [.alertDataChanged(false)]

            case PacketID.muteOn.rawValue:
                effects = [.muteChanged(true), .compatibilityAcknowledgement(ack(for: packet.id))]

            case PacketID.muteOff.rawValue:
                effects = [.muteChanged(false), .compatibilityAcknowledgement(ack(for: packet.id))]

            case PacketID.turnOffDisplay.rawValue:
                effects = [.displayPowerChanged(false), .compatibilityAcknowledgement(ack(for: packet.id))]

            case PacketID.turnOnDisplay.rawValue:
                effects = [.displayPowerChanged(true), .compatibilityAcknowledgement(ack(for: packet.id))]

            case PacketID.reqWriteUserBytes.rawValue:
                guard packet.payload.count == 6 else {
                    return CommandOutcome(packet: packet, effects: [
                        .rejected(.invalidUserBytesLength(packet.payload.count))
                    ])
                }
                let stored = Session.normalizedUserBytes(
                    packet.payload,
                    version: config.version
                )
                _ = userBytes.write(stored)
                effects = [.userBytesStored(stored)]

            case PacketID.changeMode.rawValue,
                 PacketID.reqWriteVolume.rawValue:
                effects = [.compatibilityAcknowledgement(ack(for: packet.id))]

            default:
                effects = [.unhandled]
            }

            return CommandOutcome(packet: packet, effects: effects)
        }

        private func rejectUnexpectedPayload(
            _ packet: InboundPacket
        ) -> CommandOutcome {
            return CommandOutcome(packet: packet, effects: [
                .rejected(.unexpectedPayload(
                    packetID: packet.id,
                    count: packet.payload.count
                ))
            ])
        }

        private func ack(for packetID: UInt8) -> ReplyDecision {
            return ReplyDecision(
                channel: .displayLong,
                bytes: V1.ackPacket(
                    header: config.header,
                    id: packetID,
                    checksum: config.checksum
                )
            )
        }

        private static func normalizedUserBytes(
            _ raw: [UInt8],
            version: String
        ) -> [UInt8] {
            var normalized = UserBytesStore(raw).bytes
            if !supportsSixUserBytes(version: version) {
                normalized[4] = 0xFF
                normalized[5] = 0xFF
            }
            return normalized
        }

        /// Gen2 versions before 4.1039 expose four writable bytes and fixed
        /// 0xFF values in the final two wire positions. Other modeled versions
        /// retain all six bytes.
        private static func supportsSixUserBytes(version: String) -> Bool {
            let components = version
                .filter { $0.isNumber || $0 == "." }
                .split(separator: ".", maxSplits: 1)
            guard components.count == 2,
                  let major = Int(components[0]),
                  let build = Int(components[1]),
                  major == 4 else {
                return true
            }
            return build >= 1039
        }
    }
}

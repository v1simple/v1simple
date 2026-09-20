import Foundation

extension V1 {

    /// Deterministic protocol-session state. CoreBluetooth is only an adapter
    /// around this type; notification delivery remains an integration concern.
    struct Session {

        struct Config {
            var header: Header = .v1ToApp
            /// Controls generated response framing only. Inbound V1Simple
            /// requests always require their checksum byte to validate.
            var outboundChecksum = true
            var version = "4.1038"
            var mode: ModeGlyph = .advancedLogic
            var mainVolume: UInt8 = 4
            var mutedVolume: UInt8 = 0
            /// Nil means saved initially mirrors current. Session initialization
            /// resolves it once so later non-saving writes cannot move saved.
            var savedMainVolume: UInt8?
            var savedMutedVolume: UInt8?
            // Valentine Gen2 factory defaults. Individual replay modes may
            // provide an explicit starting state for their authored stimulus.
            var userBytes: [UInt8] = Array(repeating: 0xFF, count: 6)
            var sweepDefinitions: [SweepDefinition]?
        }

        struct SweepDefinition: Codable, Equatable {
            let index: UInt8
            let lowerMHz: UInt16
            let upperMHz: UInt16

            var isUnused: Bool { return lowerMHz == 0 && upperMHz == 0 }
        }

        struct PersistentState: Codable, Equatable {
            static let currentSchemaVersion = 1

            let schemaVersion: Int
            let v1Version: String
            let mode: UInt8
            let mainVolume: UInt8
            let mutedVolume: UInt8
            let savedMainVolume: UInt8
            let savedMutedVolume: UInt8
            let userBytes: [UInt8]
            let sweepDefinitions: [SweepDefinition]
        }

        struct ControlState: Equatable {
            fileprivate(set) var mode: ModeGlyph
            fileprivate(set) var mainVolume: UInt8
            fileprivate(set) var mutedVolume: UInt8
            fileprivate(set) var savedMainVolume: UInt8
            fileprivate(set) var savedMutedVolume: UInt8

            init(mode: ModeGlyph,
                 mainVolume: UInt8,
                 mutedVolume: UInt8,
                 savedMainVolume: UInt8,
                 savedMutedVolume: UInt8) {
                self.mode = mode
                self.mainVolume = mainVolume
                self.mutedVolume = mutedVolume
                self.savedMainVolume = savedMainVolume
                self.savedMutedVolume = savedMutedVolume
            }

            var displayVolume: UInt8 {
                return (mainVolume << 4) | mutedVolume
            }
        }

        enum SubscriptionChannel: Hashable {
            case displayShort
            case displayLong
            case compatibilityNotify
        }

        struct Readiness: Equatable {
            let displaySubscribed: Bool
            let longTrafficSubscribed: Bool
            let alertDataRequested: Bool

            /// Enough logical state for short replies and display packets.
            var shortTrafficReady: Bool { return displaySubscribed }

            /// Current display and alert-row packets are short B2CE traffic.
            /// The bench additionally waits for an explicit start request.
            var alertStreamReady: Bool {
                return displaySubscribed && alertDataRequested
            }
        }

        enum Rejection: Equatable {
            case invalidRequestHeader
            case invalidChecksum
            case unexpectedPayload(packetID: UInt8, count: Int)
            case invalidUserBytesLength(Int)
            case invalidModeValue(UInt8)
            case invalidVolume(main: UInt8, muted: UInt8)
        }

        enum Effect: Equatable {
            case reply(ReplyDecision)
            case alertDataChanged(Bool)
            case muteChanged(Bool)
            case displayPowerChanged(Bool)
            case userBytesStored([UInt8])
            case modeChanged(ModeGlyph)
            case volumeChanged(ControlState)
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
        private static let maximumResidualByteCount = 64
        private(set) var alertDataRequested = false
        private(set) var controlState: ControlState
        private var userBytes: UserBytesStore
        private var sweepDefinitions: [SweepDefinition]
        private var pendingSweepDefinitions: [UInt8: SweepDefinition] = [:]
        private var rejectedMaxSweepIndexOnce = false
        private var reportedSweepDefinitionsBusyOnce = false

        init(config: Config = Config()) {
            self.config = config
            // Resolve nil saved values once. They mean "initially mirror current",
            // not "keep following current after a non-saving volume write".
            self.controlState = ControlState(
                mode: config.mode,
                mainVolume: config.mainVolume,
                mutedVolume: config.mutedVolume,
                savedMainVolume: config.savedMainVolume ?? config.mainVolume,
                savedMutedVolume: config.savedMutedVolume ?? config.mutedVolume
            )
            self.userBytes = UserBytesStore(
                Session.normalizedUserBytes(config.userBytes, version: config.version)
            )
            self.sweepDefinitions = config.sweepDefinitions ?? Session.defaultSweepDefinitions
        }

        /// Apply a modeled physical-V1 current-volume change. Bench playback
        /// uses this once at each authored checkpoint; it is not a V1Simple
        /// command and does not alter any other detector control.
        @discardableResult
        mutating func applyDetectorCurrentVolume(main: UInt8, muted: UInt8) -> ControlState {
            precondition(main <= 9, "detector main volume must be 0...9")
            precondition(muted <= 9, "detector mute volume must be 0...9")
            controlState.mainVolume = main
            controlState.mutedVolume = muted
            return controlState
        }

        /// Apply a modeled physical-V1 mode change. Bench playback uses this
        /// once at each authored checkpoint; it is not a V1Simple command and
        /// does not alter volume or saved detector state.
        @discardableResult
        mutating func applyDetectorMode(_ mode: ModeGlyph) -> ControlState {
            controlState.mode = mode
            return controlState
        }

        var readiness: Readiness {
            return Readiness(
                displaySubscribed: subscriptions.contains { $0.channel == .displayShort },
                longTrafficSubscribed: subscriptions.contains { $0.channel == .displayLong },
                alertDataRequested: alertDataRequested
            )
        }

        var subscriberCount: Int {
            return Set(subscriptions.map(\.central)).count
        }

        var shortSubscriberCount: Int {
            return Set(
                subscriptions.lazy
                    .filter { $0.channel == .displayShort }
                    .map(\.central)
            ).count
        }

        /// Machine-readable transport ownership is valid only while one
        /// central, and no ambiguous second central, owns short notifications.
        var sessionTransportActive: Bool { return shortSubscriberCount == 1 }

        var bufferedByteCount: Int { return receiveBuffer.count }
        var storedUserBytes: [UInt8] { return userBytes.bytes }
        var storedSweepDefinitions: [SweepDefinition] { return sweepDefinitions }
        var persistentState: PersistentState {
            return PersistentState(
                schemaVersion: PersistentState.currentSchemaVersion,
                v1Version: config.version,
                mode: controlState.mode.rawValue,
                mainVolume: controlState.mainVolume,
                mutedVolume: controlState.mutedVolume,
                savedMainVolume: controlState.savedMainVolume,
                savedMutedVolume: controlState.savedMutedVolume,
                userBytes: userBytes.bytes,
                sweepDefinitions: sweepDefinitions
            )
        }

        static func applyPersistentState(
            _ state: PersistentState,
            to config: inout Config
        ) throws {
            guard state.schemaVersion == PersistentState.currentSchemaVersion else {
                throw ReplayError.message("unsupported emulator state schema")
            }
            guard state.v1Version == config.version else {
                throw ReplayError.message("emulator state belongs to a different V1 version")
            }
            guard let mode = ModeGlyph(rawValue: state.mode) else {
                throw ReplayError.message("emulator state contains an invalid mode")
            }
            guard state.mainVolume <= 9,
                  state.mutedVolume <= 9,
                  state.savedMainVolume <= 9,
                  state.savedMutedVolume <= 9 else {
                throw ReplayError.message("emulator state contains an invalid volume")
            }
            guard state.userBytes.count == 6 else {
                throw ReplayError.message("emulator state must contain six user bytes")
            }
            guard validPersistentSweepDefinitions(state.sweepDefinitions) else {
                throw ReplayError.message("emulator state contains invalid sweep definitions")
            }

            config.mode = mode
            config.mainVolume = state.mainVolume
            config.mutedVolume = state.mutedVolume
            config.savedMainVolume = state.savedMainVolume
            config.savedMutedVolume = state.savedMutedVolume
            config.userBytes = normalizedUserBytes(state.userBytes, version: config.version)
            config.sweepDefinitions = state.sweepDefinitions
        }

        /// Apply the modeled Gen2 report filter to detector-authored stimulus.
        /// Band enables remain independent from Custom Frequencies; when a
        /// priority row is removed, the first retained row becomes priority so
        /// the emitted alert table remains structurally valid.
        func projectedSample(_ sample: TimedSample) -> TimedSample {
            var retained = sample.alerts.filter { alert in
                if alert.band.mask == V1.Band.x.mask {
                    return userBytes.bytes[0] & 0x01 != 0
                }
                if alert.band.mask == V1.Band.k.mask {
                    guard userBytes.bytes[0] & 0x02 != 0 else { return false }
                    return !customFrequenciesEnabled || customDefinitionsContain(alert.frequencyMHz)
                }
                if alert.band.mask == V1.Band.ka.mask {
                    guard userBytes.bytes[0] & 0x04 != 0 else { return false }
                    return !customFrequenciesEnabled || customDefinitionsContain(alert.frequencyMHz)
                }
                if alert.band.mask == V1.Band.laser.mask {
                    return userBytes.bytes[0] & 0x08 != 0
                }
                if alert.band.mask == V1.Band.ku.mask {
                    return userBytes.bytes[0] & 0x80 == 0
                }
                return true
            }
            if !retained.isEmpty && !retained.contains(where: { $0.isPriority }) {
                retained = retained.enumerated().map { index, alert in
                    alert.withPriority(index == 0)
                }
            }
            return sample.replacingAlerts(retained)
        }

        /// End one CoreBluetooth transport lifetime without resetting the
        /// emulated detector's persistent control/user settings.
        mutating func resetTransport() {
            subscriptions.removeAll(keepingCapacity: true)
            receiveBuffer.removeAll(keepingCapacity: true)
            pendingPackets.removeAll(keepingCapacity: true)
            pendingSweepDefinitions.removeAll(keepingCapacity: true)
            alertDataRequested = false
            rejectedMaxSweepIndexOnce = false
            reportedSweepDefinitionsBusyOnce = false
        }

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
                pendingSweepDefinitions.removeAll(keepingCapacity: true)
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
            pendingPackets.append(contentsOf: V1.drainFrames(from: &receiveBuffer))
            // `drainFrames` leaves at most one incomplete <=64-byte frame. Keep
            // a defensive residual bound without discarding complete frames
            // from a large coalesced write before they have been decoded.
            if receiveBuffer.count > Session.maximumResidualByteCount {
                receiveBuffer.removeFirst(
                    receiveBuffer.count - Session.maximumResidualByteCount
                )
            }
        }

        mutating func nextOutcome() -> CommandOutcome? {
            guard !pendingPackets.isEmpty else { return nil }
            return decide(pendingPackets.removeFirst())
        }

        private mutating func decide(_ packet: InboundPacket) -> CommandOutcome {
            // This ingress models commands authored by V1Simple. DA is the V1
            // destination and E6 is V1Simple's origin; E6 is not asserted as a
            // universal origin for every possible V1 client.
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
                    checksum: config.outboundChecksum
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
                        main: controlState.mainVolume,
                        muted: controlState.mutedVolume,
                        savedMain: controlState.savedMainVolume,
                        savedMuted: controlState.savedMutedVolume,
                        checksum: config.outboundChecksum
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
                        checksum: config.outboundChecksum
                    )
                ))]

            case PacketID.reqSweepSections.rawValue:
                guard packet.payload.isEmpty else {
                    return rejectUnexpectedPayload(packet)
                }
                effects = [.reply(ReplyDecision(
                    channel: .displayShort,
                    bytes: V1.sweepSectionsPacket(
                        header: config.header,
                        checksum: config.outboundChecksum
                    )
                ))]

            case PacketID.reqMaxSweepIndex.rawValue:
                guard packet.payload.isEmpty else {
                    return rejectUnexpectedPayload(packet)
                }
                if !rejectedMaxSweepIndexOnce {
                    rejectedMaxSweepIndexOnce = true
                    effects = [.reply(ReplyDecision(
                        channel: .displayShort,
                        bytes: V1.requestNotProcessedPacket(
                            header: config.header,
                            requestID: PacketID.reqMaxSweepIndex.rawValue,
                            checksum: config.outboundChecksum
                        )
                    ))]
                } else {
                    effects = [.reply(ReplyDecision(
                        channel: .displayShort,
                        bytes: V1.maxSweepIndexPacket(
                            header: config.header,
                            checksum: config.outboundChecksum
                        )
                    ))]
                }

            case PacketID.reqAllSweepDefinitions.rawValue:
                guard packet.payload.isEmpty else {
                    return rejectUnexpectedPayload(packet)
                }
                var replies: [Effect] = []
                if !reportedSweepDefinitionsBusyOnce {
                    reportedSweepDefinitionsBusyOnce = true
                    replies.append(.reply(ReplyDecision(
                        channel: .displayShort,
                        bytes: V1.busyPacket(
                            requestIDs: [PacketID.reqAllSweepDefinitions.rawValue],
                            checksum: config.outboundChecksum
                        )
                    )))
                }
                replies.append(contentsOf: sweepDefinitions.map { definition in
                    .reply(ReplyDecision(
                        channel: .displayShort,
                        bytes: V1.sweepDefinitionPacket(
                            header: config.header,
                            index: definition.index,
                            lowerMHz: definition.lowerMHz,
                            upperMHz: definition.upperMHz,
                            checksum: config.outboundChecksum
                        )
                    ))
                })
                effects = replies

            case PacketID.reqWriteSweepDefinition.rawValue:
                guard packet.payload.count == 5 else {
                    return rejectUnexpectedPayload(packet)
                }
                let indexByte = packet.payload[0]
                let index = indexByte & 0x3F
                let commit = indexByte & 0x40 != 0
                let upper = UInt16(packet.payload[1]) << 8 | UInt16(packet.payload[2])
                let lower = UInt16(packet.payload[3]) << 8 | UInt16(packet.payload[4])
                let definition = SweepDefinition(index: index, lowerMHz: lower, upperMHz: upper)
                pendingSweepDefinitions[index] = definition
                if commit {
                    let result = commitPendingSweepDefinitions()
                    effects = [.reply(ReplyDecision(
                        channel: .displayShort,
                        bytes: V1.sweepWriteResultPacket(
                            header: config.header,
                            result: result,
                            checksum: config.outboundChecksum
                        )
                    ))]
                } else {
                    effects = []
                }

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
                guard packet.payload.isEmpty else {
                    return rejectUnexpectedPayload(packet)
                }
                effects = [.muteChanged(true)]

            case PacketID.muteOff.rawValue:
                guard packet.payload.isEmpty else {
                    return rejectUnexpectedPayload(packet)
                }
                effects = [.muteChanged(false)]

            case PacketID.turnOffDisplay.rawValue:
                guard packet.payload.isEmpty
                        || packet.payload == [0x00]
                        || packet.payload == [0x01] else {
                    return rejectUnexpectedPayload(packet)
                }
                effects = [.displayPowerChanged(false)]

            case PacketID.turnOnDisplay.rawValue:
                guard packet.payload.isEmpty else {
                    return rejectUnexpectedPayload(packet)
                }
                effects = [.displayPowerChanged(true)]

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
                let regionChanged = (userBytes.bytes[1] ^ stored[1]) & 0x01 != 0
                _ = userBytes.write(stored)
                if regionChanged {
                    sweepDefinitions = Session.defaultSweepDefinitions
                    pendingSweepDefinitions.removeAll(keepingCapacity: true)
                }
                effects = [.userBytesStored(stored)]

            case PacketID.changeMode.rawValue:
                guard packet.payload.count == 1 else {
                    return rejectUnexpectedPayload(packet)
                }
                guard let mode = ModeGlyph.commandValue(packet.payload[0]) else {
                    return CommandOutcome(packet: packet, effects: [
                        .rejected(.invalidModeValue(packet.payload[0]))
                    ])
                }
                controlState.mode = mode
                effects = [.modeChanged(mode)]

            case PacketID.reqWriteVolume.rawValue:
                guard packet.payload.count == 3 else {
                    return rejectUnexpectedPayload(packet)
                }
                let main = packet.payload[0]
                let muted = packet.payload[1]
                guard main <= 9 && muted <= 9 else {
                    return CommandOutcome(packet: packet, effects: [
                        .rejected(.invalidVolume(main: main, muted: muted))
                    ])
                }
                controlState.mainVolume = main
                controlState.mutedVolume = muted
                // V1Simple sends aux0=00. Documented V1 versions 4.1037+ assign
                // bit 2 to saving the current pair; that compatibility branch
                // is host-modeled but not physically confirmed here. Earlier
                // versions leave saved state unchanged because their handling
                // of the then-reserved bit is unknown. Feedback and disconnect
                // policy remain outside this timing/lifecycle gate.
                if packet.payload[2] & 0x04 != 0,
                   Session.supportsVolumeSave(version: config.version) {
                    controlState.savedMainVolume = main
                    controlState.savedMutedVolume = muted
                }
                effects = [.volumeChanged(controlState)]

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

        private var customFrequenciesEnabled: Bool {
            return userBytes.bytes[1] & 0x08 == 0
        }

        private func customDefinitionsContain(_ frequencyMHz: UInt16) -> Bool {
            return sweepDefinitions.contains { definition in
                !definition.isUnused
                    && frequencyMHz >= definition.lowerMHz
                    && frequencyMHz <= definition.upperMHz
            }
        }

        private mutating func commitPendingSweepDefinitions() -> UInt8 {
            defer { pendingSweepDefinitions.removeAll(keepingCapacity: true) }
            var candidate = Session.unusedSweepDefinitions
            for index in pendingSweepDefinitions.keys.sorted() {
                guard let definition = pendingSweepDefinitions[index] else { continue }
                guard Int(index) < candidate.count else { return index &+ 1 }
                candidate[Int(index)] = definition
            }

            for definition in candidate where !definition.isUnused {
                guard definition.lowerMHz < definition.upperMHz,
                      Session.sectionIndex(for: definition) != nil else {
                    return definition.index &+ 1
                }
            }
            let usedSections = Set(candidate.compactMap { Session.sectionIndex(for: $0) })
            // ESP 3.016 requires one definition for each Gen2-supported band.
            guard usedSections.contains(0), usedSections.contains(1) else { return 1 }
            sweepDefinitions = candidate
            return 0
        }

        private static let sweepSections: [(lower: UInt16, upper: UInt16)] = [
            (24_000, 25_000),
            (33_000, 36_000),
        ]

        private static let defaultSweepDefinitions: [SweepDefinition] = [
            SweepDefinition(index: 0, lowerMHz: 24_050, upperMHz: 24_150),
            SweepDefinition(index: 1, lowerMHz: 34_100, upperMHz: 34_200),
        ]

        private static let unusedSweepDefinitions: [SweepDefinition] = [
            SweepDefinition(index: 0, lowerMHz: 0, upperMHz: 0),
            SweepDefinition(index: 1, lowerMHz: 0, upperMHz: 0),
        ]

        private static func sectionIndex(for definition: SweepDefinition) -> Int? {
            return sweepSections.firstIndex {
                definition.lowerMHz >= $0.lower && definition.upperMHz <= $0.upper
            }
        }

        private static func validPersistentSweepDefinitions(
            _ definitions: [SweepDefinition]
        ) -> Bool {
            guard definitions.count == unusedSweepDefinitions.count else { return false }
            for (offset, definition) in definitions.enumerated() {
                guard definition.index == UInt8(offset) else { return false }
                if definition.isUnused { continue }
                guard definition.lowerMHz < definition.upperMHz,
                      sectionIndex(for: definition) != nil else { return false }
            }
            let usedSections = Set(definitions.compactMap { sectionIndex(for: $0) })
            return usedSections.contains(0) && usedSections.contains(1)
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
            guard let build = gen2Build(version: version) else {
                return true
            }
            return build >= 1039
        }

        private static func supportsVolumeSave(version: String) -> Bool {
            guard let build = gen2Build(version: version) else {
                return false
            }
            return build >= 1037
        }

        private static func gen2Build(version: String) -> Int? {
            let components = version
                .filter { $0.isNumber || $0 == "." }
                .split(separator: ".", maxSplits: 1)
            guard components.count == 2,
                  let major = Int(components[0]),
                  let build = Int(components[1]),
                  major == 4 else {
                return nil
            }
            return build
        }
    }
}

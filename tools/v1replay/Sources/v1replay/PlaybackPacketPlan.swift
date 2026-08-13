import Foundation

extension V1 {

    /// Pure packet selection for one replay sample. CoreBluetooth subscription,
    /// backpressure, segmentation, and delivery remain adapter concerns.
    struct PlaybackPacketPlan {

        enum Kind: Equatable {
            case alertRow(index: Int, count: Int)
            case displayFrame
        }

        struct Emission: Equatable {
            let kind: Kind
            let channel: ReplyChannel
            let bytes: [UInt8]
        }

        let alertTablePackets: [[UInt8]]
        let displayPacket: [UInt8]
        let emissions: [Emission]

        init(sample: TimedSample,
             controlState: Session.ControlState,
             displayOn: Bool,
             muted: Bool,
             blinkBogey: Bool,
             blinkArrow: Bool,
             header: Header = .broadcastInformation,
             checksum: Bool = true,
             includeAlertTable: Bool = true) {
            let includeModeBits = !header.usesFixtureCompatibility
            let rows: [(Kind, [UInt8])]
            if sample.alerts.isEmpty {
                rows = [(
                    .alertRow(index: 0, count: 0),
                    AlertRow.empty().packet(header: header, checksum: checksum)
                )]
            } else {
                rows = sample.alerts.enumerated().map { index, alert in
                    let row = AlertRow.row(
                        index: index + 1,
                        count: sample.alerts.count,
                        bars: alert.strength,
                        band: alert.band,
                        direction: alert.direction,
                        frequencyMHz: alert.frequencyMHz,
                        priority: alert.isPriority
                    )
                    return (
                        .alertRow(index: index + 1, count: sample.alerts.count),
                        row.packet(header: header, checksum: checksum)
                    )
                }
            }

            let frame: DisplayFrame
            if let priority = sample.priorityAlert {
                frame = .alerting(
                    bars: priority.strength,
                    band: priority.band,
                    direction: priority.direction,
                    bogeyCount: sample.alerts.count,
                    mode: controlState.mode,
                    muted: muted,
                    volume: controlState.displayVolume,
                    displayOn: displayOn,
                    blinkPlane: blinkBogey,
                    blinkArrow: blinkArrow,
                    includeModeBits: includeModeBits
                )
            } else {
                frame = .idle(
                    mode: controlState.mode,
                    volume: controlState.displayVolume,
                    displayOn: displayOn,
                    softMuted: muted,
                    includeModeBits: includeModeBits
                )
            }

            alertTablePackets = rows.map { $0.1 }
            displayPacket = frame.packet(header: header, checksum: checksum)

            var ordered: [Emission] = []
            if includeAlertTable {
                ordered.append(contentsOf: rows.map { kind, bytes in
                    Emission(kind: kind, channel: .displayShort, bytes: bytes)
                })
            }
            ordered.append(Emission(
                kind: .displayFrame,
                channel: .displayShort,
                bytes: displayPacket
            ))
            emissions = ordered
        }

        static func idleDisplayPacket(controlState: Session.ControlState,
                                      displayOn: Bool,
                                      muted: Bool,
                                      header: Header = .broadcastInformation,
                                      checksum: Bool = true) -> [UInt8] {
            return DisplayFrame.idle(
                mode: controlState.mode,
                volume: controlState.displayVolume,
                displayOn: displayOn,
                softMuted: muted,
                includeModeBits: !header.usesFixtureCompatibility
            ).packet(header: header, checksum: checksum)
        }

        static func clearAlertPacket(header: Header = .broadcastInformation,
                                     checksum: Bool = true) -> [UInt8] {
            return AlertRow.empty().packet(header: header, checksum: checksum)
        }

        /// The reconnect preflight has one deliberately bounded stimulus: a
        /// count-zero alert row on the short notification channel. The player
        /// sends this plan once after startup readiness, then stays quiet.
        static func handshakeOnlyEmissions(
            header: Header = .broadcastInformation,
            checksum: Bool = true
        ) -> [Emission] {
            return [Emission(
                kind: .alertRow(index: 0, count: 0),
                channel: .displayShort,
                bytes: clearAlertPacket(header: header, checksum: checksum)
            )]
        }
    }
}

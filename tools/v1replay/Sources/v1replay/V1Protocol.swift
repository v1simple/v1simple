import Foundation

// =============================================================================
// V1 Gen2 ESP packet protocol
//
// Every constant here is transcribed from the firmware that will consume these
// packets, not from memory:
//   v1simple/include/config.h            — UUIDs, packet IDs, framing bytes
//   v1simple/src/packet_parser.cpp       — infDisplayData (0x31) payload layout
//   v1simple/src/packet_parser_alerts.cpp— respAlertData (0x43) row layout,
//                                          band/arrow bits, RSSI→bar thresholds
//   v1simple/src/ble_commands.cpp        — checksum + outbound command framing
// =============================================================================

enum V1 {

    // MARK: - UUIDs

    static let serviceUUID = "92A0AFF4-9E05-11E2-AA59-F23C91AEC05E"

    /// Four core characteristics plus two compatibility additions exposed by
    /// the emulator.
    /// Suffix is shared: -9E05-11E2-AA59-F23C91AEC05E
    static func characteristicUUID(_ short: String) -> String {
        return "92A0" + short + "-9E05-11E2-AA59-F23C91AEC05E"
    }

    static let displayShortUUID = characteristicUUID("B2CE")  // notify: short packets, including display/version
    static let displayLongUUID = characteristicUUID("B4E0")   // notify: long packets, including alert tables
    static let notifyAltUUID = characteristicUUID("BCE0")     // notify: compatibility stub
    static let commandUUID = characteristicUUID("B6D4")       // write-no-response: commands
    static let commandLongUUID = characteristicUUID("B8D2")   // write-no-response: long commands
    static let commandAltUUID = characteristicUUID("BAD4")    // write / write-no-response

    // MARK: - Framing

    static let packetStart: UInt8 = 0xAA
    static let packetEnd: UInt8 = 0xAB

    /// Packet header (bytes 1 and 2: destination, origin).
    /// The firmware's parser ignores both, but real captures and companion apps
    /// do not, so default to the correct V1→app direction.
    struct Header {
        let dest: UInt8
        let src: UInt8

        /// V1 → remote app. dest = 0xD0 + ESP_PACKET_REMOTE(0x06), src = 0xE0 + ESP_PACKET_ORIGIN_V1(0x0A).
        /// This is the V1-to-app directional default.
        static let v1ToApp = Header(dest: 0xD6, src: 0xEA)

        /// Compatibility convention used by repository packet fixtures. Select
        /// it explicitly with `--header draft` when fixture parity is required.
        static let repoConvention = Header(dest: 0xDA, src: 0xE4)

        /// Historical alias.
        static let draft = Header.repoConvention

        static func named(_ name: String) -> Header? {
            switch name.lowercased() {
            case "v1", "v1toapp", "spec": return .v1ToApp
            case "draft", "crib", "legacy", "repo": return .repoConvention
            default: return nil
            }
        }
    }

    // MARK: - Packet IDs (config.h)

    enum PacketID: UInt8 {
        case reqVersion = 0x01
        case respVersion = 0x02
        case reqUserBytes = 0x11
        case respUserBytes = 0x12
        case reqWriteUserBytes = 0x13
        case displayData = 0x31       // infDisplayData
        case turnOffDisplay = 0x32
        case turnOnDisplay = 0x33
        case muteOn = 0x34
        case muteOff = 0x35
        case changeMode = 0x36
        case reqWriteVolume = 0x39
        case reqAllVolume = 0x3C
        case respAllVolume = 0x3D
        case reqStartAlertData = 0x41
        case reqStopAlertData = 0x42
        case alertData = 0x43         // respAlertData
    }

    // MARK: - Band / direction bits

    struct Band {
        let mask: UInt8
        let name: String

        static let laser = Band(mask: 0x01, name: "laser")
        static let ka = Band(mask: 0x02, name: "ka")
        static let k = Band(mask: 0x04, name: "k")
        static let x = Band(mask: 0x08, name: "x")
        static let ku = Band(mask: 0x10, name: "ku")

        static func named(_ raw: String) -> Band? {
            switch raw.lowercased() {
            case "laser", "l": return .laser
            case "ka": return .ka
            case "k": return .k
            case "x": return .x
            case "ku": return .ku
            default: return nil
            }
        }
    }

    enum Direction: UInt8 {
        case front = 0x20
        case side = 0x40
        case rear = 0x80

        static func named(_ raw: String) -> Direction {
            switch raw.uppercased() {
            case "S", "SIDE": return .side
            case "R", "REAR": return .rear
            default: return .front
            }
        }

        var label: String {
            switch self {
            case .front: return "FRONT"
            case .side: return "SIDE"
            case .rear: return "REAR"
            }
        }
    }

    /// Display image1 bit 4 — mute. (packet_parser.cpp: `rawMuteBit = image1 & 0x10`)
    static let muteBit: UInt8 = 0x10

    // MARK: - auxData0 bits (packet_parser.cpp parseDisplayData)

    static let aux0SoftMute: UInt8 = 0x01
    static let aux0SystemStatus: UInt8 = 0x04   // V1 actively searching — REQUIRED,
                                                // the parser blanks bands+arrows without it
    static let aux0DisplayOn: UInt8 = 0x08

    // MARK: - Bogey counter 7-segment glyphs

    /// Reverse of `decodeBogeyCounterByte` in packet_parser.cpp.
    static func bogeyGlyph(forCount count: Int) -> UInt8 {
        switch count {
        case 0: return 0x3F  // '0'
        case 1: return 0x06  // '1'
        case 2: return 0x5B  // '2'
        case 3: return 0x4F  // '3'
        case 4: return 0x66  // '4'
        case 5: return 0x6D  // '5'
        case 6: return 0x7D  // '6'
        case 7: return 0x07  // '7'
        case 8: return 0x7F  // '8'
        default: return 0x6F // '9' (V1 tops out well before this)
        }
    }

    /// Mode glyphs — what the V1 shows in the bogey window when nothing is alerting.
    /// (packet_parser.cpp decodeMode)
    enum ModeGlyph: UInt8 {
        case allBogeys = 0x77       // 'A'
        case logic = 0x18           // 'l'
        case advancedLogic = 0x38   // 'L'
        case customSweeps = 0x39    // 'C'
        case euroKaOnly = 0x1C      // 'u'
        case euroKaPhoto = 0x3E     // 'U'

        static func named(_ raw: String) -> ModeGlyph? {
            switch raw.lowercased() {
            case "a", "all", "allbogeys": return .allBogeys
            case "l", "logic": return .logic
            case "adv", "advanced", "advancedlogic": return .advancedLogic
            case "c", "custom": return .customSweeps
            case "u", "eurokaonly": return .euroKaOnly
            case "euro", "eurokaphoto": return .euroKaPhoto
            default: return nil
            }
        }
    }

    // MARK: - Signal strength

    /// The main meter is a literal mirror of the V1's LED bitmap: N bars means
    /// the N low bits are set, with no rescaling.
    static func ledBitmap(bars: Int) -> UInt8 {
        let clamped = max(0, min(8, bars))
        return UInt8(truncatingIfNeeded: (1 << clamped) - 1)
    }

    /// Inverse of `PacketParser::mapStrengthToBars` — pick a raw RSSI byte that
    /// lands squarely inside the bucket for the requested bar count, so a
    /// round-trip through the firmware's own thresholds returns the same number.
    static func rawStrength(bars: Int, band: Band) -> UInt8 {
        let n = max(0, min(8, bars))
        if n == 0 { return 0x00 }
        if band.mask == Band.laser.mask { return 0xFF }  // laser is always 8 bars

        switch band.mask {
        case Band.ka.mask:
            // thresholds: 8≥0xBA 7≥0xB3 6≥0xAC 5≥0xA5 4≥0x9E 3≥0x97 2≥0x90 1≥0x01
            let table: [UInt8] = [0x00, 0x80, 0x93, 0x9A, 0xA1, 0xA8, 0xAF, 0xB6, 0xBD]
            return table[n]
        case Band.x.mask:
            // thresholds: 8≥0xD0 7≥0xC5 6≥0xBD 5≥0xB4 4≥0xAA 3≥0xA0 2≥0x96 1≥0x01
            let table: [UInt8] = [0x00, 0x80, 0x9B, 0xA5, 0xAF, 0xB8, 0xC1, 0xCA, 0xD8]
            return table[n]
        default:
            // K and Ku share one scale:
            // 8≥0xC2 7≥0xB8 6≥0xAE 5≥0xA4 4≥0x9A 3≥0x90 2≥0x88 1≥0x01
            let table: [UInt8] = [0x00, 0x80, 0x8C, 0x95, 0x9F, 0xA9, 0xB3, 0xBD, 0xC6]
            return table[n]
        }
    }

    // MARK: - Framing helper

    /// Build a framed ESP packet.
    ///
    /// Layout: AA | dest | src | id | len | payload… | [checksum] | AB
    /// `len` counts the payload bytes *plus* the checksum byte — matching
    /// ble_commands.cpp, where a payload-free command declares len = 0x01.
    static func frame(header: Header,
                      id: UInt8,
                      payload: [UInt8],
                      checksum: Bool = true) -> [UInt8] {
        var packet: [UInt8] = [
            packetStart,
            header.dest,
            header.src,
            id,
            UInt8(truncatingIfNeeded: payload.count + (checksum ? 1 : 0))
        ]
        packet.append(contentsOf: payload)
        if checksum {
            var sum: UInt8 = 0
            for byte in packet { sum = sum &+ byte }
            packet.append(sum)
        }
        packet.append(packetEnd)
        return packet
    }

    // MARK: - infDisplayData (0x31)

    /// The eight payload bytes of a display packet, named as the parser names them.
    struct DisplayFrame {
        var bogeyImage1: UInt8 = ModeGlyph.advancedLogic.rawValue
        var bogeyImage2: UInt8 = ModeGlyph.advancedLogic.rawValue
        var ledBitmap: UInt8 = 0x00
        var image1: UInt8 = 0x00
        var image2: UInt8 = 0x00
        var aux0: UInt8 = V1.aux0SystemStatus | V1.aux0DisplayOn
        var aux1: UInt8 = 0x00
        var aux2: UInt8 = 0x40   // upper nibble = main volume, lower = muted volume

        var payload: [UInt8] {
            return [bogeyImage1, bogeyImage2, ledBitmap, image1, image2, aux0, aux1, aux2]
        }

        func packet(header: Header, checksum: Bool) -> [UInt8] {
            return V1.frame(header: header,
                            id: PacketID.displayData.rawValue,
                            payload: payload,
                            checksum: checksum)
        }

        /// Idle: no alert, mode glyph in the bogey window, meter dark.
        static func idle(mode: ModeGlyph, volume: UInt8, displayOn: Bool, softMuted: Bool) -> DisplayFrame {
            var f = DisplayFrame()
            f.bogeyImage1 = mode.rawValue
            f.bogeyImage2 = mode.rawValue
            f.ledBitmap = 0x00
            f.image1 = 0x00
            f.image2 = 0x00
            f.aux0 = V1.aux0SystemStatus
                | (displayOn ? V1.aux0DisplayOn : 0)
                | (softMuted ? V1.aux0SoftMute : 0)
            f.aux2 = volume
            return f
        }

        /// One alerting frame: N bars, one band, one arrow.
        ///
        /// `blinkPlane` sets the bogey counter's image2 to 0x00, matching
        /// test/test_protocol_spec_conformance's fixture. Default is off because
        /// the firmware gates a separate blink-refresh repaint on
        /// `bogeyCounterByte != bogeyCounterByte2`. Keeping the glyph steady
        /// makes packet-to-paint tests deterministic.
        static func alerting(bars: Int,
                             band: Band,
                             direction: Direction,
                             bogeyCount: Int,
                             muted: Bool,
                             volume: UInt8,
                             displayOn: Bool,
                             blinkPlane: Bool = false,
                             blinkArrow: Bool = false) -> DisplayFrame {
            var f = DisplayFrame()
            f.bogeyImage1 = V1.bogeyGlyph(forCount: bogeyCount)
            f.bogeyImage2 = blinkPlane ? 0x00 : f.bogeyImage1
            f.ledBitmap = V1.ledBitmap(bars: bars)
            var image = band.mask | direction.rawValue
            if muted { image |= V1.muteBit }
            f.image1 = image
            f.image2 = blinkArrow ? (image & ~direction.rawValue) : image
            f.aux0 = V1.aux0SystemStatus
                | (displayOn ? V1.aux0DisplayOn : 0)
                | (muted ? V1.aux0SoftMute : 0)
            f.aux2 = volume
            return f
        }
    }

    // MARK: - respAlertData (0x43)

    /// One row of the V1 alert table. Seven payload bytes.
    struct AlertRow {
        var index: UInt8          // 1-based (the firmware also accepts 0-based)
        var count: UInt8
        var frequencyMHz: UInt16
        var frontRaw: UInt8
        var rearRaw: UInt8
        var bandArrow: UInt8      // low 5 bits = band, high 3 = arrow
        var aux0: UInt8           // bit7 priority, bit6 junk, low nibble photo type

        var payload: [UInt8] {
            return [
                UInt8(truncatingIfNeeded: (Int(index) << 4) | Int(count & 0x0F)),
                UInt8(truncatingIfNeeded: frequencyMHz >> 8),
                UInt8(truncatingIfNeeded: frequencyMHz & 0x00FF),
                frontRaw,
                rearRaw,
                bandArrow,
                aux0
            ]
        }

        func packet(header: Header, checksum: Bool) -> [UInt8] {
            return V1.frame(header: header,
                            id: PacketID.alertData.rawValue,
                            payload: payload,
                            checksum: checksum)
        }

        /// Build one row in a complete, one-based alert table.
        static func row(index: Int,
                        count: Int,
                        bars: Int,
                        band: Band,
                        direction: Direction,
                        frequencyMHz: UInt16,
                        priority: Bool,
                        junk: Bool = false,
                        photoType: UInt8 = 0) -> AlertRow {
            precondition(count >= 1 && count <= 15, "alert table count must be 1...15")
            precondition(index >= 1 && index <= count, "alert table index must be 1...count")

            let raw = V1.rawStrength(bars: bars, band: band)
            var aux0: UInt8 = photoType & 0x0F
            if priority { aux0 |= 0x80 }
            if junk { aux0 |= 0x40 }
            return AlertRow(
                index: UInt8(index),
                count: UInt8(count),
                frequencyMHz: frequencyMHz,
                frontRaw: direction == .rear ? 0x00 : raw,
                rearRaw: direction == .rear ? raw : 0x00,
                bandArrow: band.mask | direction.rawValue,
                aux0: aux0
            )
        }

        static func single(bars: Int,
                           band: Band,
                           direction: Direction,
                           frequencyMHz: UInt16,
                           priority: Bool = true,
                           junk: Bool = false,
                           photoType: UInt8 = 0) -> AlertRow {
            return row(index: 1,
                       count: 1,
                       bars: bars,
                       band: band,
                       direction: direction,
                       frequencyMHz: frequencyMHz,
                       priority: priority,
                       junk: junk,
                       photoType: photoType)
        }

        /// Empty table — tells the firmware every alert is gone.
        static func empty() -> AlertRow {
            return AlertRow(index: 0, count: 0, frequencyMHz: 0,
                            frontRaw: 0, rearRaw: 0, bandArrow: 0, aux0: 0)
        }
    }

    // MARK: - Responses to commands the firmware sends us

    struct UserBytesStore {
        private(set) var bytes: [UInt8]

        init(_ initial: [UInt8] = []) {
            bytes = Array(initial.prefix(6))
            while bytes.count < 6 { bytes.append(0x00) }
        }

        mutating func write(_ payload: [UInt8]) -> Bool {
            guard payload.count == 6 else { return false }
            bytes = payload
            return true
        }
    }

    /// respVersion, e.g. "4.1038" → payload "v4.1038" (7 ASCII bytes).
    static func versionPacket(header: Header, version: String, checksum: Bool) -> [UInt8] {
        let digits = version.filter { $0.isNumber || $0 == "." }
        var payload: [UInt8] = [UInt8(ascii: "v")]
        payload.append(contentsOf: Array(digits.utf8))
        while payload.count < 7 { payload.append(UInt8(ascii: "0")) }
        if payload.count > 7 { payload = Array(payload.prefix(7)) }
        return frame(header: header, id: PacketID.respVersion.rawValue, payload: payload, checksum: checksum)
    }

    /// respAllVolume — exactly four bytes: main, muted, savedMain, savedMuted.
    static func allVolumePacket(header: Header, main: UInt8, muted: UInt8, checksum: Bool) -> [UInt8] {
        return frame(header: header,
                     id: PacketID.respAllVolume.rawValue,
                     payload: [main, muted, main, muted],
                     checksum: checksum)
    }

    /// respUserBytes — six user bytes.
    static func userBytesPacket(header: Header, bytes: [UInt8], checksum: Bool) -> [UInt8] {
        var payload = bytes
        while payload.count < 6 { payload.append(0x00) }
        return frame(header: header,
                     id: PacketID.respUserBytes.rawValue,
                     payload: Array(payload.prefix(6)),
                     checksum: checksum)
    }

    /// Bare acknowledgement carrying the same packet ID and no payload.
    ///
    /// The parser rejects anything shorter than 7 bytes outright
    /// (packet_parser.cpp parseInternal), and a payload-free frame with no
    /// checksum byte is only 6 — so pad rather than emit a packet that will be
    /// silently dropped.
    static func ackPacket(header: Header, id: UInt8, checksum: Bool) -> [UInt8] {
        if checksum {
            return frame(header: header, id: id, payload: [], checksum: true)
        }
        return frame(header: header, id: id, payload: [0x00], checksum: false)
    }

    // MARK: - Inbound frame decoding (commands from v1simple)

    struct InboundPacket {
        let id: UInt8
        let payload: [UInt8]
        let raw: [UInt8]
    }

    enum ReplyChannel: Equatable {
        case displayShort
        case displayLong
    }

    struct ReplyDecision: Equatable {
        let channel: ReplyChannel
        let bytes: [UInt8]
    }

    /// Pure request-to-reply policy used by the CoreBluetooth peripheral.
    /// Physical notification delivery remains the peripheral's responsibility.
    static func replyDecision(for request: InboundPacket,
                              version: String,
                              header: Header = .v1ToApp,
                              checksum: Bool = true) -> ReplyDecision? {
        guard request.id == PacketID.reqVersion.rawValue,
              request.payload.isEmpty,
              request.raw.count >= 7,
              request.raw[1] == 0xDA,
              request.raw[2] == 0xE6 else {
            return nil
        }

        return ReplyDecision(
            channel: .displayShort,
            bytes: versionPacket(header: header, version: version, checksum: checksum)
        )
    }

    /// Pull complete AA…AB frames out of a rolling buffer. Leaves any partial
    /// tail in place — BLE writes can be split across characteristic writes.
    static func drainFrames(from buffer: inout [UInt8]) -> [InboundPacket] {
        var packets: [InboundPacket] = []

        while true {
            guard let startIndex = buffer.firstIndex(of: packetStart) else {
                buffer.removeAll()
                break
            }
            if startIndex > 0 { buffer.removeFirst(startIndex) }
            guard buffer.count >= 6 else { break }

            let declaredLength = Int(buffer[4])
            let total = 6 + declaredLength           // AA dest src id len …payload+checksum… AB
            guard total >= 6, total <= 64 else {
                buffer.removeFirst()                  // nonsense length — resync
                continue
            }
            guard buffer.count >= total else { break } // wait for the rest

            if buffer[total - 1] != packetEnd {
                buffer.removeFirst()                  // misframed — resync
                continue
            }

            let raw = Array(buffer[0..<total])
            let payloadEnd = total - 2                // drop checksum and end byte
            let payload = payloadEnd > 5 ? Array(raw[5..<payloadEnd]) : []
            packets.append(InboundPacket(id: raw[3], payload: payload, raw: raw))
            buffer.removeFirst(total)
        }

        return packets
    }

    static func name(forPacketID id: UInt8) -> String {
        switch id {
        case 0x01: return "reqVersion"
        case 0x02: return "respVersion"
        case 0x11: return "reqUserBytes"
        case 0x12: return "respUserBytes"
        case 0x13: return "reqWriteUserBytes"
        case 0x31: return "infDisplayData"
        case 0x32: return "reqTurnOffMainDisplay"
        case 0x33: return "reqTurnOnMainDisplay"
        case 0x34: return "reqMuteOn"
        case 0x35: return "reqMuteOff"
        case 0x36: return "reqChangeMode"
        case 0x39: return "reqWriteVolume"
        case 0x3C: return "reqAllVolume"
        case 0x3D: return "respAllVolume"
        case 0x41: return "reqStartAlertData"
        case 0x42: return "reqStopAlertData"
        case 0x43: return "respAlertData"
        default: return String(format: "0x%02X", id)
        }
    }
}

// MARK: - Hex helpers

extension Array where Element == UInt8 {
    var hexString: String {
        return map { String(format: "%02X", $0) }.joined(separator: " ")
    }
}

import Foundation

// =============================================================================
// v1replay — a Mac that pretends to be a Valentine One Gen2 over BLE.
//
//   v1replay demo
//   v1replay bench
//   v1replay play /path/outside/any/git/replay-input.json
//   v1replay export --synthetic --format lightblue
//   v1replay export --bench --format csv
//   v1replay crib
//   v1replay idle
// =============================================================================

let toolVersion = "1.3.0"

// MARK: - Minimal argument parsing (no external packages: this must build offline)

struct Arguments {
    let command: String
    let positional: [String]
    private let flags: [String: String]

    static let booleanFlags: Set<String> = [
        "loop", "paused", "no-alerts", "always-alerts", "no-wait",
        "no-checksum", "log-packets", "blink-bogey", "blink-arrow", "synthetic", "bench",
        "exit-on-complete", "machine-events", "handshake-only", "help", "h", "version"
    ]

    init(_ argv: [String]) {
        var rest = argv
        var command = "play"
        if let first = rest.first, !first.hasPrefix("-") {
            let known = ["play", "demo", "bench", "export", "crib", "idle", "proxy", "help", "version"]
            if known.contains(first) {
                command = first
                rest.removeFirst()
            }
        }

        var positional: [String] = []
        var flags: [String: String] = [:]
        var index = 0
        while index < rest.count {
            let token = rest[index]
            if token.hasPrefix("--") || (token.hasPrefix("-") && token.count == 2) {
                let key = String(token.drop(while: { $0 == "-" }))
                if Arguments.booleanFlags.contains(key) {
                    flags[key] = "true"
                    index += 1
                } else if index + 1 < rest.count, !rest[index + 1].hasPrefix("--") {
                    flags[key] = rest[index + 1]
                    index += 2
                } else {
                    flags[key] = "true"
                    index += 1
                }
            } else {
                positional.append(token)
                index += 1
            }
        }

        self.command = command
        self.positional = positional
        self.flags = flags
    }

    func string(_ key: String, _ fallback: String) -> String { return flags[key] ?? fallback }
    func optionalString(_ key: String) -> String? { return flags[key] }
    func bool(_ key: String) -> Bool { return flags[key] == "true" }
    func double(_ key: String, _ fallback: Double) -> Double {
        guard let raw = flags[key], let value = Double(raw) else { return fallback }
        return value
    }
    func int(_ key: String, _ fallback: Int) -> Int {
        guard let raw = flags[key], let value = Int(raw) else { return fallback }
        return value
    }
}

/// Thread-safe boolean, for the key handler telling the main loop to quit.
final class Flag {
    private let lock = NSLock()
    private var value = false
    func set() { lock.lock(); value = true; lock.unlock() }
    var isSet: Bool { lock.lock(); defer { lock.unlock() }; return value }
}

let args = Arguments(Array(CommandLine.arguments.dropFirst()))
let console = Console()

// MARK: - Shared option construction

func makeHeader() throws -> V1.Header {
    // Targeted replies use the V1→app direction. Repository fixture parity
    // remains available as an explicit compatibility option.
    let raw = args.string("header", "v1")
    guard let header = V1.Header.named(raw) else {
        throw ReplayError.message("unknown --header '\(raw)' (use v1 or draft)")
    }
    return header
}

func makeInformationHeader() throws -> V1.Header {
    let raw = args.string("header", "v1")
    guard let header = V1.Header.informationNamed(raw) else {
        throw ReplayError.message("unknown --header '\(raw)' (use v1 or draft)")
    }
    return header
}

func makeVolumeByte() -> UInt8 {
    let raw = args.string("volume", "4,0")
    let parts = raw.split(separator: ",").compactMap { Int($0.trimmingCharacters(in: .whitespaces)) }
    let main = UInt8(clamping: parts.count > 0 ? parts[0] : 4) & 0x0F
    let muted = UInt8(clamping: parts.count > 1 ? parts[1] : 0) & 0x0F
    return (main << 4) | muted
}

func makeVolumePair() -> (UInt8, UInt8) {
    let byte = makeVolumeByte()
    return ((byte >> 4) & 0x0F, byte & 0x0F)
}

func makeMode() throws -> V1.ModeGlyph {
    let raw = args.string("mode", "advanced")
    guard let mode = V1.ModeGlyph.named(raw) else {
        throw ReplayError.message("unknown --mode '\(raw)' (all, logic, advanced, custom, euro)")
    }
    return mode
}

func makeArrowBlinkProfile(benchDefault: Bool) throws -> ArrowBlinkProfile {
    let legacyStress = args.bool("blink-arrow")
    if legacyStress && args.optionalString("blink-profile") != nil {
        throw ReplayError.message("use either --blink-profile or --blink-arrow, not both")
    }
    let raw = legacyStress
        ? ArrowBlinkProfile.stress.rawValue
        : args.string("blink-profile", benchDefault ? "scenario" : "steady")
    guard let profile = ArrowBlinkProfile.named(raw) else {
        throw ReplayError.message(
            "unknown --blink-profile '\(raw)' (use scenario, steady, or stress)"
        )
    }
    return profile
}

func loadEncounter() throws -> Encounter {
    guard let path = args.positional.first else {
        throw ReplayError.message(
            "no external replay input given; use 'v1replay demo' for generated data"
        )
    }
    var encounter = try Encounter.loadExternal(path: path,
                                               mutedWhen: args.string("muted-when", "2"))

    if let bandName = args.optionalString("band") {
        guard let band = V1.Band.named(bandName) else {
            throw ReplayError.message("unknown --band '\(bandName)'")
        }
        let samples = encounter.samples.map { sample in
            let alerts = sample.alerts.map { alert in
                ReplayAlert(band: band,
                            frequencyMHz: alert.frequencyMHz,
                            strength: alert.strength,
                            direction: alert.direction,
                            isPriority: alert.isPriority)
            }
            return TimedSample(offset: sample.offset, phase: sample.phase,
                               muted: sample.muted, alerts: alerts,
                               detectorVolume: sample.detectorVolume,
                               scenarioArrowBlink: sample.scenarioArrowBlink,
                               sourceIndex: sample.sourceIndex)
        }
        encounter = Encounter(origin: encounter.origin, samples: samples)
    }

    let rate = args.double("rate", 0)
    if rate > 0 { encounter = encounter.retimed(toHz: rate) }
    return encounter
}

func validateBenchOptions() throws {
    _ = try makeArrowBlinkProfile(benchDefault: true)
    if args.bool("synthetic") || !args.positional.isEmpty {
        throw ReplayError.message("bench cannot be combined with synthetic or external input")
    }
    if args.bool("no-alerts") {
        throw ReplayError.message("bench requires alert-table packets; remove --no-alerts")
    }
    if args.bool("no-wait") {
        throw ReplayError.message("bench waits for display and alert-data readiness; remove --no-wait")
    }
    if args.bool("always-alerts") {
        throw ReplayError.message("bench requires the firmware alert-data readiness handshake; remove --always-alerts")
    }
    if args.optionalString("rate") != nil {
        throw ReplayError.message("bench has a fixed 3 Hz cadence; remove --rate")
    }
}

func parseHandshakeNotificationHoldMilliseconds(
    _ raw: String?,
    bench: Bool,
    handshakeOnly: Bool
) throws -> Int {
    guard let raw else { return 0 }
    guard bench && handshakeOnly else {
        throw ReplayError.message(
            "--handshake-notification-hold-ms is available only with bench --handshake-only"
        )
    }
    guard let value = Int(raw),
          (0..<HandshakeNotificationHoldState.upperBoundMilliseconds).contains(value) else {
        throw ReplayError.message(
            "--handshake-notification-hold-ms must be an integer from 0 through 1999"
        )
    }
    return value
}

// MARK: - Commands

func runHelp() {
    console.print("""
    \(Ansi.bold)v1replay \(toolVersion)\(Ansi.reset) — drive v1simple with generated or external BLE stimulus

    \(Ansi.bold)USAGE\(Ansi.reset)
      v1replay demo [options]                      play an in-memory synthetic ramp
      v1replay bench [options]                     play the deterministic multi-alert bench
      v1replay play <external.json> [options]      replay private input stored outside Git
      v1replay idle [options]                      advertise and stream idle frames only
      v1replay export --synthetic [options]        print generated packets, no Bluetooth
      v1replay export --bench --format csv         print the expected bench timeline
      v1replay export <external.json> [options]    print external-input packets, no Bluetooth
      v1replay crib                                the packets for a manual LightBlue test
      v1replay help

    \(Ansi.bold)PRIVACY BOUNDARY\(Ansi.reset)
      External replay input is rejected if it is anywhere inside a Git checkout.
      Input paths and metadata are never printed. The tool writes no capture or
      export files; export output is stdout only.

    \(Ansi.bold)PLAYBACK\(Ansi.reset)
      --speed <x>          playback rate multiplier (default 1.0)
      --rate <hz>          ignore recorded timing, emit at a fixed cadence
      --loop               replay continuously
      --paused             start paused (step through with 'n')
      --exit-on-complete   stop after one complete replay (for bench automation)
      --machine-events     emit stable completion events for an external runner
      --handshake-ledger P bench-only bounded startup-handshake evidence (JSONL)
      --handshake-only     runner preflight: one clear alert row, then stay quiet
      --handshake-notification-hold-ms N
                           maximum stress hold; second START releases (0...1999; default 0)
      --no-wait            start without waiting for a central to subscribe
      --idle-lead <sec>    idle frames before the encounter (default 3)
      --idle-tail <sec>    idle frames after the encounter (default 3)
      --idle-hz <hz>       idle frame cadence (default 3)

      The bench owns its 5-second lead and 14-second tail, and waits for both
      display subscription and the firmware's alert-data request before starting.

    \(Ansi.bold)PROTOCOL\(Ansi.reset)
      --name <string>      advertised local name (default V1G-REPLAY)
      --header <v1|draft>  generated information D8 EA and targeted replies D6 EA;
                           DA E4 selects fixture compatibility for both
      --blink-bogey        bogey image2 = 00, matching test_protocol_spec_conformance.
                           Off by default: image1 != image2 switches on the firmware's
                           blink-refresh repaint, the one paint path not driven by parse
      --blink-profile <scenario|steady|stress>
                           priority-arrow blink stimulus. Bench defaults to the
                           authored scenario; other modes default to steady.
      --blink-arrow        legacy alias for --blink-profile stress
      --no-checksum        omit outbound checksums; inbound commands stay validated
      --no-alerts          display packets only, no alert table
      --always-alerts      send alert rows without waiting for reqStartAlertData
      --mode <glyph>       initial display mode: all, logic, advanced, custom, euro
      --volume <main,mute> initial current/muted volume (default 4,0)
      --v1-version <ver>   version reported to reqVersion (default 4.1038)
      --muted-when <val>   input muteState value that means muted (default 2)
      --band <ka|k|x|ku|laser>   override the input's band
      --log-packets        log every packet in and out

    \(Ansi.bold)EXPORT\(Ansi.reset)
      --format <hex|csv|lightblue>   default hex
      --synthetic                     use the generated demo instead of a file
      --bench                         use the generated bench; CSV is expected state

    \(Ansi.bold)PROXY\(Ansi.reset)  v1replay proxy --capture <file.jsonl>
      Sits between a real V1 and v1simple and logs every frame both ways.
      Connects to the V1 first, then advertises, so v1simple cannot win the
      race to the real unit.
      --capture <path>     JSONL capture file (default v1proxy-<timestamp>.jsonl)
      --tag <text>         label every line, e.g. --tag "cold-connect"
      --v1-name <text>     match the V1 by advertised name instead of service UUID
      --log-packets        also echo every frame to the console
      Timing through the proxy is not meaningful — two links, one radio.

    \(Ansi.bold)KEYS DURING PLAYBACK\(Ansi.reset)
      space  pause / resume        n  step one sample     r  restart
      ]  faster    [  slower        1  reset speed to 1x
      .  next strength change       ,  previous change
      m  cycle mute override        p  toggle display power    q  quit
    """)
}

func runCrib() {
    let header = V1.Header.draft

    let alertHex = V1.AlertRow
        .single(bars: 1, band: .ka, direction: .front, frequencyMHz: 34_700)
        .packet(header: header, checksum: false)
        .hexString
    let oneBarHex = V1.DisplayFrame
        .alerting(bars: 1, band: .ka, direction: .front, bogeyCount: 1,
                  muted: false, volume: 0x00, displayOn: false,
                  includeModeBits: false)
        .packet(header: header, checksum: false)
        .hexString
    let sixBarHex = V1.DisplayFrame
        .alerting(bars: 6, band: .ka, direction: .front, bogeyCount: 1,
                  muted: false, volume: 0x00, displayOn: false,
                  includeModeBits: false)
        .packet(header: header, checksum: false)
        .hexString
    let versionHex = V1.versionPacket(header: header, version: "4.1038", checksum: false).hexString
    let volumeHex = V1.allVolumePacket(header: header, main: 4, muted: 0, checksum: false).hexString

    console.print("""
    \(Ansi.bold)LightBlue manual test — V1G-REPLAY\(Ansi.reset)

    Service   \(V1.serviceUUID)
      B2CE  Read, Notify              complete V1 packets up to 20 bytes
      B4E0  Read, Notify              partial transport for packets over 20 bytes
      B6D4  Write Without Response    commands from v1simple
      B8D2  Write Without Response    long commands
      BCE0  Read, Notify              compatibility stub
      BAD4  Write, Write Without Response   alternate commands

    \(Ansi.bold)Notify on B2CE — synthetic Ka 34.700 front, priority\(Ansi.reset)
      \(alertHex)

    \(Ansi.bold)Notify on B2CE — one bar\(Ansi.reset)
      \(oneBarHex)

    \(Ansi.bold)Notify on B2CE — six bars\(Ansi.reset)
      \(sixBarHex)

    \(Ansi.bold)Notify on B2CE — short version reply after reqVersion\(Ansi.reset)
      respVersion    \(versionHex)

    \(Ansi.bold)Notify on B2CE — short all-volume reply after reqAllVolume\(Ansi.reset)
      respAllVolume  \(volumeHex)

    \(Ansi.bold)Framing choices\(Ansi.reset)
    These crib vectors use the repository's DA E4 compatibility convention.
    Playback defaults to D8 EA for generated display/alert information and
    D6 EA for targeted replies. Use --header draft only when exact fixture
    parity is required.

    \(Ansi.bold)One deliberate difference from the draft: bogey image2\(Ansi.reset)
    The draft and test_protocol_spec_conformance both send 06 00. The tool sends
    06 06 as a deliberate stimulus choice:
    display_update.cpp:223 and display_orchestration_module.cpp:147 gate the
    blink-refresh repaint on bogeyCounterByte != bogeyCounterByte2. Sending 06 00
    therefore switches on an extra repaint source for the whole replay. 06 06
    leaves paint slaved to parse, which makes packet-to-paint tests deterministic.
    --blink-bogey sends 06 00 if you want fixture parity instead.

    `v1replay play` also adds a checksum byte and the V4.1028+ full eight-byte
    display payload (so auxData2 carries current main/muted volume in its
    high/low nibbles, never saved values) — the same 9-byte payload region
    test_protocol_spec_conformance builds. Use --no-checksum for the 14-byte
    draft form. Normal v4.1038 playback also carries the current mode in
    auxData1; the explicit draft header retains its historical zero. Both parse;
    the firmware never verifies the checksum.

    Use `v1replay export --synthetic --format lightblue` for generated packets.
    """)
}

func playbackPacketPlan(for sample: TimedSample,
                        controlState: V1.Session.ControlState,
                        blinkBogey: Bool,
                        arrowBlinkProfile: ArrowBlinkProfile,
                        header: V1.Header,
                        checksum: Bool,
                        includeAlertTable: Bool) -> V1.PlaybackPacketPlan {
    return V1.PlaybackPacketPlan(
        sample: sample,
        controlState: controlState,
        displayOn: true,
        muted: sample.muted,
        blinkBogey: blinkBogey,
        blinkArrow: arrowBlinkProfile.shouldBlink(sample),
        header: header,
        checksum: checksum,
        includeAlertTable: includeAlertTable
    )
}

func characteristicName(for channel: V1.ReplyChannel) -> String {
    switch channel {
    case .displayShort: return "B2CE"
    case .displayLong: return "B4E0"
    }
}

func compactHex(_ packet: [UInt8]) -> String {
    return packet.map { String(format: "%02X", $0) }.joined()
}

func runExport() throws {
    if args.optionalString("out") != nil {
        throw ReplayError.message("file output is disabled; export writes to stdout only")
    }

    let bench = args.bool("bench")
    if bench {
        try validateBenchOptions()
    }

    let encounter: Encounter
    if bench {
        encounter = BenchScenario.make()
    } else if args.bool("synthetic") {
        encounter = Encounter.syntheticDemo()
    } else {
        encounter = try loadEncounter()
    }

    let format = args.string("format", "hex")
    if bench {
        guard format == "csv" else {
            throw ReplayError.message("bench export supports only --format csv")
        }
        console.print(BenchScenario.expectedCSV(for: encounter))
        return
    }

    let header = try makeInformationHeader()
    let checksum = !args.bool("no-checksum")
    let mode = try makeMode()
    let (mainVolume, mutedVolume) = makeVolumePair()
    let controlState = V1.Session.ControlState(
        mode: mode,
        mainVolume: mainVolume,
        mutedVolume: mutedVolume,
        savedMainVolume: mainVolume,
        savedMutedVolume: mutedVolume
    )
    let sendAlerts = !args.bool("no-alerts")
    let blinkBogey = args.bool("blink-bogey")
    let arrowBlinkProfile = try makeArrowBlinkProfile(benchDefault: false)
    var lines: [String] = []

    switch format {
    case "csv":
        // Preserve the packet-oriented CSV used by demo and external replay.
        lines.append("offset_s,index,bars,direction,muted,characteristic,packet_hex")
        for sample in encounter.samples {
            let primary = sample.priorityAlert
            let bars = primary?.strength ?? 0
            let direction = primary?.direction.label ?? "NONE"
            let plan = playbackPacketPlan(
                for: sample, controlState: controlState,
                blinkBogey: blinkBogey, arrowBlinkProfile: arrowBlinkProfile,
                header: header, checksum: checksum, includeAlertTable: sendAlerts
            )
            for emission in plan.emissions {
                lines.append(String(format: "%.3f,%d,%d,%@,%@,%@,%@",
                                    sample.offset, sample.sourceIndex, bars, direction,
                                    sample.muted ? "1" : "0",
                                    characteristicName(for: emission.channel),
                                    compactHex(emission.bytes)))
            }
        }

    case "lightblue":
        lines.append("# \(encounter.origin.label) — \(encounter.samples.count) samples, "
                     + String(format: "%.1f", encounter.duration) + "s")
        lines.append("# paste the hex into LightBlue's notify value field, in order")
        var previous: Double = 0
        for sample in encounter.samples {
            let wait = sample.offset - previous
            previous = sample.offset
            let primary = sample.priorityAlert
            let plan = playbackPacketPlan(
                for: sample, controlState: controlState,
                blinkBogey: blinkBogey, arrowBlinkProfile: arrowBlinkProfile,
                header: header, checksum: checksum, includeAlertTable: sendAlerts
            )
            lines.append(String(format: "wait %.3fs", wait))
            let description = primary.map { "\($0.strength) bar(s) \($0.direction.label)" } ?? "idle"
            for emission in plan.emissions {
                var line = characteristicName(for: emission.channel)
                    + " " + compactHex(emission.bytes)
                if emission.kind == .displayFrame {
                    line += "   # " + description
                }
                lines.append(line)
            }
        }

    default:
        for sample in encounter.samples {
            let plan = playbackPacketPlan(
                for: sample, controlState: controlState,
                blinkBogey: blinkBogey, arrowBlinkProfile: arrowBlinkProfile,
                header: header, checksum: checksum, includeAlertTable: sendAlerts
            )
            for emission in plan.emissions {
                lines.append(String(
                    format: "%8.3f  %@  %@", sample.offset,
                    characteristicName(for: emission.channel), emission.bytes.hexString
                ))
            }
        }
    }

    console.print(lines.joined(separator: "\n"))
}

func runPlay(idleOnly: Bool,
             synthetic: Bool = false,
             bench: Bool = false,
             handshakeNotificationHoldMs: Int = 0,
             ownerProcess: ProcessOwnerGuard? = nil) throws {
    if args.bool("handshake-only") && !bench {
        throw ReplayError.message("--handshake-only is available only in bench mode")
    }
    if bench { try validateBenchOptions() }
    let replyHeader = try makeHeader()
    let informationHeader = try makeInformationHeader()
    let checksum = !args.bool("no-checksum")
    let mode = try makeMode()
    let (mainVolume, mutedVolume) = makeVolumePair()

    let encounter: Encounter
    if idleOnly {
        encounter = Encounter.idle()
    } else if bench {
        encounter = BenchScenario.make()
    } else if synthetic {
        encounter = Encounter.syntheticDemo()
    } else {
        encounter = try loadEncounter()
    }

    var peripheralConfig = V1Peripheral.Config()
    peripheralConfig.localName = args.string("name", "V1G-REPLAY")
    peripheralConfig.header = replyHeader
    peripheralConfig.checksum = checksum
    peripheralConfig.version = args.string("v1-version", "4.1038")
    peripheralConfig.mode = mode
    peripheralConfig.mainVolume = mainVolume
    peripheralConfig.mutedVolume = mutedVolume
    peripheralConfig.logPackets = args.bool("log-packets")
    peripheralConfig.handshakeNotificationHoldMs = handshakeNotificationHoldMs
    if let ledgerPath = args.optionalString("handshake-ledger") {
        guard bench else {
            throw ReplayError.message("--handshake-ledger is available only in bench mode")
        }
        peripheralConfig.handshakeLedger = try HandshakeLedger(path: ledgerPath)
    }

    var playerOptions = Player.Options()
    playerOptions.speed = args.double("speed", 1.0)
    playerOptions.loop = args.bool("loop")
    playerOptions.startPaused = args.bool("paused")
    playerOptions.sendAlerts = !args.bool("no-alerts")
    playerOptions.requireStartAlertData = !args.bool("always-alerts")
    playerOptions.waitForSubscribe = !args.bool("no-wait")
    playerOptions.waitForAlertData = bench
    playerOptions.idleLead = (idleOnly || bench) ? 0 : args.double("idle-lead", 3.0)
    playerOptions.idleTail = (idleOnly || bench) ? 0 : args.double("idle-tail", 3.0)
    playerOptions.idleHz = args.double("idle-hz", 3.0)
    playerOptions.header = informationHeader
    playerOptions.checksum = checksum
    playerOptions.blinkBogey = args.bool("blink-bogey")
    playerOptions.arrowBlinkProfile = try makeArrowBlinkProfile(benchDefault: bench)
    playerOptions.handshakeOnly = args.bool("handshake-only")

    // Banner
    console.print("")
    console.print("\(Ansi.bold)v1replay \(toolVersion)\(Ansi.reset)  —  pretending to be a Valentine One Gen2")
    console.print("\(Ansi.dim)service  \(V1.serviceUUID)\(Ansi.reset)")
    console.print("\(Ansi.dim)name     \(peripheralConfig.localName)   info \(String(format: "%02X %02X", informationHeader.dest, informationHeader.src))   replies \(String(format: "%02X %02X", replyHeader.dest, replyHeader.src))   checksum \(checksum ? "on" : "off")\(Ansi.reset)")
    // State the blink plane out loud: it decides whether the firmware's
    // blink-refresh repaint runs, so a bench log has to say which stimulus
    // produced it before it can be compared with a native replay.
    console.print("\(Ansi.dim)bogey    "
                  + (playerOptions.blinkBogey
                     ? "06 00 — blink plane ON, blink-refresh repaint engaged"
                     : "06 06 — steady, paint stays parse-driven")
                  + "\(Ansi.reset)")
    let arrowBlinkSamples = playerOptions.arrowBlinkProfile.sampleCount(in: encounter)
    console.print("\(Ansi.dim)arrow   \(playerOptions.arrowBlinkProfile.rawValue) — "
                  + "\(arrowBlinkSamples)/\(encounter.samples.count) samples, "
                  + "source \(playerOptions.arrowBlinkProfile.sourceLabel)"
                  + "\(Ansi.reset)")
    if !idleOnly {
        let histogram = encounter.strengthHistogram
            .map { "\($0.bars)→\($0.count)" }
            .joined(separator: "  ")
        console.print("\(Ansi.dim)input    \(encounter.origin.label)  \(encounter.samples.count) samples  "
                      + String(format: "%.0fs", encounter.duration)
                      + "\(Ansi.reset)")
        console.print("\(Ansi.dim)bars     \(histogram)\(Ansi.reset)")
    }
    console.print("\(Ansi.dim)keys     space pause · n step · r restart · [ ] speed · . next change · m mute · q quit\(Ansi.reset)")
    console.print("")

    let peripheral = V1Peripheral(config: peripheralConfig)
    let player = Player(encounter: encounter, peripheral: peripheral, options: playerOptions)

    peripheral.onLog = { message in console.log("\(Ansi.cyan)ble\(Ansi.reset)  \(message)") }
    peripheral.onMuteCommand = { muted in
        player.setMuteOverride(muted)
        console.log("\(Ansi.cyan)ble\(Ansi.reset)  v1simple asked for mute \(muted ? "ON" : "OFF")")
    }
    peripheral.onDisplayPowerCommand = { on in
        player.setDisplayOn(on)
        console.log("\(Ansi.cyan)ble\(Ansi.reset)  v1simple asked for display \(on ? "ON" : "OFF")")
    }
    let machineEvents = args.bool("machine-events")
    let sessionTransportEvents = machineEvents
        ? BooleanMachineEventEmitter { active in
            console.print("V1REPLAY_EVENT {\"state\":\"session_transport\","
                          + "\"active\":\(active ? "true" : "false")}")
        }
        : nil
    if machineEvents {
        peripheral.onStateChange = {
            sessionTransportEvents?.emit(peripheral.sessionTransportActive)
            if playerOptions.handshakeOnly {
                let active = peripheral.displaySubscribed
                    && peripheral.alertDataRequested
                    && peripheralConfig.handshakeLedger?.activeEpoch != nil
                console.print("V1REPLAY_EVENT {\"state\":\"handshake_transport\","
                              + "\"active\":\(active ? "true" : "false")}")
            }
        }
        sessionTransportEvents?.emit(peripheral.sessionTransportActive)
    }
    if machineEvents && bench {
        player.onDetectorVolumeCheckpoint = { checkpoint in
            console.print(checkpoint.machineEventLine)
        }
    }
    if machineEvents && bench {
        player.onDetectorMuteCheckpoint = { checkpoint in
            console.print(checkpoint.machineEventLine)
        }
    }
    if machineEvents && bench {
        player.onDetectorModeCheckpoint = { checkpoint in
            console.print(checkpoint.machineEventLine)
        }
    }
    if machineEvents && bench {
        player.onStimulusRequested = { stimulus in
            console.print(stimulus.machineEventLine)
        }
    }
    if machineEvents && bench {
        console.print("V1REPLAY_EVENT {\"state\":\"configured\","
                      + "\"blinkProfile\":\"\(playerOptions.arrowBlinkProfile.rawValue)\","
                      + "\"blinkSource\":\"\(playerOptions.arrowBlinkProfile.sourceLabel)\","
                      + "\"blinkSamples\":\(arrowBlinkSamples),"
                      + "\"totalSamples\":\(encounter.samples.count),"
                      + "\"cadenceHz\":\(BenchScenario.cadenceHz)}")
    }
    player.onLog = { message in
        console.log("\(Ansi.blue)play\(Ansi.reset) \(message)")
        if machineEvents && message.hasPrefix("Replay complete —") {
            console.print("V1REPLAY_EVENT {\"state\":\"complete\"}")
        } else if machineEvents && message.hasPrefix("Handshake-only ready —") {
            console.print("V1REPLAY_EVENT {\"state\":\"handshake_ready\"}")
        }
    }
    peripheral.onHandshakeClearDelivered = {
        player.handshakeOnlyClearDelivered()
    }
    peripheral.onStartAlertData = {
        player.ensureHandshakeOnlyClear()
    }
    player.onReplayStarted = { hostMonotonicSeconds in
        if machineEvents && bench {
            console.log("V1REPLAY_EVENT {\"state\":\"replay_started\","
                        + "\"hostMonotonicSeconds\":\(hostMonotonicSeconds)}")
        }
    }

    let quit = Flag()
    let signalMonitor = GracefulSignalMonitor { quit.set() }
    defer { signalMonitor.cancel() }
    let exitOnComplete = args.bool("exit-on-complete")
    console.enableRawMode()
    console.onKey = { key in
        switch key {
        case " ": player.togglePause()
        case "n": player.step()
        case "r": player.restart(); console.log("\(Ansi.blue)play\(Ansi.reset) restart")
        case "]": player.nudgeSpeed(2.0)
        case "[": player.nudgeSpeed(0.5)
        case "1": player.setSpeed(1.0)
        case ".": player.jumpToNextChange()
        case ",": player.jumpToPreviousChange()
        case "m": player.toggleMuteOverride()
        case "p":
            player.toggleDisplayPower()
            console.log("\(Ansi.blue)play\(Ansi.reset) display power "
                        + (player.snapshot.displayOn ? "ON" : "OFF (V1 dark mode)"))
        case "q", "\u{03}", "\u{04}": quit.set()
        default: break
        }
    }
    console.startKeyLoop()

    player.start()

    var warnedAboutPower = false
    let startedAt = nowSeconds()

    while !quit.isSet {
        if let ownerProcess, !ownerProcess.isDirectParent() {
            quit.set()
            continue
        }
        let snapshot = player.snapshot
        if exitOnComplete && snapshot.phase == .finished {
            quit.set()
            continue
        }
        let connection: String
        if peripheral.subscriberCount > 0 {
            var parts: [String] = []
            parts.append(peripheral.displaySubscribed ? "\(Ansi.green)B2CE\(Ansi.reset)" : "\(Ansi.dim)B2CE\(Ansi.reset)")
            parts.append(peripheral.longSubscribed ? "\(Ansi.green)B4E0\(Ansi.reset)" : "\(Ansi.dim)B4E0\(Ansi.reset)")
            parts.append(peripheral.alertDataRequested ? "\(Ansi.green)alerts\(Ansi.reset)" : "\(Ansi.dim)alerts\(Ansi.reset)")
            connection = parts.joined(separator: " ")
        } else if peripheral.isAdvertising {
            connection = "\(Ansi.dim)advertising\(Ansi.reset)"
        } else {
            connection = "\(Ansi.dim)starting\(Ansi.reset)"
        }

        var muteTag = ""
        if let override = snapshot.muteOverride {
            muteTag = override ? "  \(Ansi.yellow)MUTE\(Ansi.reset)" : "  \(Ansi.dim)unmuted\(Ansi.reset)"
        }

        let progress = idleOnly
            ? "idle"
            : "\(Console.clock(snapshot.elapsed))/\(Console.clock(snapshot.duration))  \(snapshot.index)/\(snapshot.total)"

        console.setStatus(String(format: "%@ %d  │  %@  │  %.2f×  │  %@  │  tx %d%@  │  %@",
                                 Console.barMeter(snapshot.bars),
                                 snapshot.bars,
                                 progress,
                                 snapshot.speed,
                                 snapshot.phase.rawValue,
                                 snapshot.packetsSent,
                                 muteTag,
                                 connection))

        if !warnedAboutPower, !peripheral.isPoweredOn, nowSeconds() - startedAt > 4 {
            warnedAboutPower = true
            console.log("\(Ansi.yellow)warn\(Ansi.reset) Bluetooth is not powered on for this process. "
                        + "If macOS never prompted, grant Bluetooth to your terminal in "
                        + "System Settings → Privacy & Security → Bluetooth, then run it again.")
        }

        Thread.sleep(forTimeInterval: ownerProcess == nil
                     ? 0.1
                     : ProcessOwnerGuard.pollIntervalSeconds)
    }

    player.stop()
    peripheral.stop { sessionTransportActive in
        guard machineEvents else { return }
        console.print(
            StoppingMachineEvent(sessionTransportActive: sessionTransportActive).line
        )
    }
    console.clearStatus()
    sessionTransportEvents?.emit(false)
    if machineEvents {
        console.print("V1REPLAY_EVENT {\"state\":\"stopped\"}")
    }
    console.restore()
    console.print("Stopped. \(player.snapshot.packetsSent) packets sent, "
                  + "\(peripheral.commandsReceived) commands received, "
                  + "\(peripheral.notifiesDropped) notifications dropped.")
}

// MARK: - Proxy

/// Thread-safe counter for the status line.
final class Counter {
    private let lock = NSLock()
    private var value = 0
    func bump() { lock.lock(); value += 1; lock.unlock() }
    var count: Int { lock.lock(); defer { lock.unlock() }; return value }
}

func defaultCapturePath() -> String {
    let formatter = DateFormatter()
    formatter.dateFormat = "yyyyMMdd_HHmmss"
    formatter.locale = Locale(identifier: "en_US_POSIX")
    return "v1proxy-\(formatter.string(from: Date())).jsonl"
}

func runProxy() throws {
    let capturePath = args.optionalString("capture") ?? defaultCapturePath()
    let tag = args.optionalString("tag")
    let verbose = args.bool("log-packets")
    let log = try PacketLog(path: capturePath, tag: tag)

    var peripheralConfig = V1Peripheral.Config()
    peripheralConfig.localName = args.string("name", "V1G-REPLAY")
    peripheralConfig.deferAdvertising = true

    console.print("\(Ansi.bold)v1replay \(toolVersion)\(Ansi.reset)  —  proxy: real V1 ↔ Mac ↔ v1simple")
    console.print("\(Ansi.dim)capture  \(capturePath)\(Ansi.reset)")
    if let tag = tag { console.print("\(Ansi.dim)tag      \(tag)\(Ansi.reset)") }
    console.print("\(Ansi.dim)note     timing through the proxy is not meaningful — semantics only\(Ansi.reset)")
    console.print("\(Ansi.dim)keys     q quit\(Ansi.reset)")
    console.print("")

    let peripheral = V1Peripheral(config: peripheralConfig)
    let central = V1ProxyCentral(
        config: V1ProxyCentral.Config(nameFilter: args.optionalString("v1-name"),
                                      excludeName: peripheralConfig.localName),
        queue: peripheral.queue
    )

    let fromV1 = Counter()
    let toV1 = Counter()

    peripheral.onLog = { message in console.log("\(Ansi.cyan)dut\(Ansi.reset)  \(message)") }
    central.onLog = { message in console.log("\(Ansi.green)v1\(Ansi.reset)   \(message)") }

    // Real V1 → capture → v1simple.
    central.onNotify = { uuid, bytes in
        log.record(direction: "v1->dut", characteristic: uuid, bytes: bytes)
        fromV1.bump()
        peripheral.forward(bytes, toCharacteristicUUID: uuid)
        if verbose {
            console.log("\(Ansi.green)v1\(Ansi.reset)   → \(V1Peripheral.shortName(uuid)) \(bytes.hexString)")
        }
    }

    // v1simple → capture → real V1. Returning true suppresses the emulator.
    peripheral.onRawWrite = { uuid, bytes in
        log.record(direction: "dut->v1", characteristic: uuid, bytes: bytes)
        toV1.bump()
        central.write(bytes, to: uuid)
        if verbose {
            console.log("\(Ansi.cyan)dut\(Ansi.reset)  ← \(V1Peripheral.shortName(uuid)) \(bytes.hexString)")
        }
        return true
    }

    central.onConnected = { name, characteristics in
        log.note("v1_connected", ["name": name, "characteristics": characteristics])
        console.log("\(Ansi.green)v1\(Ansi.reset)   connected — advertising to v1simple now")
        peripheral.beginAdvertising()
    }
    central.onDisconnected = { name in
        log.note("v1_disconnected", ["name": name ?? ""])
    }

    log.note("proxy_start", ["tool": toolVersion, "localName": peripheralConfig.localName])

    let quit = Flag()
    let signalMonitor = GracefulSignalMonitor { quit.set() }
    defer { signalMonitor.cancel() }
    console.enableRawMode()
    console.onKey = { key in
        switch key {
        case "q", "\u{03}", "\u{04}": quit.set()
        default: break
        }
    }
    console.startKeyLoop()

    let startedAt = nowSeconds()
    var warnedAboutPower = false

    while !quit.isSet {
        let link = central.isConnected
            ? "\(Ansi.green)V1 linked\(Ansi.reset) (\(central.subscriptions) notify)"
            : "\(Ansi.yellow)waiting for V1\(Ansi.reset)"
        let dut = peripheral.subscriberCount > 0
            ? "\(Ansi.green)v1simple linked\(Ansi.reset)"
            : (peripheral.isAdvertising ? "advertising" : "idle")

        console.setStatus(String(format: "%@  │  %@  │  v1→dut %d  │  dut→v1 %d  │  %@",
                                 link,
                                 dut,
                                 fromV1.count,
                                 toV1.count,
                                 Console.clock(nowSeconds() - startedAt)))

        if !warnedAboutPower, !peripheral.isPoweredOn, nowSeconds() - startedAt > 4 {
            warnedAboutPower = true
            console.log("\(Ansi.yellow)warn\(Ansi.reset) Bluetooth is not powered on for this process. "
                        + "If macOS never prompted, grant Bluetooth to your terminal in "
                        + "System Settings → Privacy & Security → Bluetooth, then run it again.")
        }

        Thread.sleep(forTimeInterval: 0.1)
    }

    log.note("proxy_stop", ["framesFromV1": String(fromV1.count), "framesToV1": String(toV1.count)])
    log.close()
    central.stop()
    peripheral.stop()
    console.clearStatus()
    console.restore()
    console.print("Stopped. \(fromV1.count) notifications from the V1, "
                  + "\(toV1.count) commands from v1simple.")
    console.print("Capture: \(capturePath)")
}

// MARK: - Dispatch

do {
    let ownerProcess = try parseProcessOwnerGuard(
        args.optionalString("owner-pid"),
        command: args.command
    )
    let handshakeNotificationHoldMs = try parseHandshakeNotificationHoldMilliseconds(
        args.optionalString("handshake-notification-hold-ms"),
        bench: args.command == "bench",
        handshakeOnly: args.bool("handshake-only")
    )
    switch args.command {
    case "help":
        runHelp()
    case "version":
        console.print("v1replay \(toolVersion)")
    case "crib":
        runCrib()
    case "export":
        try runExport()
    case "demo":
        try runPlay(idleOnly: false, synthetic: true)
    case "bench":
        try runPlay(
            idleOnly: false,
            bench: true,
            handshakeNotificationHoldMs: handshakeNotificationHoldMs,
            ownerProcess: ownerProcess
        )
    case "idle":
        try runPlay(idleOnly: true, ownerProcess: ownerProcess)
    case "proxy":
        try runProxy()
    default:
        if args.bool("help") || args.bool("h") || args.positional.isEmpty {
            runHelp()
        } else {
            try runPlay(idleOnly: false)
        }
    }
} catch let error as ReplayError {
    console.restore()
    FileHandle.standardError.write(Data(("v1replay: " + error.description + "\n").utf8))
    exit(2)
} catch {
    console.restore()
    FileHandle.standardError.write(Data(("v1replay: " + error.localizedDescription + "\n").utf8))
    exit(2)
}

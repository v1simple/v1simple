import Foundation

@inline(__always)
func nowSeconds() -> Double {
    return Double(DispatchTime.now().uptimeNanoseconds) / 1_000_000_000.0
}

// =============================================================================
// Replay engine: walks an encounter's timeline and hands packets to the
// peripheral at the recorded cadence, with transport controls.
// =============================================================================

final class Player {

    struct Options {
        var speed: Double = 1.0
        var loop: Bool = false
        var startPaused: Bool = false
        var sendAlerts: Bool = true
        var requireStartAlertData: Bool = true
        var waitForSubscribe: Bool = true
        var waitForAlertData: Bool = false
        var idleLead: TimeInterval = 3.0
        var idleTail: TimeInterval = 3.0
        var idleHz: Double = 3.0
        var header: V1.Header = .broadcastInformation
        var checksum: Bool = true
        var blinkBogey: Bool = false
        var arrowBlinkProfile: ArrowBlinkProfile = .steady
        var handshakeOnly: Bool = false
    }

    enum Phase: String {
        case waiting = "waiting for central"
        case idleLead = "idle lead-in"
        case playing = "playing"
        case paused = "paused"
        case idleTail = "idle tail"
        case finished = "finished"
    }

    private enum Outcome {
        case completed
        case restart
        case aborted
    }

    private enum WaitResult {
        case fire
        case restart
        case seek
        case aborted
    }

    // Live state, read by the console thread.
    private let lock = NSLock()
    private var _phase: Phase = .waiting
    private var _index: Int = 0
    private var _speed: Double
    private var _paused: Bool
    private var _stepRequested = false
    private var _seekRequest: Int? = nil
    private var _restartRequested = false
    private var _stopped = false
    private var _muteOverride: Bool? = nil
    private var _displayOn = true
    private var _packetsSent = 0
    private var _lastBars = 0
    private var _elapsed: Double = 0
    private var reportedReplayStart = false
    private var handshakeClearDeliveryConfirmed = false
    private var stimulusSequence = 0

    private let encounter: Encounter
    private let peripheral: V1Peripheral
    private let options: Options
    private var thread: Thread?

    var onLog: ((String) -> Void)?
    var onReplayStarted: ((Double) -> Void)?
    var onDetectorVolumeCheckpoint: ((DetectorVolumeCheckpoint) -> Void)?
    var onDetectorMuteCheckpoint: ((DetectorMuteCheckpoint) -> Void)?
    var onDetectorModeCheckpoint: ((DetectorModeCheckpoint) -> Void)?
    var onStimulusRequested: ((ReplayStimulusEvent) -> Void)?

    init(encounter: Encounter, peripheral: V1Peripheral, options: Options) {
        self.encounter = encounter
        self.peripheral = peripheral
        self.options = options
        self._speed = options.speed
        self._paused = options.startPaused
    }

    // MARK: - Transport controls (called from the key handler)

    func togglePause() {
        lock.lock(); _paused.toggle(); lock.unlock()
    }

    func step() {
        lock.lock(); _paused = true; _stepRequested = true; lock.unlock()
    }

    func nudgeSpeed(_ factor: Double) {
        lock.lock()
        _speed = min(32.0, max(0.05, _speed * factor))
        lock.unlock()
    }

    func setSpeed(_ value: Double) {
        lock.lock()
        _speed = min(32.0, max(0.05, value))
        lock.unlock()
    }

    func restart() {
        lock.lock(); _restartRequested = true; lock.unlock()
    }

    func seek(to index: Int) {
        lock.lock()
        _seekRequest = max(0, min(encounter.samples.count - 1, index))
        lock.unlock()
    }

    func jumpToNextChange() {
        let current = currentIndex
        // `_index` points at the next sample to emit, not the sample most
        // recently displayed.  A change at exactly `current` therefore still
        // needs to be visited; using `>` silently skipped back-to-back changes.
        if let next = encounter.changeIndices.first(where: { $0 >= current }) {
            seek(to: next)
        }
    }

    func jumpToPreviousChange() {
        let current = currentIndex
        if let previous = encounter.changeIndices.last(where: { $0 < current - 1 }) {
            seek(to: previous)
        }
    }

    func stop() {
        lock.lock(); _stopped = true; _paused = false; lock.unlock()
    }

    /// Event-driven handshake-only response. The peripheral owns pending-row
    /// deduplication and delivery retry; this method is also called once by the
    /// polling path as a fallback for a start accepted before callback wiring.
    func ensureHandshakeOnlyClear() {
        guard options.handshakeOnly else { return }
        let emissions = V1.PlaybackPacketPlan.handshakeOnlyEmissions(
            header: options.header,
            checksum: options.checksum
        )
        guard emissions.count == 1,
              let clear = emissions.first,
              clear.channel == .displayShort else { return }
        peripheral.ensureHandshakeClear(clear.bytes)
    }

    /// Called only after CoreBluetooth accepts the canonical clear row. Keep
    /// the ready event one-shot even if an adapter ever repeats its callback.
    func handshakeOnlyClearDelivered() {
        guard options.handshakeOnly else { return }
        lock.lock()
        guard !handshakeClearDeliveryConfirmed else {
            lock.unlock()
            return
        }
        handshakeClearDeliveryConfirmed = true
        _packetsSent += 1
        _phase = .finished
        lock.unlock()
        onLog?("Handshake-only ready — clear alert row delivery confirmed; holding quiet.")
    }

    func toggleMuteOverride() {
        lock.lock()
        switch _muteOverride {
        case .none: _muteOverride = true
        case .some(true): _muteOverride = false
        case .some(false): _muteOverride = nil
        }
        lock.unlock()
    }

    func setMuteOverride(_ value: Bool?) {
        lock.lock(); _muteOverride = value; lock.unlock()
    }

    func setDisplayOn(_ value: Bool) {
        lock.lock(); _displayOn = value; lock.unlock()
    }

    func toggleDisplayPower() {
        lock.lock(); _displayOn.toggle(); lock.unlock()
    }

    // MARK: - Snapshot for the status line

    struct Snapshot {
        let phase: Phase
        let index: Int
        let total: Int
        let elapsed: Double
        let duration: Double
        let speed: Double
        let bars: Int
        let packetsSent: Int
        let muteOverride: Bool?
        let displayOn: Bool
    }

    var snapshot: Snapshot {
        lock.lock()
        defer { lock.unlock() }
        let phase = (_paused && _phase == .playing) ? Phase.paused : _phase
        return Snapshot(phase: phase,
                        index: _index,
                        total: encounter.samples.count,
                        elapsed: _elapsed,
                        duration: encounter.duration,
                        speed: _speed,
                        bars: _lastBars,
                        packetsSent: _packetsSent,
                        muteOverride: _muteOverride,
                        displayOn: _displayOn)
    }

    var currentIndex: Int {
        lock.lock(); defer { lock.unlock() }
        return _index
    }

    var isStopped: Bool {
        lock.lock(); defer { lock.unlock() }
        return _stopped
    }

    // MARK: - Run loop

    func start() {
        let thread = Thread { [weak self] in self?.run() }
        thread.name = "v1replay.player"
        thread.stackSize = 512 * 1024
        self.thread = thread
        thread.start()
    }

    private func run() {
        while !isStopped {
            waitForCentral()
            if isStopped { return }

            if options.handshakeOnly {
                runHandshakeOnly()
                return
            }

            switch emitIdle(seconds: options.idleLead, phase: .idleLead) {
            case .aborted: return
            case .restart: continue
            case .completed: break
            }

            switch playTimeline() {
            case .aborted:
                return
            case .restart:
                continue
            case .completed:
                switch emitIdle(seconds: options.idleTail, phase: .idleTail) {
                case .aborted:
                    return
                case .restart:
                    continue
                case .completed:
                    if sendEmptyAlertTable() {
                        lock.lock(); _packetsSent += 1; lock.unlock()
                    }
                }
            }

            if options.loop && !isStopped {
                onLog?("Loop — restarting \(encounter.origin.label)")
                resetProgress()
                continue
            }

            setPhase(.finished)
            if !encounter.samples.isEmpty {
                onLog?("Replay complete — \(snapshot.packetsSent) packets sent. 'r' replays, 'q' quits.")
            }

            // Hold the link open with idle frames: dropping the connection would
            // reset the firmware's BLE session and destroy the state you just
            // spent three minutes building up.
            switch holdIdleUntilRestart() {
            case .aborted: return
            case .restart, .completed: resetProgress()
            }
        }
    }

    private func waitForCentral() {
        guard options.waitForSubscribe || options.waitForAlertData else { return }
        lock.lock()
        if !(options.handshakeOnly && handshakeClearDeliveryConfirmed) {
            _phase = .waiting
        }
        lock.unlock()
        while !isStopped {
            if transportReady() { return }
            Thread.sleep(forTimeInterval: 0.05)
        }
    }

    private func transportReady() -> Bool {
        let displayReady = !options.waitForSubscribe || peripheral.displaySubscribed
        let alertDataReady = !options.waitForAlertData
            || !options.sendAlerts
            || (peripheral.displaySubscribed && peripheral.alertDataRequested)
        return displayReady && alertDataReady
    }

    private func runHandshakeOnly() {
        ensureHandshakeOnlyClear()
        while !isStopped {
            Thread.sleep(forTimeInterval: 0.05)
        }
    }

    /// Stream idle display frames (meter dark, no alert) for a while.
    private func emitIdle(seconds: TimeInterval, phase: Phase) -> Outcome {
        guard seconds > 0 else { return .completed }
        setPhase(phase)
        let interval = 1.0 / max(0.5, options.idleHz)
        var remaining = seconds
        while remaining > 0 {
            if isStopped { return .aborted }
            if consumeRestart() { return .restart }
            sendIdleFrame()
            Thread.sleep(forTimeInterval: interval)
            remaining -= interval
        }
        return .completed
    }

    private func holdIdleUntilRestart() -> Outcome {
        let interval = 1.0 / max(0.5, options.idleHz)
        while true {
            if isStopped { return .aborted }
            if consumeRestart() { return .restart }
            if consumeSeek() != nil { return .restart }
            sendIdleFrame()
            Thread.sleep(forTimeInterval: interval)
        }
    }

    private func sendIdleFrame() {
        lock.lock()
        let muted = _muteOverride ?? false
        let displayOn = _displayOn
        lock.unlock()
        let control = peripheral.controlState

        let packet = V1.PlaybackPacketPlan.idleDisplayPacket(
            controlState: control,
            displayOn: displayOn,
            muted: muted,
            header: options.header,
            checksum: options.checksum
        )
        peripheral.sendDisplay(packet)

        lock.lock(); _packetsSent += 1; _lastBars = 0; lock.unlock()
    }

    @discardableResult
    private func sendEmptyAlertTable() -> Bool {
        guard options.sendAlerts else { return false }
        if options.requireStartAlertData && !peripheral.alertDataRequested { return false }
        peripheral.sendDisplay(V1.PlaybackPacketPlan.clearAlertPacket(
            header: options.header,
            checksum: options.checksum
        ))
        return true
    }

    private func send(_ emission: V1.PlaybackPacketPlan.Emission) {
        switch emission.channel {
        case .displayShort:
            peripheral.sendDisplay(emission.bytes)
        case .displayLong:
            peripheral.sendLong(emission.bytes)
        }
    }

    private func playTimeline() -> Outcome {
        setPhase(.playing)
        var index = startIndex()
        var deadline = nowSeconds()
        var applyGap = false

        while index < encounter.samples.count {
            if isStopped { return .aborted }
            if consumeRestart() { return .restart }

            if let target = consumeSeek() {
                index = target
                deadline = nowSeconds()
                applyGap = false
                setIndex(index)
            }

            if applyGap && index > 0 {
                let gap = encounter.samples[index].offset - encounter.samples[index - 1].offset
                deadline += max(0, gap) / currentSpeed()
            }

            switch waitUntil(&deadline) {
            case .aborted:
                return .aborted
            case .restart:
                return .restart
            case .seek:
                applyGap = false
                continue
            case .fire:
                break
            }

            // Bench playback must remain aligned with its expected timeline. If
            // the B2CE notification subscription or the alert-data request is
            // lost in the small window after waitUntil(), retry this same step
            // once readiness returns instead of advancing without a full table.
            if !emit(sampleAt: index) {
                deadline = nowSeconds()
                applyGap = false
                continue
            }
            index += 1
            applyGap = true
            setIndex(index)
        }
        return .completed
    }

    @discardableResult
    private func emit(sampleAt index: Int) -> Bool {
        if options.waitForAlertData && !transportReady() { return false }
        let sample = encounter.samples[index]

        let muteCheckpoint = encounter.detectorMuteCheckpoint(at: index)
        lock.lock()
        // A physical detector mute edge updates the live modeled state once.
        // Held samples then observe later V1Simple commands instead of forcing
        // the scenario value on every frame.
        if let muteCheckpoint = muteCheckpoint {
            _muteOverride = muteCheckpoint.muted
        }
        let muted = _muteOverride ?? sample.muted
        let displayOn = _displayOn
        lock.unlock()
        if let muteCheckpoint = muteCheckpoint {
            onDetectorMuteCheckpoint?(muteCheckpoint)
        }
        // Apply the authored physical-V1 current pair once at its checkpoint
        // boundary, before taking the one atomic protocol-state snapshot used
        // by the complete table/display emission. The session then owns the
        // held value; a later V1Simple write remains observable rather than
        // being masked by a per-frame scenario override.
        let modeCheckpoint = encounter.detectorModeCheckpoint(at: index)
        if let modeCheckpoint = modeCheckpoint {
            _ = peripheral.applyDetectorMode(modeCheckpoint.mode)
            onDetectorModeCheckpoint?(modeCheckpoint)
        }
        let checkpoint = encounter.detectorVolumeCheckpoint(at: index)
        let control: V1.Session.ControlState
        if let checkpoint = checkpoint {
            control = peripheral.applyDetectorCurrentVolume(checkpoint.volume)
        } else {
            control = peripheral.controlState
        }
        if let checkpoint = checkpoint {
            onDetectorVolumeCheckpoint?(checkpoint)
        }

        let includeAlertTable = options.sendAlerts
            && (!options.requireStartAlertData || peripheral.alertDataRequested)
        let arrowBlink = options.arrowBlinkProfile.shouldBlink(sample)
        let plan = V1.PlaybackPacketPlan(
            sample: sample,
            controlState: control,
            displayOn: displayOn,
            muted: muted,
            blinkBogey: options.blinkBogey,
            blinkArrow: arrowBlink,
            header: options.header,
            checksum: options.checksum,
            includeAlertTable: includeAlertTable
        )
        for emission in plan.emissions { send(emission) }
        let sent = plan.emissions.count
        let requestedAt = nowSeconds()
        stimulusSequence += 1
        if !reportedReplayStart {
            reportedReplayStart = true
            onReplayStarted?(requestedAt)
        }
        onStimulusRequested?(ReplayStimulusEvent(
            sequence: stimulusSequence,
            sample: sample,
            controlState: control,
            muted: muted,
            displayOn: displayOn,
            arrowBlink: arrowBlink,
            plan: plan,
            requestedHostMonotonicSeconds: requestedAt
        ))

        lock.lock()
        _packetsSent += sent
        _lastBars = sample.priorityAlert?.strength ?? 0
        _elapsed = sample.offset
        lock.unlock()
        return true
    }

    /// Sleep until `deadline`, absorbing pause time and honouring single steps.
    private func waitUntil(_ deadline: inout Double) -> WaitResult {
        while true {
            if isStopped { return .aborted }

            if options.waitForAlertData && !transportReady() {
                // Freeze the replay clock while the bench transport is not
                // ready. Catching up would omit alert tables and make the
                // expected CSV diverge from what the firmware actually saw.
                let readinessLostAt = nowSeconds()
                setPhase(.waiting)
                while !isStopped && !transportReady() {
                    if consumeRestart() {
                        deadline += nowSeconds() - readinessLostAt
                        return .restart
                    }
                    if hasSeekRequest() {
                        deadline += nowSeconds() - readinessLostAt
                        return .seek
                    }
                    Thread.sleep(forTimeInterval: 0.02)
                }
                deadline += nowSeconds() - readinessLostAt
                if isStopped { return .aborted }
                setPhase(.playing)
                continue
            }

            lock.lock()
            let paused = _paused
            let stepping = _stepRequested
            let restarting = _restartRequested
            let seeking = _seekRequest != nil
            if stepping { _stepRequested = false }
            lock.unlock()

            if restarting { return .restart }
            if seeking { return .seek }
            if stepping {
                deadline = nowSeconds()
                return .fire
            }

            if paused {
                // Freeze the clock: push the deadline forward by the paused time
                // so resuming does not fire a burst of catch-up packets.
                let pauseStart = nowSeconds()
                Thread.sleep(forTimeInterval: 0.02)
                deadline += nowSeconds() - pauseStart
                continue
            }

            let remaining = deadline - nowSeconds()
            if remaining <= 0 { return .fire }
            Thread.sleep(forTimeInterval: min(remaining, 0.015))
        }
    }

    // MARK: - Small locked helpers

    private func currentSpeed() -> Double {
        lock.lock(); defer { lock.unlock() }
        return _speed
    }

    private func setPhase(_ phase: Phase) {
        lock.lock(); _phase = phase; lock.unlock()
    }

    private func setIndex(_ index: Int) {
        lock.lock()
        _index = index
        if index < encounter.samples.count {
            _elapsed = encounter.samples[index].offset
        }
        lock.unlock()
    }

    private func resetProgress() {
        lock.lock()
        _index = 0
        _elapsed = 0
        lock.unlock()
    }

    private func startIndex() -> Int {
        lock.lock()
        let index = _seekRequest ?? 0
        _seekRequest = nil
        _index = index
        _elapsed = index < encounter.samples.count ? encounter.samples[index].offset : 0
        lock.unlock()
        return index
    }

    private func consumeSeek() -> Int? {
        lock.lock(); defer { lock.unlock() }
        let value = _seekRequest
        _seekRequest = nil
        return value
    }

    private func hasSeekRequest() -> Bool {
        lock.lock(); defer { lock.unlock() }
        return _seekRequest != nil
    }

    private func consumeRestart() -> Bool {
        lock.lock(); defer { lock.unlock() }
        if _restartRequested {
            _restartRequested = false
            _index = 0
            _elapsed = 0
            return true
        }
        return false
    }
}

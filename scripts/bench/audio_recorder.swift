#!/usr/bin/env swift

// Native audio acquisition with source-clock continuity and encoded-frame checks.
// --self-test creates synthetic buffers only; it never discovers capture devices.
import AVFoundation
import AudioToolbox
import CoreMedia
import Darwin
import Foundation

let outputRate: Int32 = 22_050
let hostClock = CMClockGetHostTimeClock()

enum AudioFailure: String, Error {
    case arguments, existingOutput = "existing_output", outputPaths = "output_paths"
    case permission = "microphone_permission_not_granted", device = "exact_microphone_not_found"
    case input = "capture_input_rejected", output = "capture_output_rejected"
    case clock = "synchronization_clock_unavailable", timestamp = "invalid_source_timestamp"
    case format = "unsupported_source_format", formatChanged = "source_format_changed"
    case duration = "source_duration_mismatch", sourceOrder = "source_out_of_order"
    case sourceGap = "source_gap", sourceOverlap = "source_overlap", sourceSpan = "source_span_mismatch"
    case hostOrder = "host_time_out_of_order", hostConversion = "host_clock_conversion_failed"
    case hostDuration = "host_duration_mismatch", hostFuture = "host_start_after_callback"
    case buffer = "invalid_source_buffer", retiming = "sample_retiming_failed"
    case writer = "writer_failed", backpressure = "writer_backpressure"
    case encodedFormat = "encoded_format_mismatch", encodedFrames = "encoded_sample_count_mismatch"
    case noSamples = "no_samples", finalize = "writer_finalize_timeout", sidecar = "timing_sidecar_failed"
    case selfTest = "self_test_failed"
}

func nanoseconds(_ time: CMTime) throws -> Int64 {
    guard time.isValid, time.isNumeric, time.timescale > 0 else { throw AudioFailure.timestamp }
    let result = CMTimeConvertScale(time, timescale: 1_000_000_000, method: .roundHalfAwayFromZero)
    guard result.isValid, result.isNumeric else { throw AudioFailure.timestamp }
    return result.value
}

func nowNS() -> Int64 { (try? nanoseconds(CMClockGetTime(hostClock))) ?? 0 }

func hostNS(_ source: CMTime, clock: CMClock) throws -> Int64 {
    let converted = CMSyncConvertTime(source, from: clock, to: hostClock)
    guard converted.isValid, converted.isNumeric else { throw AudioFailure.hostConversion }
    return try nanoseconds(converted)
}

func jsonData(_ value: [String: Any]) throws -> Data {
    var data = try JSONSerialization.data(withJSONObject: value, options: [.sortedKeys])
    data.append(10)
    return data
}

func emit(_ value: [String: Any]) throws { try FileHandle.standardOutput.write(contentsOf: jsonData(value)) }

func failureName(_ error: Error) -> String {
    if let error = error as? AudioFailure { return error.rawValue }
    let value = error as NSError
    return "\(value.domain):\(value.code)"
}

struct Options {
    let device: String
    let output: URL
    let timing: URL
    let summary: URL
    let owner: pid_t
    let maximum: Double

    init(_ arguments: [String]) throws {
        let names = ["--device-name", "--output", "--timing-sidecar", "--summary", "--owner-pid", "--max-seconds"]
        guard arguments.count == names.count * 2 else { throw AudioFailure.arguments }
        var values: [String: String] = [:]
        for index in stride(from: 0, to: arguments.count, by: 2) {
            let key = arguments[index]
            guard names.contains(key), values[key] == nil else { throw AudioFailure.arguments }
            values[key] = arguments[index + 1]
        }
        guard let device = values["--device-name"], !device.isEmpty,
              device == device.trimmingCharacters(in: .whitespacesAndNewlines),
              !device.contains("\n"), let owner = Int32(values["--owner-pid"] ?? ""), owner > 1,
              owner != getpid(), let maximum = Double(values["--max-seconds"] ?? ""),
              maximum.isFinite, maximum > 0, maximum <= 240 else { throw AudioFailure.arguments }
        self.device = device
        self.output = URL(fileURLWithPath: values["--output"]!).standardizedFileURL
        self.timing = URL(fileURLWithPath: values["--timing-sidecar"]!).standardizedFileURL
        self.summary = URL(fileURLWithPath: values["--summary"]!).standardizedFileURL
        self.owner = owner
        self.maximum = maximum
        guard Set([output.path, timing.path, summary.path]).count == 3,
              output.pathExtension.lowercased() == "wav" else { throw AudioFailure.outputPaths }
        for url in [output, timing, summary] {
            var information = stat()
            guard lstat(url.path, &information) != 0, errno == ENOENT else { throw AudioFailure.existingOutput }
        }
    }
}

struct AudioFormat: Equatable {
    let rate: Int32
    let channels: UInt32
    let bits: UInt32
    let flags: UInt32
    let bytesPerFrame: UInt32

    init(_ description: CMAudioFormatDescription) throws {
        guard let pointer = CMAudioFormatDescriptionGetStreamBasicDescription(description) else {
            throw AudioFailure.format
        }
        let value = pointer.pointee
        guard value.mFormatID == kAudioFormatLinearPCM, value.mSampleRate.isFinite,
              value.mSampleRate.rounded() == value.mSampleRate, (8_000...192_000).contains(value.mSampleRate),
              (1...8).contains(value.mChannelsPerFrame), value.mFramesPerPacket == 1,
              value.mBytesPerFrame > 0, (8...64).contains(value.mBitsPerChannel) else { throw AudioFailure.format }
        rate = Int32(value.mSampleRate)
        channels = value.mChannelsPerFrame
        bits = value.mBitsPerChannel
        flags = value.mFormatFlags
        bytesPerFrame = value.mBytesPerFrame
    }

    var json: [String: Any] {
        ["sample_rate": rate, "channels": channels, "bits_per_channel": bits,
         "format_id": kAudioFormatLinearPCM, "format_flags": flags, "bytes_per_frame": bytesPerFrame]
    }
}

struct AudioTimeline {
    private(set) var origin: CMTime?
    private(set) var end: CMTime?
    private(set) var frames: Int64 = 0
    private var lastPTS: CMTime?
    private var rate: Int32?

    // A source PTS expressed in a non-sample-integral timebase can round a
    // boundary by one timebase tick. No complete audio sample may be missing.
    private func roundingTolerance(_ time: CMTime, rate: Int32) -> Double {
        if time.timescale % rate == 0 || rate % time.timescale == 0 { return 0 }
        return min(1.5 / Double(time.timescale), 0.25 / Double(rate))
    }

    mutating func admit(pts: CMTime, duration: CMTime, count: Int, rate: Int32) throws -> CMTime {
        guard pts.isValid, pts.isNumeric, pts.timescale >= rate, count > 0,
              duration.isValid, duration.isNumeric, duration.timescale > 0 else { throw AudioFailure.timestamp }
        if let oldRate = self.rate, oldRate != rate { throw AudioFailure.formatChanged }
        let expected = CMTime(value: Int64(count), timescale: rate)
        guard abs(CMTimeGetSeconds(CMTimeSubtract(duration, expected))) <= roundingTolerance(duration, rate: rate) else {
            throw AudioFailure.duration
        }
        if let lastPTS, CMTimeCompare(pts, lastPTS) <= 0 { throw AudioFailure.sourceOrder }
        if let end {
            let gap = CMTimeGetSeconds(CMTimeSubtract(pts, end))
            let tolerance = roundingTolerance(pts, rate: rate)
            if gap > tolerance { throw AudioFailure.sourceGap }
            if gap < -tolerance { throw AudioFailure.sourceOverlap }
        }
        let first = origin ?? pts
        let nextEnd = CMTimeAdd(pts, expected)
        let totalFrames = frames + Int64(count)
        guard abs(CMTimeGetSeconds(CMTimeSubtract(nextEnd, first)) - Double(totalFrames) / Double(rate))
                <= max(roundingTolerance(pts, rate: rate), 0.5 / Double(rate)) else { throw AudioFailure.sourceSpan }
        self.rate = rate
        origin = first
        lastPTS = pts
        end = nextEnd
        frames = totalFrames
        return CMTimeSubtract(pts, first)
    }
}

struct HostAudioTimeline {
    private(set) var lastStart: Int64?
    private(set) var originMinimum: Int64?
    private(set) var originMaximum: Int64?

    mutating func admit(start: Int64, end: Int64, callback: Int64,
                        sourceOffset: CMTime, sourceDuration: CMTime, rate: Int32) throws {
        guard end > start,
              abs(Double(end - start) - CMTimeGetSeconds(sourceDuration) * 1e9) <= 1e9 / Double(rate) else {
            throw AudioFailure.hostDuration
        }
        guard start <= callback else { throw AudioFailure.hostFuture }
        if let lastStart, start <= lastStart { throw AudioFailure.hostOrder }

        // CMSyncConvertTime can recalibrate the source-to-host mapping between
        // callbacks. Source continuity is checked separately in AudioTimeline;
        // adjacent converted endpoints need not touch after recalibration.
        // Retain every observed origin, including mapping drift within a buffer,
        // so downstream cue checks use the whole measured uncertainty interval.
        for (mapped, offset) in [(start, sourceOffset), (end, CMTimeAdd(sourceOffset, sourceDuration))] {
            let floor = CMTimeConvertScale(offset, timescale: 1_000_000_000, method: .roundTowardNegativeInfinity)
            let ceiling = CMTimeConvertScale(offset, timescale: 1_000_000_000, method: .roundTowardPositiveInfinity)
            guard floor.isValid, floor.isNumeric, ceiling.isValid, ceiling.isNumeric else { throw AudioFailure.hostConversion }
            let lower = mapped - ceiling.value - 1
            let upper = mapped - floor.value + 1
            originMinimum = min(originMinimum ?? lower, lower)
            originMaximum = max(originMaximum ?? upper, upper)
        }
        lastStart = start
    }
}

func retimed(_ sample: CMSampleBuffer, at pts: CMTime, rate: Int32) throws -> CMSampleBuffer {
    var timing = CMSampleTimingInfo(duration: CMTime(value: 1, timescale: rate),
                                    presentationTimeStamp: pts, decodeTimeStamp: .invalid)
    var result: CMSampleBuffer?
    let status = CMSampleBufferCreateCopyWithNewTiming(allocator: kCFAllocatorDefault,
        sampleBuffer: sample, sampleTimingEntryCount: 1, sampleTimingArray: &timing, sampleBufferOut: &result)
    guard status == noErr, let result else { throw AudioFailure.retiming }
    return result
}

final class AudioWriter {
    let writer: AVAssetWriter
    let input: AVAssetWriterInput
#if compiler(>=6.4)
    let receiver: AVAssetWriterInput.SampleBufferReceiver
#endif

    init(_ url: URL) throws {
        writer = try AVAssetWriter(outputURL: url, fileType: .wav)
        let settings: [String: Any] = [AVFormatIDKey: kAudioFormatLinearPCM,
            AVSampleRateKey: outputRate, AVNumberOfChannelsKey: 1, AVLinearPCMBitDepthKey: 16,
            AVLinearPCMIsBigEndianKey: false, AVLinearPCMIsFloatKey: false,
            AVLinearPCMIsNonInterleaved: false]
        guard writer.canApply(outputSettings: settings, forMediaType: .audio) else { throw AudioFailure.writer }
        input = AVAssetWriterInput(mediaType: .audio, outputSettings: settings)
        guard writer.canAdd(input) else { throw AudioFailure.writer }
#if compiler(>=6.4)
        receiver = writer.inputReceiver(for: input)
        try writer.start()
#else
        input.expectsMediaDataInRealTime = true
        writer.add(input)
        guard writer.startWriting() else { throw AudioFailure.writer }
#endif
        writer.startSession(atSourceTime: .zero)
    }

    func append(_ sample: CMSampleBuffer) throws {
        guard writer.status == .writing else { throw AudioFailure.writer }
#if compiler(>=6.4)
        let ready = CMReadySampleBuffer<CMSampleBuffer.DynamicContent>(unsafeBuffer: sample)
        guard try receiver.appendImmediately(ready) else { throw AudioFailure.backpressure }
#else
        guard input.isReadyForMoreMediaData else { throw AudioFailure.backpressure }
        guard input.append(sample) else { throw AudioFailure.writer }
#endif
    }

    func finish() throws {
        guard writer.status == .writing else { throw AudioFailure.writer }
#if compiler(>=6.4)
        receiver.finish()
#else
        input.markAsFinished()
#endif
        let done = DispatchSemaphore(value: 0)
        writer.finishWriting { done.signal() }
        guard done.wait(timeout: .now() + 10) == .success else {
            writer.cancelWriting()
            throw AudioFailure.finalize
        }
        guard writer.status == .completed else { throw AudioFailure.writer }
    }
}

func encodedFrames(_ path: URL, expectedDuration: Double) throws -> Int64 {
    let file = try AVAudioFile(forReading: path)
    let format = file.fileFormat.streamDescription.pointee
    guard format.mFormatID == kAudioFormatLinearPCM, format.mSampleRate == Double(outputRate),
          format.mChannelsPerFrame == 1, format.mBitsPerChannel == 16,
          format.mFormatFlags & (kAudioFormatFlagIsFloat | kAudioFormatFlagIsBigEndian) == 0 else {
        throw AudioFailure.encodedFormat
    }
    // Rate conversion may round the final fractional output sample, but a
    // missing source buffer or writer truncation must never be padded to pass.
    guard abs(Double(file.length) - expectedDuration * Double(outputRate)) <= 1 else {
        throw AudioFailure.encodedFrames
    }
    return file.length
}

final class StopControl: @unchecked Sendable {
    private let lock = NSLock()
    private var value: String?
    private var lastCallback: Int64?
    func request(_ reason: String) { lock.lock(); defer { lock.unlock() }; value = value ?? reason }
    var reason: String? { lock.lock(); defer { lock.unlock() }; return value }
    func observed(_ callback: Int64) { lock.lock(); defer { lock.unlock() }; lastCallback = callback }
    func stalled(now: Int64) -> Bool {
        lock.lock(); defer { lock.unlock() }
        return lastCallback.map { now - $0 > 3_000_000_000 } ?? false
    }
}

final class AudioRecorder: NSObject, AVCaptureAudioDataOutputSampleBufferDelegate {
    let queue = DispatchQueue(label: "v1simple.audio.capture", qos: .userInitiated)
    private let options: Options
    private let writer: AudioWriter
    private let sidecar: FileHandle
    private let stop: StopControl
    private var clock: CMClock?
    private var timeline = AudioTimeline()
    private var hostTimeline = HostAudioTimeline()
    private var format: AudioFormat?
    private var failure: String?
    private var buffers = 0
    private var timingErrors = 0
    private var writerDrops = 0
    private var firstHost: Int64?
    private var lastHost: Int64?
    private var readyHost: Int64?

    init(options: Options, stop: StopControl) throws {
        self.options = options
        self.stop = stop
        let fd = open(options.timing.path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0o600)
        guard fd >= 0 else { throw AudioFailure.sidecar }
        sidecar = FileHandle(fileDescriptor: fd, closeOnDealloc: true)
        writer = try AudioWriter(options.output)
    }

    func setClock(_ clock: CMClock) { queue.sync { self.clock = clock } }

    func healthProblem(now: Int64) -> String? {
        // The owner/max-duration watchdog must not queue behind disk or writer
        // work on the capture delegate. Only a tiny lock protects its heartbeat.
        stop.reason ?? (stop.stalled(now: now) ? "native_audio_stalled" : nil)
    }

    func captureOutput(_ output: AVCaptureOutput, didOutput sample: CMSampleBuffer, from connection: AVCaptureConnection) {
        // Startup buffers before the session supplies its clock cannot establish
        // readiness. Every buffer after the first admitted one is accounted for.
        guard let clock, failure == nil else { return }
        let callback = nowNS()
        var record: [String: Any] = ["schema_version": 1, "event": "audio_buffer", "buffer_seq": buffers + 1,
                                   "callback_host_ns": callback]
        do {
            guard CMSampleBufferIsValid(sample), CMSampleBufferDataIsReady(sample),
                  let description = CMSampleBufferGetFormatDescription(sample) else { throw AudioFailure.buffer }
            let observed = try AudioFormat(description)
            guard format == nil || format == observed else { throw AudioFailure.formatChanged }
            let pts = CMSampleBufferGetPresentationTimeStamp(sample)
            let duration = CMSampleBufferGetDuration(sample)
            let count = CMSampleBufferGetNumSamples(sample)
            record.merge(["source_pts_value": pts.value, "source_pts_timescale": pts.timescale,
                          "source_duration_value": duration.value, "source_duration_timescale": duration.timescale,
                          "source_sample_frames": count, "source_format": observed.json]) { _, new in new }
            let relative = try timeline.admit(pts: pts, duration: duration, count: count, rate: observed.rate)
            let begin = try hostNS(pts, clock: clock)
            let end = try hostNS(CMTimeAdd(pts, duration), clock: clock)
            record.merge(["host_start_ns": begin, "host_end_ns": end,
                          "audio_pts_value": relative.value, "audio_pts_timescale": relative.timescale]) { _, new in new }
            record["previous_host_end_ns"] = lastHost as Any? ?? NSNull()
            record["previous_host_start_ns"] = hostTimeline.lastStart as Any? ?? NSNull()
            try hostTimeline.admit(start: begin, end: end, callback: callback,
                                   sourceOffset: relative, sourceDuration: duration, rate: observed.rate)
            try writer.append(retimed(sample, at: relative, rate: observed.rate))
            record["status"] = "written"
            try sidecar.write(contentsOf: jsonData(record))
            format = observed
            buffers += 1
            firstHost = firstHost ?? begin
            lastHost = end
            stop.observed(callback)
            if buffers == 1 {
                readyHost = nowNS()
                try emit(["schema_version": 1, "event": "ready", "device": options.device,
                    "source_clock": "avcapture_session_synchronization_clock",
                    "first_host_ns": begin, "ready_host_ns": readyHost!, "source_format": observed.json,
                    "source_duration_s": Double(timeline.frames) / Double(observed.rate),
                    "output_format": ["sample_rate": outputRate, "channels": 1, "bits_per_channel": 16]])
            }
        } catch {
            let name = failureName(error)
            if let error = error as? AudioFailure, error == .backpressure || error == .writer { writerDrops += 1 }
            else { timingErrors += 1 }
            failure = name
            record["status"] = "failed"
            record["reason"] = name
            try? sidecar.write(contentsOf: jsonData(record))
            stop.request(name)
        }
    }

    func finish(shutdown: String) -> [String: Any] {
        queue.sync {
            var frames: Int64 = 0
            let duration = format.map { Double(timeline.frames) / Double($0.rate) } ?? 0
            if shutdown != "q" { failure = failure ?? shutdown }
            do {
                guard buffers > 0 else { throw AudioFailure.noSamples }
                try writer.finish()
                frames = try encodedFrames(options.output, expectedDuration: duration)
                try sidecar.synchronize()
                try sidecar.close()
            } catch {
                failure = failure ?? failureName(error)
                if writer.writer.status == .writing || writer.writer.status == .unknown { writer.writer.cancelWriting() }
                try? sidecar.close()
            }
            return ["schema_version": 1, "event": "finished", "device": options.device,
                "result": failure == nil ? "CAPTURED" : "INCOMPLETE", "reason": failure ?? "none",
                "shutdown_reason": shutdown, "source_clock": "avcapture_session_synchronization_clock",
                "first_host_ns": firstHost as Any? ?? NSNull(), "last_host_ns": lastHost as Any? ?? NSNull(),
                "origin_min_host_ns": hostTimeline.originMinimum as Any? ?? NSNull(),
                "origin_max_host_ns": hostTimeline.originMaximum as Any? ?? NSNull(),
                "ready_host_ns": readyHost as Any? ?? NSNull(), "finished_host_ns": nowNS(),
                "source_format": format?.json as Any? ?? NSNull(), "source_buffers": buffers,
                "source_sample_frames": timeline.frames, "source_duration_s": duration,
                "encoded_sample_frames": frames, "duration_s": Double(frames) / Double(outputRate),
                "sample_rate": outputRate, "channels": 1, "bits_per_channel": 16,
                "timing_errors": timingErrors, "writer_drops": writerDrops]
        }
    }
}

func syntheticBuffer(rate: Int32, frameOffset: Int, count: Int, pts: CMTime) throws -> CMSampleBuffer {
    var asbd = AudioStreamBasicDescription(mSampleRate: Double(rate), mFormatID: kAudioFormatLinearPCM,
        mFormatFlags: kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked,
        mBytesPerPacket: 2, mFramesPerPacket: 1, mBytesPerFrame: 2, mChannelsPerFrame: 1,
        mBitsPerChannel: 16, mReserved: 0)
    var description: CMAudioFormatDescription?
    guard CMAudioFormatDescriptionCreate(allocator: kCFAllocatorDefault, asbd: &asbd,
        layoutSize: 0, layout: nil, magicCookieSize: 0, magicCookie: nil,
        extensions: nil, formatDescriptionOut: &description) == noErr, let description else { throw AudioFailure.selfTest }
    let samples = (0..<count).map { index -> Int16 in
        let frame = Double(frameOffset + index)
        let phase = frame * 2 * Double.pi * 440 / Double(rate)
        let amplitude = sin(phase) * 10_000
        return Int16(amplitude.rounded())
    }
    var block: CMBlockBuffer?
    guard CMBlockBufferCreateWithMemoryBlock(allocator: kCFAllocatorDefault, memoryBlock: nil,
        blockLength: count * 2, blockAllocator: kCFAllocatorDefault, customBlockSource: nil,
        offsetToData: 0, dataLength: count * 2, flags: 0, blockBufferOut: &block) == noErr, let block else { throw AudioFailure.selfTest }
    let status = samples.withUnsafeBytes { bytes in
        CMBlockBufferReplaceDataBytes(with: bytes.baseAddress!, blockBuffer: block, offsetIntoDestination: 0, dataLength: bytes.count)
    }
    guard status == noErr else { throw AudioFailure.selfTest }
    var result: CMSampleBuffer?
    guard CMAudioSampleBufferCreateReadyWithPacketDescriptions(allocator: kCFAllocatorDefault,
        dataBuffer: block, formatDescription: description, sampleCount: count,
        presentationTimeStamp: pts, packetDescriptions: nil, sampleBufferOut: &result) == noErr,
        let result else { throw AudioFailure.selfTest }
    return result
}

func selfTest() throws {
    func require(_ condition: Bool) throws { if !condition { throw AudioFailure.selfTest } }
    let directory = FileManager.default.temporaryDirectory.appendingPathComponent("audio-recorder-test-\(UUID().uuidString)")
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: false)
    defer { try? FileManager.default.removeItem(at: directory) }
    for rate: Int32 in [22_050, 44_100, 48_000] {
        let path = directory.appendingPathComponent("\(rate).wav")
        let writer = try AudioWriter(path)
        var timeline = AudioTimeline()
        let count = Int(rate / 50)
        for index in 0..<100 {
            let pts = CMTime(value: Int64(rate) * 100 + Int64(index * count), timescale: rate)
            let sample = try syntheticBuffer(rate: rate, frameOffset: index * count, count: count, pts: pts)
            let relative = try timeline.admit(pts: pts, duration: CMSampleBufferGetDuration(sample), count: count, rate: rate)
            // Synthetic delivery is intentionally paced; live capture may not
            // wait or discard a buffer when the writer reports backpressure.
            try writer.append(retimed(sample, at: relative, rate: rate))
            Thread.sleep(forTimeInterval: 0.002)
        }
        try writer.finish()
        try require(try encodedFrames(path, expectedDuration: 2) == Int64(outputRate) * 2)
        do {
            _ = try encodedFrames(path, expectedDuration: 2.25)
            throw AudioFailure.selfTest
        } catch let failure as AudioFailure { try require(failure == .encodedFrames) }
        let file = try AVAudioFile(forReading: path)
        let buffer = AVAudioPCMBuffer(pcmFormat: file.processingFormat, frameCapacity: AVAudioFrameCount(file.length))!
        try file.read(into: buffer)
        let values = buffer.floatChannelData![0]
        let countOut = Int(buffer.frameLength)
        let energy = (0..<countOut).reduce(0.0) { $0 + Double(values[$1] * values[$1]) }
        try require(countOut == Int(outputRate) * 2 && energy / Double(countOut) > 0.01)
    }
    for (offset, expected): (Int64, AudioFailure) in [(481, .sourceGap), (479, .sourceOverlap), (0, .sourceOrder)] {
        var timeline = AudioTimeline()
        _ = try timeline.admit(pts: CMTime(value: 0, timescale: 48_000), duration: CMTime(value: 480, timescale: 48_000), count: 480, rate: 48_000)
        do {
            _ = try timeline.admit(pts: CMTime(value: offset, timescale: 48_000), duration: CMTime(value: 480, timescale: 48_000), count: 480, rate: 48_000)
            throw AudioFailure.selfTest
        } catch let failure as AudioFailure { try require(failure == expected) }
    }
    var mapping = HostAudioTimeline()
    let nativeDuration = CMTime(value: 512, timescale: 48_000)
    // Synthetic clock recalibration moves the second mapped start 8,000ns
    // before the prior mapped end despite exact native 512-frame continuity.
    try mapping.admit(start: 10_000_000_000, end: 10_010_666_667, callback: 10_012_000_000,
                      sourceOffset: .zero, sourceDuration: nativeDuration, rate: 48_000)
    try mapping.admit(start: 10_010_658_667, end: 10_021_325_334, callback: 10_023_000_000,
                      sourceOffset: nativeDuration, sourceDuration: nativeDuration, rate: 48_000)
    try require(mapping.originMinimum == 9_999_991_999 && mapping.originMaximum == 10_000_000_002)
    for (start, end, callback, expected): (Int64, Int64, Int64, AudioFailure) in [
        (10_010_658_667, 10_021_325_334, 10_023_000_000, .hostOrder),
        (10_021_325_334, 10_031_992_001, 10_021_325_333, .hostFuture),
        (10_021_325_334, 10_032_092_001, 10_034_000_000, .hostDuration),
    ] {
        do {
            try mapping.admit(start: start, end: end, callback: callback,
                sourceOffset: CMTime(value: 1024, timescale: 48_000), sourceDuration: nativeDuration, rate: 48_000)
            throw AudioFailure.selfTest
        } catch let failure as AudioFailure { try require(failure == expected) }
    }
    let time = CMClockGetTime(hostClock)
    try require(try hostNS(time, clock: hostClock) == nanoseconds(time))
    let existing = directory.appendingPathComponent("existing.wav")
    try Data("preserve".utf8).write(to: existing)
    do {
        _ = try Options(["--device-name", "Fixture", "--output", existing.path,
            "--timing-sidecar", directory.appendingPathComponent("timing.ndjson").path,
            "--summary", directory.appendingPathComponent("summary.json").path,
            "--owner-pid", String(getppid()), "--max-seconds", "240"])
        throw AudioFailure.selfTest
    } catch let failure as AudioFailure { try require(failure == .existingOutput) }
    try require(try Data(contentsOf: existing) == Data("preserve".utf8))
    try emit(["schema_version": 1, "event": "self_test", "result": "PASS", "checks": ["native_pcm_writer_22050_44100_48000",
        "encoded_frame_count", "truncated_duration_rejected", "nonzero_encoded_signal", "source_gap",
        "source_overlap", "source_order", "host_clock_conversion", "host_recalibration_origin_bounds",
        "host_order_future_duration_rejected", "existing_output_preserved"]])
}

func run(_ options: Options) throws -> Int32 {
    guard AVCaptureDevice.authorizationStatus(for: .audio) == .authorized else { throw AudioFailure.permission }
    let discovery = AVCaptureDevice.DiscoverySession(deviceTypes: [.microphone, .external], mediaType: .audio, position: .unspecified)
    let matches = discovery.devices.filter { $0.localizedName == options.device }
    guard matches.count == 1, let device = matches.first else { throw AudioFailure.device }
    let session = AVCaptureSession()
    let input = try AVCaptureDeviceInput(device: device)
    guard session.canAddInput(input) else { throw AudioFailure.input }
    session.addInput(input)
    let output = AVCaptureAudioDataOutput()
    output.audioSettings = nil
    guard session.canAddOutput(output) else { throw AudioFailure.output }
    session.addOutput(output)
    let stop = StopControl()
    let recorder = try AudioRecorder(options: options, stop: stop)
    output.setSampleBufferDelegate(recorder, queue: recorder.queue)
    let runtime = NotificationCenter.default.addObserver(forName: AVCaptureSession.runtimeErrorNotification, object: session, queue: nil) { _ in
        stop.request("capture_session_runtime_error")
    }
    defer { NotificationCenter.default.removeObserver(runtime) }
    signal(SIGINT, SIG_IGN); signal(SIGTERM, SIG_IGN); signal(SIGPIPE, SIG_IGN)
    let interrupt = DispatchSource.makeSignalSource(signal: SIGINT, queue: .global())
    let terminate = DispatchSource.makeSignalSource(signal: SIGTERM, queue: .global())
    interrupt.setEventHandler { stop.request("signal_interrupt") }
    terminate.setEventHandler { stop.request("signal_terminate") }
    interrupt.resume(); terminate.resume()
    defer { interrupt.cancel(); terminate.cancel() }
    let stdin = DispatchSource.makeReadSource(fileDescriptor: STDIN_FILENO, queue: DispatchQueue(label: "v1simple.audio.stdin"))
    stdin.setEventHandler {
        var bytes = [UInt8](repeating: 0, count: 256)
        let count = read(STDIN_FILENO, &bytes, bytes.count)
        if count <= 0 { stop.request("stdin_closed"); return }
        if String(bytes: bytes.prefix(count), encoding: .utf8)?.trimmingCharacters(in: .whitespacesAndNewlines) == "q" {
            stop.request("q")
        } else { stop.request("invalid_stdin_command") }
    }
    stdin.resume()
    defer { stdin.cancel() }
    let started = nowNS()
    session.startRunning()
    guard let clock = session.synchronizationClock else {
        session.stopRunning()
        throw AudioFailure.clock
    }
    recorder.setClock(clock)
    while stop.reason == nil {
        let now = nowNS()
        if kill(options.owner, 0) != 0 && errno == ESRCH { stop.request("owner_lost") }
        if Double(now - started) / 1e9 >= options.maximum { stop.request("maximum_duration") }
        if let problem = recorder.healthProblem(now: now) { stop.request(problem) }
        Thread.sleep(forTimeInterval: 0.02)
    }
    session.stopRunning()
    output.setSampleBufferDelegate(nil, queue: nil)
    let summary = recorder.finish(shutdown: stop.reason ?? "unknown")
    try jsonData(summary).write(to: options.summary, options: .withoutOverwriting)
    try emit(summary)
    return summary["result"] as? String == "CAPTURED" ? 0 : 1
}

umask(0o077)
do {
    if Array(CommandLine.arguments.dropFirst()) == ["--self-test"] {
        try selfTest()
        exit(0)
    }
    let options = try Options(Array(CommandLine.arguments.dropFirst()))
    do { exit(try run(options)) }
    catch {
        let payload: [String: Any] = ["schema_version": 1, "event": "finished", "result": "INCOMPLETE",
                                    "reason": failureName(error), "shutdown_reason": "setup_or_finalization_failed"]
        try? jsonData(payload).write(to: options.summary, options: .withoutOverwriting)
        try? emit(payload)
        exit(1)
    }
} catch {
    fputs("audio recorder: \(failureName(error))\n", stderr)
    exit(2)
}

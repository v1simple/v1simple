#!/usr/bin/env swift

import AVFoundation
import CoreMedia
import CoreVideo
import Darwin
import Foundation

struct Options {
    let deviceName: String
    let width: Int32
    let height: Int32
    let frameRate: Int32
    let pixelFormat: OSType
    let pixelFormatName: String
    let output: URL
    let preflightOutput: URL
    let timingSidecar: URL
    let preflightTimingSidecar: URL
    let sessionReady: URL
    let startMarker: URL
    let preflightReady: URL
    let preflightStop: URL
    let preflightFinished: URL
    let recordingReady: URL
    let firstFrameMarker: URL
    let failureMarker: URL
    let statsMarker: URL
    let finalizeTimeoutSeconds: TimeInterval
    let preflightFinalizeTimeoutSeconds: TimeInterval
}

func argument(_ name: String) -> String {
    guard let index = CommandLine.arguments.firstIndex(of: name), index + 1 < CommandLine.arguments.count else {
        fputs("missing required argument \(name)\n", stderr)
        exit(2)
    }
    return CommandLine.arguments[index + 1]
}

func parseVideoSize(_ value: String) -> (Int32, Int32) {
    let parts = value.split(separator: "x", maxSplits: 1).compactMap { Int32($0) }
    guard parts.count == 2, parts[0] > 0, parts[1] > 0 else {
        fputs("invalid video size \(value)\n", stderr)
        exit(2)
    }
    return (parts[0], parts[1])
}

func parseOptions() -> Options {
    let (width, height) = parseVideoSize(argument("--video-size"))
    guard let frameRate = Int32(argument("--framerate")), frameRate > 0 else {
        fputs("invalid frame rate\n", stderr)
        exit(2)
    }
    let pixelFormatName = argument("--pixel-format")
    guard let finalizeTimeoutSeconds = Double(argument("--finalize-timeout-seconds")),
          finalizeTimeoutSeconds > 0 else {
        fputs("invalid finalize timeout\n", stderr)
        exit(2)
    }
    guard let preflightFinalizeTimeoutSeconds = Double(
        argument("--preflight-finalize-timeout-seconds")
    ), preflightFinalizeTimeoutSeconds > 0 else {
        fputs("invalid preflight finalize timeout\n", stderr)
        exit(2)
    }
    let pixelFormat: OSType
    switch pixelFormatName {
    case "nv12":
        pixelFormat = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange
    case "yuy2":
        pixelFormat = kCVPixelFormatType_422YpCbCr8_yuvs
    default:
        fputs("unsupported pixel format \(pixelFormatName)\n", stderr)
        exit(2)
    }
    return Options(
        deviceName: argument("--device-name"),
        width: width,
        height: height,
        frameRate: frameRate,
        pixelFormat: pixelFormat,
        pixelFormatName: pixelFormatName,
        output: URL(fileURLWithPath: argument("--output")),
        preflightOutput: URL(fileURLWithPath: argument("--preflight-output")),
        timingSidecar: URL(fileURLWithPath: argument("--timing-sidecar")),
        preflightTimingSidecar: URL(fileURLWithPath: argument("--preflight-timing-sidecar")),
        sessionReady: URL(fileURLWithPath: argument("--session-ready")),
        startMarker: URL(fileURLWithPath: argument("--start-marker")),
        preflightReady: URL(fileURLWithPath: argument("--preflight-ready")),
        preflightStop: URL(fileURLWithPath: argument("--preflight-stop")),
        preflightFinished: URL(fileURLWithPath: argument("--preflight-finished")),
        recordingReady: URL(fileURLWithPath: argument("--recording-ready")),
        firstFrameMarker: URL(fileURLWithPath: argument("--first-frame-marker")),
        failureMarker: URL(fileURLWithPath: argument("--failure-marker")),
        statsMarker: URL(fileURLWithPath: argument("--stats-marker")),
        finalizeTimeoutSeconds: finalizeTimeoutSeconds,
        preflightFinalizeTimeoutSeconds: preflightFinalizeTimeoutSeconds
    )
}

func writeMarker(_ url: URL, _ payload: [String: Any]) throws {
    let data = try JSONSerialization.data(withJSONObject: payload, options: [.prettyPrinted, .sortedKeys])
    try data.write(to: url, options: .atomic)
}

enum SampleTimingError: String, Error {
    case invalidSourcePresentationTime = "invalid_source_pts"
    case invalidSourceDuration = "invalid_source_duration"
    case nonMonotonicSourcePresentationTime = "non_monotonic_source_pts"
    case invalidRelativePresentationTime = "invalid_relative_pts"
    case nonMonotonicVideoPresentationTime = "non_monotonic_video_pts"
    case invalidVideoDuration = "invalid_video_duration"
    case unsupportedWriterTimeScale = "unsupported_writer_timescale"
    case inexactVideoPresentationTime = "inexact_video_pts"
    case inexactVideoDuration = "inexact_video_duration"
    case synchronizationClockUnavailable = "synchronization_clock_unavailable"
    case hostClockConversionFailed = "host_clock_conversion_failed"
    case sampleRetimingFailed = "sample_retiming_failed"
}

struct ResolvedVideoTiming {
    let presentationTime: CMTime
    let duration: CMTime
    let writerTimeScale: CMTimeScale
}

struct SourcePresentationTimeline {
    private var firstSourcePresentationTime: CMTime?
    private var lastSourcePresentationTime: CMTime?
    private var lastVideoPresentationTime: CMTime?
    private var writerTimeScale: CMTimeScale?

    private func greatestCommonDivisor(_ left: Int64, _ right: Int64) -> Int64 {
        var a = abs(left)
        var b = abs(right)
        while b != 0 {
            let remainder = a % b
            a = b
            b = remainder
        }
        return a
    }

    private func exactWriterTimeScale(
        presentationTimeScale: CMTimeScale,
        duration: CMTime
    ) throws -> CMTimeScale {
        let left = Int64(presentationTimeScale)
        let durationScale = Int64(duration.timescale)
        let durationDivisor = greatestCommonDivisor(duration.value, durationScale)
        let right = durationScale / max(durationDivisor, 1)
        let divisor = greatestCommonDivisor(left, right)
        let (product, overflow) = (left / divisor).multipliedReportingOverflow(by: right)
        guard !overflow, product > 0, product <= Int64(Int32.max) else {
            throw SampleTimingError.unsupportedWriterTimeScale
        }
        return CMTimeScale(product)
    }

    private func convertExactly(
        _ time: CMTime,
        to timeScale: CMTimeScale,
        error: SampleTimingError
    ) throws -> CMTime {
        let converted = CMTimeConvertScale(time, timescale: timeScale, method: .roundTowardZero)
        guard converted.isValid,
              converted.isNumeric,
              CMTimeCompare(converted, time) == 0 else {
            throw error
        }
        return converted
    }

    mutating func resolve(sourcePresentationTime: CMTime, sourceDuration: CMTime) throws -> ResolvedVideoTiming {
        guard sourcePresentationTime.isValid,
              sourcePresentationTime.isNumeric,
              sourcePresentationTime.timescale > 0 else {
            throw SampleTimingError.invalidSourcePresentationTime
        }
        if let lastSourcePresentationTime,
           CMTimeCompare(sourcePresentationTime, lastSourcePresentationTime) <= 0 {
            throw SampleTimingError.nonMonotonicSourcePresentationTime
        }

        let origin = firstSourcePresentationTime ?? sourcePresentationTime
        // Observing a valid source PTS is independent of whether its duration
        // can be written. A rejected sample must still preserve its gap in the
        // source-relative movie timeline that follows it.
        firstSourcePresentationTime = origin
        lastSourcePresentationTime = sourcePresentationTime

        guard sourceDuration.isValid,
              sourceDuration.isNumeric,
              sourceDuration.timescale > 0,
              CMTimeCompare(sourceDuration, .zero) > 0 else {
            throw SampleTimingError.invalidSourceDuration
        }
        let relative = CMTimeSubtract(sourcePresentationTime, origin)
        guard relative.isValid, relative.isNumeric, CMTimeCompare(relative, .zero) >= 0 else {
            throw SampleTimingError.invalidRelativePresentationTime
        }
        let resolvedTimeScale = try writerTimeScale ?? exactWriterTimeScale(
            presentationTimeScale: sourcePresentationTime.timescale,
            duration: sourceDuration
        )
        let presentationTime = try convertExactly(
            relative,
            to: resolvedTimeScale,
            error: .inexactVideoPresentationTime
        )
        let duration = try convertExactly(
            sourceDuration,
            to: resolvedTimeScale,
            error: .inexactVideoDuration
        )
        guard duration.isValid, duration.isNumeric, CMTimeCompare(duration, .zero) > 0 else {
            throw SampleTimingError.invalidVideoDuration
        }
        if let lastVideoPresentationTime,
           CMTimeCompare(presentationTime, lastVideoPresentationTime) <= 0 {
            throw SampleTimingError.nonMonotonicVideoPresentationTime
        }

        lastVideoPresentationTime = presentationTime
        writerTimeScale = resolvedTimeScale
        return ResolvedVideoTiming(
            presentationTime: presentationTime,
            duration: duration,
            writerTimeScale: resolvedTimeScale
        )
    }
}

func nonWrittenSampleCount(sourceSamples: Int, writtenFrames: Int) -> Int {
    max(sourceSamples - writtenFrames, 0)
}

func nanoseconds(_ time: CMTime) -> Int64? {
    guard time.isValid, time.isNumeric else { return nil }
    let scaled = CMTimeConvertScale(
        time,
        timescale: 1_000_000_000,
        method: .roundHalfAwayFromZero
    )
    guard scaled.isValid, scaled.isNumeric else { return nil }
    return scaled.value
}

func hostNanoseconds(_ sourceTime: CMTime, from sourceClock: CMClock, to hostClock: CMClock) -> Int64? {
    nanoseconds(CMSyncConvertTime(sourceTime, from: sourceClock, to: hostClock))
}

func sanitizedError(_ error: Error?) -> [String: Any] {
    guard let error else { return [:] }
    let nsError = error as NSError
    var payload: [String: Any] = [
        "domain": nsError.domain,
        "code": nsError.code,
    ]
    if let underlying = nsError.userInfo[NSUnderlyingErrorKey] as? NSError {
        payload["underlying"] = [
            "domain": underlying.domain,
            "code": underlying.code,
        ]
    }
    return payload
}

func runTimingSelfTest() -> Never {
    var timeline = SourcePresentationTimeline()
    let sourceTimes = [10_000_000_000, 10_005_001_000, 10_010_002_777, 10_015_004_123].map {
        CMTime(value: Int64($0), timescale: 1_000_000_000)
    }
    let duration = CMTime(value: 5_000_000, timescale: 1_000_000_000)
    let outputs = sourceTimes.compactMap {
        try? timeline.resolve(sourcePresentationTime: $0, sourceDuration: duration).presentationTime
    }
    let expectedValues: [Int64] = [0, 5_001_000, 10_002_777, 15_004_123]
    var rejectedRegression = false
    do {
        _ = try timeline.resolve(
            sourcePresentationTime: CMTime(value: 10_015_004_122, timescale: 1_000_000_000),
            sourceDuration: duration
        )
    } catch SampleTimingError.nonMonotonicSourcePresentationTime {
        rejectedRegression = true
    } catch {}
    let hostClock = CMClockGetHostTimeClock()
    let hostTime = CMClockGetTime(hostClock)
    let convertedHostNanoseconds = hostNanoseconds(hostTime, from: hostClock, to: hostClock)

    var invalidDurationTimeline = SourcePresentationTimeline()
    var rejectedInvalidDuration = false
    do {
        _ = try invalidDurationTimeline.resolve(
            sourcePresentationTime: CMTime(value: 20_000_000_000, timescale: 1_000_000_000),
            sourceDuration: .zero
        )
    } catch SampleTimingError.invalidSourceDuration {
        rejectedInvalidDuration = true
    } catch {}
    let afterInvalidDuration = try? invalidDurationTimeline.resolve(
        sourcePresentationTime: CMTime(value: 20_005_000_000, timescale: 1_000_000_000),
        sourceDuration: duration
    )

    var reducedDurationTimeline = SourcePresentationTimeline()
    let reducedDuration = try? reducedDurationTimeline.resolve(
        sourcePresentationTime: CMTime(value: 30_000_000_001, timescale: 1_000_000_000),
        sourceDuration: CMTime(value: 3, timescale: 600)
    )
    let passed = zip(outputs, expectedValues).allSatisfy {
        $0.value == $1 && $0.timescale == 1_000_000_000
    }
        && outputs.count == expectedValues.count
        && rejectedRegression
        && convertedHostNanoseconds == nanoseconds(hostTime)
        && rejectedInvalidDuration
        && afterInvalidDuration?.presentationTime.value == 5_000_000
        && afterInvalidDuration?.presentationTime.timescale == 1_000_000_000
        && reducedDuration?.writerTimeScale == 1_000_000_000
        && reducedDuration?.duration.value == 5_000_000
        && reducedDuration?.duration.timescale == 1_000_000_000
        && nonWrittenSampleCount(sourceSamples: 5, writtenFrames: 2) == 3
    if passed {
        print("camera recorder timing self-test: PASS")
        exit(0)
    }
    fputs("camera recorder timing self-test: FAIL\n", stderr)
    exit(1)
}

struct WriterPipeline {
    let writer: AVAssetWriter
    let input: AVAssetWriterInput
}

func makeWriterPipeline(
    outputURL: URL,
    width: Int32,
    height: Int32,
    frameRate: Int32,
    mediaTimeScale: CMTimeScale,
    sourceFormatHint: CMFormatDescription?,
    codec: AVVideoCodecType = .h264
) throws -> WriterPipeline {
    let writer = try AVAssetWriter(outputURL: outputURL, fileType: .mov)
    let compression: [String: Any] = [
        AVVideoAverageBitRateKey: 20_000_000,
        AVVideoExpectedSourceFrameRateKey: Int(frameRate),
        AVVideoMaxKeyFrameIntervalKey: Int(frameRate),
        AVVideoAllowFrameReorderingKey: false,
    ]
    var settings: [String: Any] = [
        AVVideoCodecKey: codec,
        AVVideoWidthKey: Int(width),
        AVVideoHeightKey: Int(height),
    ]
    if codec == .h264 {
        settings[AVVideoCompressionPropertiesKey] = compression
    }
    let input = AVAssetWriterInput(
        mediaType: .video,
        outputSettings: settings,
        sourceFormatHint: sourceFormatHint
    )
    input.expectsMediaDataInRealTime = true
    input.mediaTimeScale = mediaTimeScale
    guard writer.canAdd(input) else {
        throw NSError(domain: "v1simple.camera", code: 2)
    }
    writer.add(input)
    guard writer.startWriting() else {
        throw writer.error ?? NSError(domain: "v1simple.camera", code: 3)
    }
    writer.startSession(atSourceTime: .zero)
    return WriterPipeline(writer: writer, input: input)
}

func copySampleBuffer(
    _ sampleBuffer: CMSampleBuffer,
    with timing: ResolvedVideoTiming
) throws -> CMSampleBuffer {
    var sampleTiming = CMSampleTimingInfo(
        duration: timing.duration,
        presentationTimeStamp: timing.presentationTime,
        decodeTimeStamp: .invalid
    )
    var retimed: CMSampleBuffer?
    let status = CMSampleBufferCreateCopyWithNewTiming(
        allocator: kCFAllocatorDefault,
        sampleBuffer: sampleBuffer,
        sampleTimingEntryCount: 1,
        sampleTimingArray: &sampleTiming,
        sampleBufferOut: &retimed
    )
    guard status == noErr, let retimed else {
        throw NSError(domain: "v1simple.camera.timing", code: Int(status))
    }
    return retimed
}

func runWriterSelfTest() -> Never {
    // Use the software ProRes encoder at a modest profile so CI proves the
    // retimed-sample/timestamp/finalization path without requiring H.264 hardware.
    let width: Int32 = 320
    let height: Int32 = 240
    let frameRate: Int32 = 30
    let outputURL = FileManager.default.temporaryDirectory.appendingPathComponent(
        "v1simple-camera-writer-\(UUID().uuidString).mov"
    )
    defer { try? FileManager.default.removeItem(at: outputURL) }

    do {
        var pixelBuffer: CVPixelBuffer?
        let createStatus = CVPixelBufferCreate(
            kCFAllocatorDefault,
            Int(width),
            Int(height),
            kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
            nil,
            &pixelBuffer
        )
        guard createStatus == kCVReturnSuccess, let pixelBuffer else {
            throw NSError(domain: "v1simple.camera.selftest", code: Int(createStatus))
        }
        CVPixelBufferLockBaseAddress(pixelBuffer, [])
        defer { CVPixelBufferUnlockBaseAddress(pixelBuffer, []) }
        for plane in 0 ..< CVPixelBufferGetPlaneCount(pixelBuffer) {
            guard let baseAddress = CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, plane) else {
                throw NSError(domain: "v1simple.camera.selftest", code: 10 + plane)
            }
            let fill: Int32 = plane == 0 ? 16 : 128
            memset(
                baseAddress,
                fill,
                CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, plane)
                    * CVPixelBufferGetHeightOfPlane(pixelBuffer, plane)
            )
        }

        var formatDescription: CMVideoFormatDescription?
        let descriptionStatus = CMVideoFormatDescriptionCreateForImageBuffer(
            allocator: kCFAllocatorDefault,
            imageBuffer: pixelBuffer,
            formatDescriptionOut: &formatDescription
        )
        guard descriptionStatus == noErr, let formatDescription else {
            throw NSError(domain: "v1simple.camera.selftest", code: Int(descriptionStatus))
        }
        let sourceValues: [Int64] = [
            30_000_000_000, 30_005_001_000, 30_010_002_777, 30_015_004_123,
            30_020_006_000, 30_025_007_333, 30_030_009_100, 30_035_010_222,
            30_040_012_555, 30_045_014_001, 30_050_015_999, 30_055_018_250,
        ]
        let sourceDurations: [CMTime] = sourceValues.map { _ in
            CMTime(value: 5_000_000, timescale: 1_000_000_000)
        }
        var timeline = SourcePresentationTimeline()
        let resolvedTimings = try zip(sourceValues, sourceDurations).map { value, sourceDuration in
            try timeline.resolve(
                sourcePresentationTime: CMTime(value: value, timescale: 1_000_000_000),
                sourceDuration: sourceDuration
            )
        }
        guard let mediaTimeScale = resolvedTimings.first?.writerTimeScale else {
            throw NSError(domain: "v1simple.camera.selftest", code: 19)
        }
        let pipeline = try makeWriterPipeline(
            outputURL: outputURL,
            width: width,
            height: height,
            frameRate: frameRate,
            mediaTimeScale: mediaTimeScale,
            sourceFormatHint: formatDescription,
            codec: .proRes422LT
        )
        for (frameIndex, resolved) in resolvedTimings.enumerated() {
            let deadline = Date().addingTimeInterval(5)
            while !pipeline.input.isReadyForMoreMediaData {
                guard pipeline.writer.status == .writing, Date() < deadline else {
                    throw pipeline.writer.error
                        ?? NSError(domain: "v1simple.camera.selftest", code: 20)
                }
                Thread.sleep(forTimeInterval: 0.005)
            }
            let sourcePresentationTime = CMTime(
                value: sourceValues[frameIndex],
                timescale: 1_000_000_000
            )
            let sourceDuration = sourceDurations[frameIndex]
            var sourceTiming = CMSampleTimingInfo(
                duration: sourceDuration,
                presentationTimeStamp: sourcePresentationTime,
                decodeTimeStamp: .invalid
            )
            var sourceSampleBuffer: CMSampleBuffer?
            let sampleStatus = CMSampleBufferCreateReadyWithImageBuffer(
                allocator: kCFAllocatorDefault,
                imageBuffer: pixelBuffer,
                formatDescription: formatDescription,
                sampleTiming: &sourceTiming,
                sampleBufferOut: &sourceSampleBuffer
            )
            guard sampleStatus == noErr, let sourceSampleBuffer else {
                throw NSError(domain: "v1simple.camera.selftest", code: Int(sampleStatus))
            }
            let retimed = try copySampleBuffer(sourceSampleBuffer, with: resolved)
            guard pipeline.input.append(retimed) else {
                throw pipeline.writer.error
                    ?? NSError(domain: "v1simple.camera.selftest", code: 21)
            }
        }
        pipeline.input.markAsFinished()
        let completed = DispatchSemaphore(value: 0)
        pipeline.writer.finishWriting { completed.signal() }
        guard completed.wait(timeout: .now() + 10) == .success,
              pipeline.writer.status == .completed else {
            throw pipeline.writer.error
                ?? NSError(domain: "v1simple.camera.selftest", code: 22)
        }
        let movie = try Data(contentsOf: outputURL)
        guard !movie.isEmpty, movie.range(of: Data("moov".utf8)) != nil else {
            throw NSError(domain: "v1simple.camera.selftest", code: 23)
        }

        let asset = AVURLAsset(url: outputURL)
        let reader = try AVAssetReader(asset: asset)
        guard let track = asset.tracks(withMediaType: .video).first else {
            throw NSError(domain: "v1simple.camera.selftest", code: 24)
        }
        let output = AVAssetReaderTrackOutput(track: track, outputSettings: nil)
        guard reader.canAdd(output) else {
            throw NSError(domain: "v1simple.camera.selftest", code: 25)
        }
        reader.add(output)
        guard reader.startReading() else {
            throw reader.error ?? NSError(domain: "v1simple.camera.selftest", code: 26)
        }
        var encodedTimings: [(CMTime, CMTime)] = []
        while let sample = output.copyNextSampleBuffer() {
            let presentationTime = CMSampleBufferGetPresentationTimeStamp(sample)
            let duration = CMSampleBufferGetDuration(sample)
            if CMSampleBufferGetNumSamples(sample) > 0,
               presentationTime.isValid,
               presentationTime.isNumeric,
               duration.isValid,
               duration.isNumeric,
               CMTimeCompare(duration, .zero) > 0 {
                encodedTimings.append((presentationTime, duration))
            }
        }
        guard reader.status == .completed else {
            throw reader.error ?? NSError(domain: "v1simple.camera.selftest", code: 27)
        }
        guard encodedTimings.count == resolvedTimings.count else {
            throw NSError(domain: "v1simple.camera.selftest", code: 28)
        }
        guard zip(encodedTimings, resolvedTimings).allSatisfy({ encoded, expected in
            CMTimeCompare(encoded.0, expected.presentationTime) == 0
        }) else {
            throw NSError(domain: "v1simple.camera.selftest", code: 29)
        }
        let encodedDurations = resolvedTimings.indices.map { index in
            index + 1 < resolvedTimings.count
                ? CMTimeSubtract(
                    resolvedTimings[index + 1].presentationTime,
                    resolvedTimings[index].presentationTime
                )
                : resolvedTimings[index].duration
        }
        guard zip(encodedTimings, encodedDurations).allSatisfy({ encoded, duration in
            CMTimeCompare(encoded.1, duration) == 0
        }) else {
            throw NSError(domain: "v1simple.camera.selftest", code: 30)
        }
        print("camera recorder writer self-test: PASS")
        exit(0)
    } catch {
        let nsError = error as NSError
        fputs("camera recorder writer self-test: FAIL \(nsError.domain) \(nsError.code)\n", stderr)
        exit(1)
    }
}

final class FrameRecorder: NSObject, AVCaptureVideoDataOutputSampleBufferDelegate {
    let queue = DispatchQueue(label: "v1simple.camera.frames")

    private struct PreparedSample {
        var record: [String: Any]
        let timing: ResolvedVideoTiming
        let hostCaptureNanoseconds: Int64
        let callbackHostNanoseconds: Int64?
        let sourcePresentationTime: CMTime
        let sourceDuration: CMTime
        let frameSequence: UInt64
    }

    private let width: Int32
    private let height: Int32
    private let frameRate: Int32
    private let hostClock = CMClockGetHostTimeClock()
    private var synchronizationClock: CMClock?
    private var outputURL: URL?
    private var readyMarker: URL?
    private var firstFrameMarker: URL?
    private var failureMarker: URL?
    private var statsMarker: URL?
    private var timingSidecarHandle: FileHandle?
    private var phase = ""
    private var writer: AVAssetWriter?
    private var writerInput: AVAssetWriterInput?
    private var errorMessage: String?
    private var frameCount = 0
    private var writerDropCount = 0
    private var captureDropCount = 0
    private var timestampErrorCount = 0
    private var phaseSampleCount = 0
    private var sourceSequence: UInt64 = 0
    private var lastCallbackHostTime: CMTime?
    private var maxCallbackGapMilliseconds = 0.0
    private var timeline = SourcePresentationTimeline()

    init(width: Int32, height: Int32, frameRate: Int32) {
        self.width = width
        self.height = height
        self.frameRate = frameRate
    }

    func setSynchronizationClock(_ clock: CMClock) {
        queue.sync {
            synchronizationClock = clock
        }
    }

    func startRecording(
        to outputURL: URL,
        timingSidecar: URL,
        readyMarker: URL,
        firstFrameMarker: URL?,
        failureMarker: URL,
        statsMarker: URL,
        phase: String
    ) {
        queue.sync {
            try? FileManager.default.removeItem(at: failureMarker)
            try? FileManager.default.removeItem(at: statsMarker)
            self.outputURL = outputURL
            self.readyMarker = readyMarker
            self.firstFrameMarker = firstFrameMarker
            self.failureMarker = failureMarker
            self.statsMarker = statsMarker
            self.phase = phase
            self.writer = nil
            self.writerInput = nil
            self.timingSidecarHandle = nil
            self.errorMessage = nil
            self.frameCount = 0
            self.writerDropCount = 0
            self.captureDropCount = 0
            self.timestampErrorCount = 0
            self.phaseSampleCount = 0
            self.lastCallbackHostTime = nil
            self.maxCallbackGapMilliseconds = 0
            self.timeline = SourcePresentationTimeline()
            do {
                self.timingSidecarHandle = try self.openExclusiveSidecar(timingSidecar)
            } catch {
                self.recordFailure(
                    code: "timing_sidecar_open_failed",
                    message: "camera timing sidecar could not be created",
                    error: error
                )
            }
        }
    }

    func healthProblem() -> String? {
        queue.sync { errorMessage }
    }

    func captureOutput(
        _ output: AVCaptureOutput,
        didOutput sampleBuffer: CMSampleBuffer,
        from connection: AVCaptureConnection
    ) {
        guard outputURL != nil else { return }
        let callbackTime = CMClockGetTime(hostClock)
        observeCallback(at: callbackTime)
        let frameSequence = nextSourceSequence()
        guard var prepared = prepareSample(
            sampleBuffer,
            callbackTime: callbackTime,
            frameSequence: frameSequence
        ) else { return }
        guard CMSampleBufferDataIsReady(sampleBuffer) else {
            recordWriterDrop(
                &prepared,
                reason: "sample_data_not_ready",
                fatalCode: nil,
                fatalMessage: nil
            )
            return
        }
        if writer == nil {
            do {
                try beginWriter(with: sampleBuffer, timing: prepared.timing)
            } catch {
                recordWriterDrop(
                    &prepared,
                    reason: "writer_start_failed",
                    fatalCode: "writer_start_failed",
                    fatalMessage: "movie writer could not start",
                    error: error
                )
                return
            }
        }
        guard let writer, let writerInput else {
            recordWriterDrop(
                &prepared,
                reason: "writer_unavailable",
                fatalCode: "writer_start_failed",
                fatalMessage: "movie writer was unavailable after startup"
            )
            return
        }
        switch writer.status {
        case .writing:
            break
        case .failed:
            recordWriterDrop(
                &prepared,
                reason: "writer_failed",
                fatalCode: "writer_failed",
                fatalMessage: "movie writer failed during capture",
                error: writer.error
            )
            return
        case .cancelled:
            recordWriterDrop(
                &prepared,
                reason: "writer_cancelled",
                fatalCode: "writer_cancelled",
                fatalMessage: "movie writer was cancelled during capture",
                error: writer.error
            )
            return
        case .completed, .unknown:
            recordWriterDrop(
                &prepared,
                reason: "writer_left_writing_state",
                fatalCode: "writer_left_writing_state",
                fatalMessage: "movie writer left the writing state during capture",
                error: writer.error
            )
            return
        @unknown default:
            recordWriterDrop(
                &prepared,
                reason: "writer_unknown_state",
                fatalCode: "writer_unknown_state",
                fatalMessage: "movie writer entered an unknown state during capture",
                error: writer.error
            )
            return
        }
        guard writerInput.isReadyForMoreMediaData else {
            recordWriterDrop(
                &prepared,
                reason: "writer_backpressure",
                fatalCode: nil,
                fatalMessage: nil
            )
            return
        }

        let retimed: CMSampleBuffer
        do {
            retimed = try copySampleBuffer(sampleBuffer, with: prepared.timing)
        } catch {
            timestampErrorCount += 1
            prepared.record["status"] = "timestamp_error"
            prepared.record["timestamp_error"] = SampleTimingError.sampleRetimingFailed.rawValue
            _ = appendTimingRecord(prepared.record)
            recordFailure(
                code: "sample_retiming_failed",
                message: "camera sample timing could not be copied",
                error: error
            )
            return
        }

        guard writerInput.append(retimed) else {
            recordWriterDrop(
                &prepared,
                reason: "frame_append_failed",
                fatalCode: "frame_append_failed",
                fatalMessage: "movie frame append failed",
                error: writer.error
            )
            return
        }

        frameCount += 1
        prepared.record["status"] = "written"
        guard appendTimingRecord(prepared.record) else { return }
        if frameCount == 1, let readyMarker {
            if let firstFrameMarker {
                do {
                    try writeMarker(firstFrameMarker, [
                        "schema_version": 2,
                        "event": "first_frame",
                        "source_clock": "avcapture_session_synchronization_clock",
                        "frame_seq": prepared.frameSequence,
                        "source_pts_value": prepared.sourcePresentationTime.value,
                        "source_pts_timescale": prepared.sourcePresentationTime.timescale,
                        "source_duration_value": prepared.sourceDuration.value,
                        "source_duration_timescale": prepared.sourceDuration.timescale,
                        "host_monotonic_ns": prepared.hostCaptureNanoseconds,
                        "host_capture_ns": prepared.hostCaptureNanoseconds,
                        "callback_host_ns": prepared.callbackHostNanoseconds ?? NSNull(),
                        "video_pts_value": prepared.timing.presentationTime.value,
                        "video_pts_timescale": prepared.timing.presentationTime.timescale,
                        "pts_zero_seconds": CMTimeGetSeconds(prepared.timing.presentationTime),
                    ])
                } catch {
                    recordFailure(
                        code: "first_frame_marker_failed",
                        message: "first appended frame timing marker could not be written",
                        error: error
                    )
                    return
                }
            }
            do {
                try writeMarker(readyMarker, ["result": "READY"])
            } catch {
                recordFailure(
                    code: "ready_marker_failed",
                    message: "recording-ready marker could not be written",
                    error: error
                )
            }
        }
    }

    func captureOutput(
        _ output: AVCaptureOutput,
        didDrop sampleBuffer: CMSampleBuffer,
        from connection: AVCaptureConnection
    ) {
        guard outputURL != nil else { return }
        let callbackTime = CMClockGetTime(hostClock)
        observeCallback(at: callbackTime)
        let frameSequence = nextSourceSequence()
        captureDropCount += 1
        let reason = droppedFrameReason(sampleBuffer)
        let reasonInfo = droppedFrameReasonInfo(sampleBuffer)
        guard var prepared = prepareSample(
            sampleBuffer,
            callbackTime: callbackTime,
            frameSequence: frameSequence,
            timingFailureStatus: "capture_drop",
            dropReason: reason,
            dropReasonInfo: reasonInfo
        ) else { return }
        prepared.record["status"] = "capture_drop"
        _ = appendTimingRecord(prepared.record)
    }

    private func nextSourceSequence() -> UInt64 {
        sourceSequence &+= 1
        phaseSampleCount += 1
        return sourceSequence
    }

    private func observeCallback(at hostTime: CMTime) {
        if let previous = lastCallbackHostTime {
            let gap = CMTimeGetSeconds(CMTimeSubtract(hostTime, previous))
            if gap.isFinite, gap >= 0 {
                maxCallbackGapMilliseconds = max(maxCallbackGapMilliseconds, gap * 1_000)
            }
        }
        lastCallbackHostTime = hostTime
    }

    private func prepareSample(
        _ sampleBuffer: CMSampleBuffer,
        callbackTime: CMTime,
        frameSequence: UInt64,
        timingFailureStatus: String = "timestamp_error",
        dropReason: String? = nil,
        dropReasonInfo: String? = nil
    ) -> PreparedSample? {
        let sourcePresentationTime = CMSampleBufferGetPresentationTimeStamp(sampleBuffer)
        let sourceDuration = CMSampleBufferGetDuration(sampleBuffer)
        let callbackHostNanoseconds = nanoseconds(callbackTime)
        var record: [String: Any] = [
            "schema_version": 1,
            "phase": phase,
            "frame_seq": frameSequence,
            "source_clock": "avcapture_session_synchronization_clock",
            "callback_clock": "host_monotonic",
            "source_pts_value": sourcePresentationTime.value,
            "source_pts_timescale": sourcePresentationTime.timescale,
            "source_pts_epoch": sourcePresentationTime.epoch,
            "source_pts_flags": sourcePresentationTime.flags.rawValue,
            "source_duration_value": sourceDuration.value,
            "source_duration_timescale": sourceDuration.timescale,
            "source_duration_epoch": sourceDuration.epoch,
            "source_duration_flags": sourceDuration.flags.rawValue,
            "callback_host_ns": callbackHostNanoseconds ?? NSNull(),
            "host_capture_ns": NSNull(),
            "video_pts_value": NSNull(),
            "video_pts_timescale": NSNull(),
            "video_duration_value": NSNull(),
            "video_duration_timescale": NSNull(),
            "duration_ns": nanoseconds(sourceDuration) ?? NSNull(),
            "drop_reason": dropReason ?? NSNull(),
            "drop_reason_info": dropReasonInfo ?? NSNull(),
        ]

        var timingErrors: [String] = []
        var hostCaptureNanoseconds: Int64?
        if let synchronizationClock {
            hostCaptureNanoseconds = hostNanoseconds(
                sourcePresentationTime,
                from: synchronizationClock,
                to: hostClock
            )
            if let hostCaptureNanoseconds, hostCaptureNanoseconds >= 0 {
                record["host_capture_ns"] = hostCaptureNanoseconds
            } else {
                hostCaptureNanoseconds = nil
                timingErrors.append(SampleTimingError.hostClockConversionFailed.rawValue)
            }
        } else {
            timingErrors.append(SampleTimingError.synchronizationClockUnavailable.rawValue)
        }

        var resolved: ResolvedVideoTiming?
        do {
            resolved = try timeline.resolve(
                sourcePresentationTime: sourcePresentationTime,
                sourceDuration: sourceDuration
            )
        } catch let error as SampleTimingError {
            timingErrors.append(error.rawValue)
        } catch {
            timingErrors.append("unknown_timing_error")
        }

        if let resolved {
            record["video_pts_value"] = resolved.presentationTime.value
            record["video_pts_timescale"] = resolved.presentationTime.timescale
            record["video_duration_value"] = resolved.duration.value
            record["video_duration_timescale"] = resolved.duration.timescale
        }
        guard timingErrors.isEmpty, let resolved, let hostCaptureNanoseconds else {
            timestampErrorCount += 1
            record["status"] = timingFailureStatus
            record["timestamp_error"] = timingErrors.first ?? "unknown_timing_error"
            record["timestamp_errors"] = timingErrors
            _ = appendTimingRecord(record)
            return nil
        }
        return PreparedSample(
            record: record,
            timing: resolved,
            hostCaptureNanoseconds: hostCaptureNanoseconds,
            callbackHostNanoseconds: callbackHostNanoseconds,
            sourcePresentationTime: sourcePresentationTime,
            sourceDuration: sourceDuration,
            frameSequence: frameSequence
        )
    }

    private func recordWriterDrop(
        _ prepared: inout PreparedSample,
        reason: String,
        fatalCode: String?,
        fatalMessage: String?,
        error: Error? = nil
    ) {
        writerDropCount += 1
        prepared.record["status"] = "writer_drop"
        prepared.record["drop_reason"] = reason
        _ = appendTimingRecord(prepared.record)
        if let fatalCode, let fatalMessage {
            recordFailure(code: fatalCode, message: fatalMessage, error: error)
        }
    }

    private func droppedFrameReason(_ sampleBuffer: CMSampleBuffer) -> String {
        guard let value = CMGetAttachment(
            sampleBuffer,
            key: kCMSampleBufferAttachmentKey_DroppedFrameReason,
            attachmentModeOut: nil
        ) else { return "unknown" }
        if CFEqual(value, kCMSampleBufferDroppedFrameReason_FrameWasLate) { return "frame_was_late" }
        if CFEqual(value, kCMSampleBufferDroppedFrameReason_OutOfBuffers) { return "out_of_buffers" }
        if CFEqual(value, kCMSampleBufferDroppedFrameReason_Discontinuity) { return "discontinuity" }
        return String(describing: value)
    }

    private func droppedFrameReasonInfo(_ sampleBuffer: CMSampleBuffer) -> String? {
        guard let value = CMGetAttachment(
            sampleBuffer,
            key: kCMSampleBufferAttachmentKey_DroppedFrameReasonInfo,
            attachmentModeOut: nil
        ) else { return nil }
        if CFEqual(value, kCMSampleBufferDroppedFrameReasonInfo_CameraModeSwitch) {
            return "camera_mode_switch"
        }
        return String(describing: value)
    }

    private func openExclusiveSidecar(_ url: URL) throws -> FileHandle {
        let descriptor = Darwin.open(
            url.path,
            O_WRONLY | O_CREAT | O_EXCL,
            S_IRUSR | S_IWUSR
        )
        guard descriptor >= 0 else {
            throw NSError(domain: NSPOSIXErrorDomain, code: Int(errno))
        }
        return FileHandle(fileDescriptor: descriptor, closeOnDealloc: true)
    }

    @discardableResult
    private func appendTimingRecord(_ payload: [String: Any]) -> Bool {
        guard let timingSidecarHandle else {
            recordFailure(
                code: "timing_sidecar_unavailable",
                message: "camera timing sidecar is unavailable"
            )
            return false
        }
        do {
            var data = try JSONSerialization.data(withJSONObject: payload, options: [.sortedKeys])
            data.append(0x0A)
            try timingSidecarHandle.write(contentsOf: data)
            return true
        } catch {
            recordFailure(
                code: "timing_sidecar_write_failed",
                message: "camera timing sidecar could not be written",
                error: error
            )
            return false
        }
    }

    private func beginWriter(
        with sampleBuffer: CMSampleBuffer,
        timing: ResolvedVideoTiming
    ) throws {
        guard let outputURL else {
            throw NSError(domain: "v1simple.camera", code: 1)
        }
        let pipeline = try makeWriterPipeline(
            outputURL: outputURL,
            width: width,
            height: height,
            frameRate: frameRate,
            mediaTimeScale: timing.writerTimeScale,
            sourceFormatHint: CMSampleBufferGetFormatDescription(sampleBuffer)
        )
        self.writer = pipeline.writer
        self.writerInput = pipeline.input
    }

    private func recordFailure(code: String, message: String, error: Error? = nil) {
        guard errorMessage == nil else { return }
        var rendered = "\(code): \(message)"
        if let error {
            let nsError = error as NSError
            rendered += " (\(nsError.domain) \(nsError.code))"
        }
        errorMessage = rendered

        guard let failureMarker else { return }
        var payload: [String: Any] = [
            "schema_version": 1,
            "result": "CAPTURE_FAILED",
            "code": code,
            "message": message,
            "phase": phase,
            "frames_appended": frameCount,
            "source_samples": phaseSampleCount,
            "last_source_sequence": sourceSequence,
            "writer_backpressure_drops": writerDropCount,
            "capture_drops": captureDropCount,
            "timestamp_errors": timestampErrorCount,
            "source_timing_anomalies": timestampErrorCount,
            "max_callback_gap_ms": round(maxCallbackGapMilliseconds * 1_000) / 1_000,
        ]
        let details = sanitizedError(error)
        if !details.isEmpty {
            payload["error"] = details
        }
        do {
            try writeMarker(failureMarker, payload)
        } catch {
            errorMessage = rendered + "; failure marker could not be written"
        }
    }

    private func writeStatsMarker() {
        guard let statsMarker else { return }
        let payload: [String: Any] = [
            "schema_version": 1,
            "result": errorMessage == nil ? "PASS" : "CAPTURE_FAILED",
            "phase": phase,
            "frames_appended": frameCount,
            "source_samples": phaseSampleCount,
            "last_source_sequence": sourceSequence,
            "writer_backpressure_drops": writerDropCount,
            "capture_drops": captureDropCount,
            "timestamp_errors": timestampErrorCount,
            "source_timing_anomalies": timestampErrorCount,
            "max_callback_gap_ms": round(maxCallbackGapMilliseconds * 1_000) / 1_000,
        ]
        do {
            try writeMarker(statsMarker, payload)
        } catch {
            recordFailure(
                code: "stats_marker_failed",
                message: "recorder statistics marker could not be written",
                error: error
            )
        }
    }

    private func completeStop(_ completed: DispatchSemaphore) {
        if let timingSidecarHandle {
            do {
                try timingSidecarHandle.synchronize()
                try timingSidecarHandle.close()
            } catch {
                recordFailure(
                    code: "timing_sidecar_finalize_failed",
                    message: "camera timing sidecar could not be finalized",
                    error: error
                )
            }
        }
        timingSidecarHandle = nil
        writeStatsMarker()
        writer = nil
        writerInput = nil
        completed.signal()
    }

    func stopRecording(timeout: TimeInterval) -> (frames: Int, dropped: Int, error: String?) {
        let completed = DispatchSemaphore(value: 0)
        queue.async {
            guard let writer = self.writer, let writerInput = self.writerInput else {
                if self.errorMessage == nil {
                    self.recordFailure(
                        code: "no_frames",
                        message: "movie recording received no frames"
                    )
                }
                self.outputURL = nil
                self.completeStop(completed)
                return
            }
            self.outputURL = nil
            guard writer.status == .writing else {
                if self.errorMessage == nil {
                    self.recordFailure(
                        code: "writer_failed",
                        message: "movie writer left the writing state",
                        error: writer.error
                    )
                }
                self.completeStop(completed)
                return
            }
            writerInput.markAsFinished()
            writer.finishWriting {
                self.queue.async {
                    if writer.status != .completed, self.errorMessage == nil {
                        self.recordFailure(
                            code: "writer_finalize_failed",
                            message: "movie recording failed to finalize",
                            error: writer.error
                        )
                    }
                    self.completeStop(completed)
                }
            }
        }
        if completed.wait(timeout: .now() + timeout) == .timedOut {
            return queue.sync {
                self.recordFailure(
                    code: "writer_finalize_timeout",
                    message: "movie recording did not finalize"
                )
                self.writeStatsMarker()
                return (
                    self.frameCount,
                    nonWrittenSampleCount(
                        sourceSamples: self.phaseSampleCount,
                        writtenFrames: self.frameCount
                    ),
                    self.errorMessage
                )
            }
        }
        return queue.sync {
            (
                frameCount,
                nonWrittenSampleCount(sourceSamples: phaseSampleCount, writtenFrames: frameCount),
                errorMessage
            )
        }
    }
}

if CommandLine.arguments.contains("--self-test-timing") {
    runTimingSelfTest()
}
if CommandLine.arguments.contains("--self-test-writer") {
    runWriterSelfTest()
}

let options = parseOptions()
let fileManager = FileManager.default
for url in [
    options.output,
    options.preflightOutput,
    options.timingSidecar,
    options.preflightTimingSidecar,
    options.sessionReady,
    options.startMarker,
    options.preflightReady,
    options.preflightStop,
    options.preflightFinished,
    options.recordingReady,
    options.firstFrameMarker,
    options.failureMarker,
    options.statsMarker,
] {
    try? fileManager.removeItem(at: url)
}

let discovery = AVCaptureDevice.DiscoverySession(
    deviceTypes: [.external],
    mediaType: .video,
    position: .unspecified
)
guard let device = discovery.devices.first(where: { $0.localizedName == options.deviceName }) else {
    fputs("camera not found: \(options.deviceName)\n", stderr)
    exit(3)
}
guard let format = device.formats.first(where: { candidate in
    let description = candidate.formatDescription
    let dimensions = CMVideoFormatDescriptionGetDimensions(description)
    return dimensions.width == options.width
        && dimensions.height == options.height
        && CMFormatDescriptionGetMediaSubType(description) == options.pixelFormat
        && candidate.videoSupportedFrameRateRanges.contains(where: { range in
            range.minFrameRate <= Double(options.frameRate)
                && range.maxFrameRate >= Double(options.frameRate)
        })
}) else {
    fputs(
        "camera format unavailable: \(options.width)x\(options.height) "
            + "\(options.pixelFormatName) @ \(options.frameRate)\n",
        stderr
    )
    exit(3)
}

let session = AVCaptureSession()
let input: AVCaptureDeviceInput
do {
    input = try AVCaptureDeviceInput(device: device)
} catch {
    fputs("camera input failed: \(error)\n", stderr)
    exit(3)
}
guard session.canAddInput(input) else {
    fputs("camera input was rejected\n", stderr)
    exit(3)
}
session.addInput(input)

let videoOutput = AVCaptureVideoDataOutput()
videoOutput.alwaysDiscardsLateVideoFrames = true
videoOutput.videoSettings = [kCVPixelBufferPixelFormatTypeKey as String: options.pixelFormat]
let recorder = FrameRecorder(width: options.width, height: options.height, frameRate: options.frameRate)
videoOutput.setSampleBufferDelegate(recorder, queue: recorder.queue)
guard session.canAddOutput(videoOutput) else {
    fputs("video output was rejected\n", stderr)
    exit(3)
}
session.addOutput(videoOutput)
session.startRunning()
guard let synchronizationClock = session.synchronizationClock else {
    session.stopRunning()
    fputs("camera synchronization clock is unavailable\n", stderr)
    exit(3)
}
recorder.setSynchronizationClock(synchronizationClock)

do {
    try device.lockForConfiguration()
    device.activeFormat = format
    let frameDuration = CMTime(value: 1, timescale: options.frameRate)
    device.activeVideoMinFrameDuration = frameDuration
    device.activeVideoMaxFrameDuration = frameDuration
    device.unlockForConfiguration()
} catch {
    session.stopRunning()
    fputs("camera format activation failed: \(error)\n", stderr)
    exit(3)
}

Thread.sleep(forTimeInterval: 0.5)
let activeDescription = device.activeFormat.formatDescription
let activeDimensions = CMVideoFormatDescriptionGetDimensions(activeDescription)
let activePixelFormat = CMFormatDescriptionGetMediaSubType(activeDescription)
let activeMinRate = 1.0 / device.activeVideoMinFrameDuration.seconds
let activeMaxRate = 1.0 / device.activeVideoMaxFrameDuration.seconds
guard activeDimensions.width == options.width,
      activeDimensions.height == options.height,
      activePixelFormat == options.pixelFormat,
      abs(activeMinRate - Double(options.frameRate)) < 0.5,
      abs(activeMaxRate - Double(options.frameRate)) < 0.5 else {
    session.stopRunning()
    fputs("camera did not retain the requested active format\n", stderr)
    exit(3)
}

do {
    try writeMarker(options.sessionReady, [
        "result": "READY",
        "width": Int(activeDimensions.width),
        "height": Int(activeDimensions.height),
        "framerate": options.frameRate,
        "pixel_format": options.pixelFormatName,
    ])
} catch {
    session.stopRunning()
    fputs("session-ready marker failed: \(error)\n", stderr)
    exit(3)
}

let stopRequested = DispatchSemaphore(value: 0)
signal(SIGINT, SIG_IGN)
signal(SIGTERM, SIG_IGN)
let interruptSource = DispatchSource.makeSignalSource(signal: SIGINT, queue: .global())
let terminateSource = DispatchSource.makeSignalSource(signal: SIGTERM, queue: .global())
interruptSource.setEventHandler { stopRequested.signal() }
terminateSource.setEventHandler { stopRequested.signal() }
interruptSource.resume()
terminateSource.resume()

while !fileManager.fileExists(atPath: options.startMarker.path) {
    if stopRequested.wait(timeout: .now() + .milliseconds(50)) == .success {
        session.stopRunning()
        exit(130)
    }
    RunLoop.current.run(mode: .default, before: Date().addingTimeInterval(0.01))
}

recorder.startRecording(
    to: options.preflightOutput,
    timingSidecar: options.preflightTimingSidecar,
    readyMarker: options.preflightReady,
    firstFrameMarker: nil,
    failureMarker: options.failureMarker,
    statsMarker: options.statsMarker,
    phase: "preflight"
)

while !fileManager.fileExists(atPath: options.preflightStop.path) {
    if recorder.healthProblem() != nil {
        break
    }
    if stopRequested.wait(timeout: .now() + .milliseconds(50)) == .success {
        _ = recorder.stopRecording(timeout: 15)
        session.stopRunning()
        exit(130)
    }
    RunLoop.current.run(mode: .default, before: Date().addingTimeInterval(0.01))
}
let preflightResult = recorder.stopRecording(timeout: options.preflightFinalizeTimeoutSeconds)
if let error = preflightResult.error {
    session.stopRunning()
    fputs("preflight recording failed: \(error)\n", stderr)
    exit(3)
}
do {
    try writeMarker(options.preflightFinished, [
        "result": "READY",
        "frames": preflightResult.frames,
        "dropped_frames": preflightResult.dropped,
    ])
} catch {
    session.stopRunning()
    fputs("preflight-finished marker failed: \(error)\n", stderr)
    exit(3)
}

recorder.startRecording(
    to: options.output,
    timingSidecar: options.timingSidecar,
    readyMarker: options.recordingReady,
    firstFrameMarker: options.firstFrameMarker,
    failureMarker: options.failureMarker,
    statsMarker: options.statsMarker,
    phase: "recording"
)

while stopRequested.wait(timeout: .now() + .milliseconds(50)) != .success {
    if recorder.healthProblem() != nil {
        break
    }
    RunLoop.current.run(mode: .default, before: Date().addingTimeInterval(0.01))
}
let recordingResult = recorder.stopRecording(timeout: options.finalizeTimeoutSeconds)
session.stopRunning()

if let error = recordingResult.error {
    fputs("movie recording failed: \(error)\n", stderr)
    exit(3)
}
guard fileManager.fileExists(atPath: options.output.path) else {
    fputs("movie output is missing\n", stderr)
    exit(3)
}

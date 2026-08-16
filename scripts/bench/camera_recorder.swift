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
    let sessionReady: URL
    let startMarker: URL
    let preflightReady: URL
    let preflightStop: URL
    let preflightFinished: URL
    let recordingReady: URL
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
        sessionReady: URL(fileURLWithPath: argument("--session-ready")),
        startMarker: URL(fileURLWithPath: argument("--start-marker")),
        preflightReady: URL(fileURLWithPath: argument("--preflight-ready")),
        preflightStop: URL(fileURLWithPath: argument("--preflight-stop")),
        preflightFinished: URL(fileURLWithPath: argument("--preflight-finished")),
        recordingReady: URL(fileURLWithPath: argument("--recording-ready")),
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

struct HostPresentationTimeline {
    private static let timeScale: CMTimeScale = 60_000

    private let nominalFrameDuration: CMTime
    private var startHostTime: CMTime?
    private var lastCallbackHostTime: CMTime?
    private(set) var lastPresentationTime: CMTime?
    private(set) var timingBumps = 0
    private(set) var maxCallbackGapMilliseconds = 0.0

    init(frameRate: Int32) {
        nominalFrameDuration = CMTime(value: 1, timescale: frameRate)
    }

    mutating func observeCallback(at hostTime: CMTime) {
        if let previousHostTime = lastCallbackHostTime {
            let gap = CMTimeGetSeconds(CMTimeSubtract(hostTime, previousHostTime))
            if gap.isFinite, gap >= 0 {
                maxCallbackGapMilliseconds = max(maxCallbackGapMilliseconds, gap * 1_000)
            }
        }
        lastCallbackHostTime = hostTime
    }

    mutating func presentationTime(for hostTime: CMTime) -> CMTime {
        guard let startHostTime else {
            self.startHostTime = hostTime
            lastPresentationTime = .zero
            return .zero
        }

        var candidate = CMTimeConvertScale(
            CMTimeSubtract(hostTime, startHostTime),
            timescale: Self.timeScale,
            method: .roundHalfAwayFromZero
        )
        if !candidate.isValid || !candidate.isNumeric || CMTimeCompare(candidate, .zero) < 0 {
            candidate = lastPresentationTime.map { CMTimeAdd($0, nominalFrameDuration) } ?? .zero
            timingBumps += 1
        } else if let lastPresentationTime, CMTimeCompare(candidate, lastPresentationTime) <= 0 {
            candidate = CMTimeAdd(lastPresentationTime, nominalFrameDuration)
            timingBumps += 1
        }
        lastPresentationTime = candidate
        return candidate
    }
}

func sanitizedError(_ error: Error?) -> [String: Any] {
    guard let error else { return [:] }
    let nsError = error as NSError
    var payload: [String: Any] = [
        "domain": nsError.domain,
        "code": nsError.code,
        "description": nsError.localizedDescription,
    ]
    if let underlying = nsError.userInfo[NSUnderlyingErrorKey] as? NSError {
        payload["underlying"] = [
            "domain": underlying.domain,
            "code": underlying.code,
            "description": underlying.localizedDescription,
        ]
    }
    return payload
}

func runTimingSelfTest() -> Never {
    var timeline = HostPresentationTimeline(frameRate: 200)
    let inputs = [0.0, 0.005, 0.004, 0.040].map {
        CMTime(seconds: 10.0 + $0, preferredTimescale: 60_000)
    }
    let outputs = inputs.map {
        timeline.observeCallback(at: $0)
        return CMTimeGetSeconds(timeline.presentationTime(for: $0))
    }
    let expected = [0.0, 0.005, 0.010, 0.040]
    let passed = zip(outputs, expected).allSatisfy { abs($0 - $1) < 0.000_001 }
        && timeline.timingBumps == 1
        && abs(timeline.maxCallbackGapMilliseconds - 36.0) < 0.001
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
    let adaptor: AVAssetWriterInputPixelBufferAdaptor
}

func makeWriterPipeline(
    outputURL: URL,
    width: Int32,
    height: Int32,
    frameRate: Int32,
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
    input.mediaTimeScale = 60_000
    guard writer.canAdd(input) else {
        throw NSError(domain: "v1simple.camera", code: 2)
    }
    writer.add(input)
    let adaptor = AVAssetWriterInputPixelBufferAdaptor(
        assetWriterInput: input,
        sourcePixelBufferAttributes: nil
    )
    guard writer.startWriting() else {
        throw writer.error ?? NSError(domain: "v1simple.camera", code: 3)
    }
    writer.startSession(atSourceTime: .zero)
    return WriterPipeline(writer: writer, input: input, adaptor: adaptor)
}

func runWriterSelfTest() -> Never {
    // Use the software ProRes encoder at a modest profile so CI proves the
    // adaptor/timestamp/finalization path without requiring H.264 hardware.
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
        let pipeline = try makeWriterPipeline(
            outputURL: outputURL,
            width: width,
            height: height,
            frameRate: frameRate,
            sourceFormatHint: formatDescription,
            codec: .proRes422LT
        )
        for frameIndex in 0 ..< 12 {
            let deadline = Date().addingTimeInterval(5)
            while !pipeline.input.isReadyForMoreMediaData {
                guard pipeline.writer.status == .writing, Date() < deadline else {
                    throw pipeline.writer.error
                        ?? NSError(domain: "v1simple.camera.selftest", code: 20)
                }
                Thread.sleep(forTimeInterval: 0.005)
            }
            let presentationTime = CMTime(value: Int64(frameIndex), timescale: frameRate)
            guard pipeline.adaptor.append(
                pixelBuffer,
                withPresentationTime: presentationTime
            ) else {
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
        print("camera recorder writer self-test: PASS")
        exit(0)
    } catch {
        fputs("camera recorder writer self-test: FAIL \(error)\n", stderr)
        exit(1)
    }
}

final class FrameRecorder: NSObject, AVCaptureVideoDataOutputSampleBufferDelegate {
    let queue = DispatchQueue(label: "v1simple.camera.frames")

    private let width: Int32
    private let height: Int32
    private let frameRate: Int32
    private var outputURL: URL?
    private var readyMarker: URL?
    private var failureMarker: URL?
    private var statsMarker: URL?
    private var phase = ""
    private var writer: AVAssetWriter?
    private var writerInput: AVAssetWriterInput?
    private var pixelBufferAdaptor: AVAssetWriterInputPixelBufferAdaptor?
    private var errorMessage: String?
    private var frameCount = 0
    private var droppedFrameCount = 0
    private var sourceTimingAnomalyCount = 0
    private var lastSourcePresentationTime: CMTime?
    private var timeline: HostPresentationTimeline

    init(width: Int32, height: Int32, frameRate: Int32) {
        self.width = width
        self.height = height
        self.frameRate = frameRate
        self.timeline = HostPresentationTimeline(frameRate: frameRate)
    }

    func startRecording(
        to outputURL: URL,
        readyMarker: URL,
        failureMarker: URL,
        statsMarker: URL,
        phase: String
    ) {
        queue.sync {
            try? FileManager.default.removeItem(at: failureMarker)
            try? FileManager.default.removeItem(at: statsMarker)
            self.outputURL = outputURL
            self.readyMarker = readyMarker
            self.failureMarker = failureMarker
            self.statsMarker = statsMarker
            self.phase = phase
            self.writer = nil
            self.writerInput = nil
            self.pixelBufferAdaptor = nil
            self.errorMessage = nil
            self.frameCount = 0
            self.droppedFrameCount = 0
            self.sourceTimingAnomalyCount = 0
            self.lastSourcePresentationTime = nil
            self.timeline = HostPresentationTimeline(frameRate: self.frameRate)
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
        guard outputURL != nil, errorMessage == nil, CMSampleBufferDataIsReady(sampleBuffer) else {
            return
        }
        let hostTime = CMClockGetTime(CMClockGetHostTimeClock())
        timeline.observeCallback(at: hostTime)
        let sourcePresentationTime = CMSampleBufferGetPresentationTimeStamp(sampleBuffer)
        let sourceTimingValid = sourcePresentationTime.isValid && sourcePresentationTime.isNumeric
        let sourceTimingAdvanced = sourceTimingValid
            && (lastSourcePresentationTime.map {
                CMTimeCompare(sourcePresentationTime, $0) > 0
            } ?? true)
        if !sourceTimingAdvanced {
            sourceTimingAnomalyCount += 1
        } else {
            lastSourcePresentationTime = sourcePresentationTime
        }
        if writer == nil {
            do {
                try beginWriter(with: sampleBuffer)
            } catch {
                recordFailure(
                    code: "writer_start_failed",
                    message: "movie writer could not start",
                    error: error
                )
                return
            }
        }
        guard let writer, let writerInput, let pixelBufferAdaptor else { return }
        guard let pixelBuffer = CMSampleBufferGetImageBuffer(sampleBuffer) else {
            recordFailure(
                code: "pixel_buffer_missing",
                message: "camera sample did not contain an image buffer"
            )
            return
        }
        switch writer.status {
        case .writing:
            break
        case .failed:
            recordFailure(
                code: "writer_failed",
                message: "movie writer failed during capture",
                error: writer.error
            )
            return
        case .cancelled:
            recordFailure(
                code: "writer_cancelled",
                message: "movie writer was cancelled during capture",
                error: writer.error
            )
            return
        case .completed, .unknown:
            recordFailure(
                code: "writer_left_writing_state",
                message: "movie writer left the writing state during capture",
                error: writer.error
            )
            return
        @unknown default:
            recordFailure(
                code: "writer_unknown_state",
                message: "movie writer entered an unknown state during capture",
                error: writer.error
            )
            return
        }
        if writerInput.isReadyForMoreMediaData {
            let presentationTime = timeline.presentationTime(for: hostTime)
            if pixelBufferAdaptor.append(pixelBuffer, withPresentationTime: presentationTime) {
                frameCount += 1
                if frameCount == 1, let readyMarker {
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
            } else {
                recordFailure(
                    code: "frame_append_failed",
                    message: "movie frame append failed",
                    error: writer.error
                )
            }
        } else {
            droppedFrameCount += 1
        }
    }

    private func beginWriter(with sampleBuffer: CMSampleBuffer) throws {
        guard let outputURL else {
            throw NSError(domain: "v1simple.camera", code: 1)
        }
        let pipeline = try makeWriterPipeline(
            outputURL: outputURL,
            width: width,
            height: height,
            frameRate: frameRate,
            sourceFormatHint: CMSampleBufferGetFormatDescription(sampleBuffer)
        )
        self.writer = pipeline.writer
        self.writerInput = pipeline.input
        self.pixelBufferAdaptor = pipeline.adaptor
    }

    private func recordFailure(code: String, message: String, error: Error? = nil) {
        guard errorMessage == nil else { return }
        var rendered = "\(code): \(message)"
        if let error {
            let nsError = error as NSError
            rendered += " (\(nsError.domain) \(nsError.code): \(nsError.localizedDescription))"
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
            "writer_backpressure_drops": droppedFrameCount,
            "source_timing_anomalies": sourceTimingAnomalyCount,
            "timing_bumps": timeline.timingBumps,
            "max_callback_gap_ms": round(timeline.maxCallbackGapMilliseconds * 1_000) / 1_000,
        ]
        let details = sanitizedError(error)
        if !details.isEmpty {
            payload["error"] = details
        }
        do {
            try writeMarker(failureMarker, payload)
        } catch {
            errorMessage = rendered + "; failure marker could not be written: \(error.localizedDescription)"
        }
    }

    private func writeStatsMarker() {
        guard let statsMarker else { return }
        let payload: [String: Any] = [
            "schema_version": 1,
            "result": errorMessage == nil ? "PASS" : "CAPTURE_FAILED",
            "phase": phase,
            "frames_appended": frameCount,
            "writer_backpressure_drops": droppedFrameCount,
            "source_timing_anomalies": sourceTimingAnomalyCount,
            "timing_bumps": timeline.timingBumps,
            "max_callback_gap_ms": round(timeline.maxCallbackGapMilliseconds * 1_000) / 1_000,
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
        writeStatsMarker()
        writer = nil
        writerInput = nil
        pixelBufferAdaptor = nil
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
                return (self.frameCount, self.droppedFrameCount, self.errorMessage)
            }
        }
        return queue.sync { (frameCount, droppedFrameCount, errorMessage) }
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
    options.sessionReady,
    options.startMarker,
    options.preflightReady,
    options.preflightStop,
    options.preflightFinished,
    options.recordingReady,
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
    readyMarker: options.preflightReady,
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
    readyMarker: options.recordingReady,
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

#!/usr/bin/env swift

import AVFoundation
import CoreMedia
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
        recordingReady: URL(fileURLWithPath: argument("--recording-ready"))
    )
}

func writeMarker(_ url: URL, _ payload: [String: Any]) throws {
    let data = try JSONSerialization.data(withJSONObject: payload, options: [.prettyPrinted, .sortedKeys])
    try data.write(to: url, options: .atomic)
}

final class FrameRecorder: NSObject, AVCaptureVideoDataOutputSampleBufferDelegate {
    let queue = DispatchQueue(label: "v1simple.camera.frames")

    private let width: Int32
    private let height: Int32
    private let frameRate: Int32
    private var outputURL: URL?
    private var readyMarker: URL?
    private var writer: AVAssetWriter?
    private var writerInput: AVAssetWriterInput?
    private var errorMessage: String?
    private var frameCount = 0
    private var droppedFrameCount = 0

    init(width: Int32, height: Int32, frameRate: Int32) {
        self.width = width
        self.height = height
        self.frameRate = frameRate
    }

    func startRecording(to outputURL: URL, readyMarker: URL) {
        queue.sync {
            self.outputURL = outputURL
            self.readyMarker = readyMarker
            self.writer = nil
            self.writerInput = nil
            self.errorMessage = nil
            self.frameCount = 0
            self.droppedFrameCount = 0
        }
    }

    func captureOutput(
        _ output: AVCaptureOutput,
        didOutput sampleBuffer: CMSampleBuffer,
        from connection: AVCaptureConnection
    ) {
        guard outputURL != nil, errorMessage == nil, CMSampleBufferDataIsReady(sampleBuffer) else {
            return
        }
        if writer == nil {
            do {
                try beginWriter(with: sampleBuffer)
            } catch {
                errorMessage = "movie writer could not start: \(error)"
                return
            }
        }
        guard let writerInput else { return }
        if writerInput.isReadyForMoreMediaData {
            if writerInput.append(sampleBuffer) {
                frameCount += 1
                if frameCount == 1, let readyMarker {
                    do {
                        try writeMarker(readyMarker, ["result": "READY"])
                    } catch {
                        errorMessage = "recording-ready marker failed: \(error)"
                    }
                }
            } else {
                errorMessage = "movie frame append failed: \(writer?.error?.localizedDescription ?? "unknown error")"
            }
        } else {
            droppedFrameCount += 1
        }
    }

    private func beginWriter(with sampleBuffer: CMSampleBuffer) throws {
        guard let outputURL else {
            throw NSError(domain: "v1simple.camera", code: 1)
        }
        let writer = try AVAssetWriter(outputURL: outputURL, fileType: .mov)
        let compression: [String: Any] = [
            AVVideoAverageBitRateKey: 20_000_000,
            AVVideoExpectedSourceFrameRateKey: Int(frameRate),
            AVVideoMaxKeyFrameIntervalKey: Int(frameRate),
            AVVideoAllowFrameReorderingKey: false,
        ]
        let settings: [String: Any] = [
            AVVideoCodecKey: AVVideoCodecType.h264,
            AVVideoWidthKey: Int(width),
            AVVideoHeightKey: Int(height),
            AVVideoCompressionPropertiesKey: compression,
        ]
        let input = AVAssetWriterInput(
            mediaType: .video,
            outputSettings: settings,
            sourceFormatHint: CMSampleBufferGetFormatDescription(sampleBuffer)
        )
        input.expectsMediaDataInRealTime = true
        guard writer.canAdd(input) else {
            throw NSError(domain: "v1simple.camera", code: 2)
        }
        writer.add(input)
        guard writer.startWriting() else {
            throw writer.error ?? NSError(domain: "v1simple.camera", code: 3)
        }
        writer.startSession(atSourceTime: CMSampleBufferGetPresentationTimeStamp(sampleBuffer))
        self.writer = writer
        self.writerInput = input
    }

    func stopRecording(timeout: TimeInterval) -> (frames: Int, dropped: Int, error: String?) {
        let completed = DispatchSemaphore(value: 0)
        queue.async {
            guard let writer = self.writer, let writerInput = self.writerInput else {
                if self.errorMessage == nil {
                    self.errorMessage = "movie recording received no frames"
                }
                self.outputURL = nil
                completed.signal()
                return
            }
            self.outputURL = nil
            writerInput.markAsFinished()
            writer.finishWriting {
                self.queue.async {
                    if writer.status != .completed, self.errorMessage == nil {
                        self.errorMessage = "movie recording failed: \(writer.error?.localizedDescription ?? "unknown error")"
                    }
                    self.writer = nil
                    self.writerInput = nil
                    completed.signal()
                }
            }
        }
        if completed.wait(timeout: .now() + timeout) == .timedOut {
            return (0, 0, "movie recording did not finalize")
        }
        return queue.sync { (frameCount, droppedFrameCount, errorMessage) }
    }
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
videoOutput.alwaysDiscardsLateVideoFrames = false
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

recorder.startRecording(to: options.preflightOutput, readyMarker: options.preflightReady)

while !fileManager.fileExists(atPath: options.preflightStop.path) {
    if stopRequested.wait(timeout: .now() + .milliseconds(50)) == .success {
        _ = recorder.stopRecording(timeout: 15)
        session.stopRunning()
        exit(130)
    }
    RunLoop.current.run(mode: .default, before: Date().addingTimeInterval(0.01))
}
let preflightResult = recorder.stopRecording(timeout: 15)
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

recorder.startRecording(to: options.output, readyMarker: options.recordingReady)

while stopRequested.wait(timeout: .now() + .milliseconds(50)) != .success {
    RunLoop.current.run(mode: .default, before: Date().addingTimeInterval(0.01))
}
let recordingResult = recorder.stopRecording(timeout: 30)
session.stopRunning()

if let error = recordingResult.error {
    fputs("movie recording failed: \(error)\n", stderr)
    exit(3)
}
guard fileManager.fileExists(atPath: options.output.path) else {
    fputs("movie output is missing\n", stderr)
    exit(3)
}

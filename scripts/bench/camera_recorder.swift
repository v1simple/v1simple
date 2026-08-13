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

final class RecordingDelegate: NSObject, AVCaptureFileOutputRecordingDelegate {
    private let lock = NSLock()
    private var startedValue = false
    private var finishedValue = false
    private var finishErrorValue: Error?
    let recordingReady: URL

    init(recordingReady: URL) {
        self.recordingReady = recordingReady
    }

    var started: Bool {
        lock.lock()
        defer { lock.unlock() }
        return startedValue
    }

    var finished: Bool {
        lock.lock()
        defer { lock.unlock() }
        return finishedValue
    }

    var finishError: Error? {
        lock.lock()
        defer { lock.unlock() }
        return finishErrorValue
    }

    func fileOutput(
        _ output: AVCaptureFileOutput,
        didStartRecordingTo fileURL: URL,
        from connections: [AVCaptureConnection]
    ) {
        do {
            try writeMarker(recordingReady, ["result": "READY"])
        } catch {
            lock.lock()
            finishErrorValue = error
            lock.unlock()
        }
        lock.lock()
        startedValue = true
        lock.unlock()
    }

    func fileOutput(
        _ output: AVCaptureFileOutput,
        didFinishRecordingTo outputFileURL: URL,
        from connections: [AVCaptureConnection],
        error: Error?
    ) {
        lock.lock()
        finishErrorValue = finishErrorValue ?? error
        finishedValue = true
        lock.unlock()
    }
}

@discardableResult
func waitUntil(timeout: TimeInterval, condition: () -> Bool) -> Bool {
    let deadline = Date().addingTimeInterval(timeout)
    while Date() < deadline {
        if condition() { return true }
        RunLoop.current.run(mode: .default, before: Date().addingTimeInterval(0.01))
    }
    return condition()
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

let movieOutput = AVCaptureMovieFileOutput()
guard session.canAddOutput(movieOutput) else {
    fputs("movie output was rejected\n", stderr)
    exit(3)
}
session.addOutput(movieOutput)
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

let preflightDelegate = RecordingDelegate(recordingReady: options.preflightReady)
movieOutput.startRecording(to: options.preflightOutput, recordingDelegate: preflightDelegate)
guard waitUntil(timeout: 15, condition: { preflightDelegate.started }),
      preflightDelegate.finishError == nil else {
    if movieOutput.isRecording { movieOutput.stopRecording() }
    _ = waitUntil(timeout: 5, condition: { preflightDelegate.finished })
    session.stopRunning()
    fputs("preflight recording did not start\n", stderr)
    exit(3)
}

while !fileManager.fileExists(atPath: options.preflightStop.path) {
    if stopRequested.wait(timeout: .now()) == .success {
        movieOutput.stopRecording()
        _ = waitUntil(timeout: 5, condition: { preflightDelegate.finished })
        session.stopRunning()
        exit(130)
    }
    RunLoop.current.run(mode: .default, before: Date().addingTimeInterval(0.01))
}
if movieOutput.isRecording { movieOutput.stopRecording() }
guard waitUntil(timeout: 15, condition: { preflightDelegate.finished }) else {
    session.stopRunning()
    fputs("preflight recording did not finalize\n", stderr)
    exit(3)
}
if let error = preflightDelegate.finishError {
    session.stopRunning()
    fputs("preflight recording failed: \(error)\n", stderr)
    exit(3)
}
do {
    try writeMarker(options.preflightFinished, ["result": "READY"])
} catch {
    session.stopRunning()
    fputs("preflight-finished marker failed: \(error)\n", stderr)
    exit(3)
}

let delegate = RecordingDelegate(recordingReady: options.recordingReady)
movieOutput.startRecording(to: options.output, recordingDelegate: delegate)
guard waitUntil(timeout: 15, condition: { delegate.started }), delegate.finishError == nil else {
    if movieOutput.isRecording { movieOutput.stopRecording() }
    _ = waitUntil(timeout: 5, condition: { delegate.finished })
    session.stopRunning()
    fputs("movie recording did not start\n", stderr)
    exit(3)
}

while movieOutput.isRecording {
    if stopRequested.wait(timeout: .now()) == .success {
        break
    }
    RunLoop.current.run(mode: .default, before: Date().addingTimeInterval(0.01))
}
if movieOutput.isRecording {
    movieOutput.stopRecording()
}
guard waitUntil(timeout: 15, condition: { delegate.finished }) else {
    session.stopRunning()
    fputs("movie recording did not finalize\n", stderr)
    exit(3)
}
session.stopRunning()

if let error = delegate.finishError {
    fputs("movie recording failed: \(error)\n", stderr)
    exit(3)
}
guard fileManager.fileExists(atPath: options.output.path) else {
    fputs("movie output is missing\n", stderr)
    exit(3)
}

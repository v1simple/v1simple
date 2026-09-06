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
    let rawFrameOutputDirectory: URL?
    let rawFrameIndices: [Int]
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

enum RawFrameError: String, Error {
    case invalidSelection, invalidLayout, unsupportedPixelFormat, lockFailed
    case missingImageBuffer, nonemptyOutputDirectory, incompleteSelection, invalidMetadata
}

let maximumRawFrameSnapshots = 128

func parseRawFrameIndices(_ value: String) throws -> [Int] {
    let parts = value.split(separator: ",", omittingEmptySubsequences: false)
    guard (1...maximumRawFrameSnapshots).contains(parts.count) else { throw RawFrameError.invalidSelection }
    let indices = try parts.map { part -> Int in
        guard !part.isEmpty, part.utf8.allSatisfy({ (48...57).contains($0) }),
              let index = Int(part), index >= 0 else { throw RawFrameError.invalidSelection }
        return index
    }
    guard Set(indices).count == indices.count else { throw RawFrameError.invalidSelection }
    return indices.sorted()
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
    var rawDirectory: URL?
    var rawIndices: [Int] = []
    let hasRawDirectory = CommandLine.arguments.contains("--raw-frame-output-dir")
    let hasRawIndices = CommandLine.arguments.contains("--raw-frame-indices")
    if hasRawDirectory || hasRawIndices {
        guard hasRawDirectory && hasRawIndices else {
            fputs("raw camera frame diagnostics require both snapshot options\n", stderr)
            exit(2)
        }
        do {
            rawIndices = try parseRawFrameIndices(argument("--raw-frame-indices"))
        } catch {
            fputs("raw camera frame indices must be 1-128 unique nonnegative recording frame indices\n", stderr)
            exit(2)
        }
        rawDirectory = URL(fileURLWithPath: argument("--raw-frame-output-dir"), isDirectory: true)
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
        preflightFinalizeTimeoutSeconds: preflightFinalizeTimeoutSeconds,
        rawFrameOutputDirectory: rawDirectory,
        rawFrameIndices: rawIndices
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
    if let rawError = error as? RawFrameError {
        payload["reason"] = rawError.rawValue
    }
    if let underlying = nsError.userInfo[NSUnderlyingErrorKey] as? NSError {
        payload["underlying"] = [
            "domain": underlying.domain,
            "code": underlying.code,
        ]
    }
    return payload
}

// This diagnostic copies active source bytes without conversion.
// It never retains a camera buffer on the disk queue.
func copyActivePlane(_ base: UnsafeRawPointer, stride: Int, rowBytes: Int, rows: Int) throws -> Data {
    let (size, overflow) = rowBytes.multipliedReportingOverflow(by: rows)
    let (_, strideOverflow) = stride.multipliedReportingOverflow(by: rows)
    guard rowBytes > 0, rows > 0, stride >= rowBytes, !overflow, !strideOverflow else {
        throw RawFrameError.invalidLayout
    }
    var data = Data(count: size)
    data.withUnsafeMutableBytes { destination in
        for row in 0..<rows {
            destination.baseAddress!.advanced(by: row * rowBytes).copyMemory(
                from: base.advanced(by: row * stride), byteCount: rowBytes
            )
        }
    }
    return data
}

func rawFrameCopy(_ buffer: CVPixelBuffer, formatDescription: CMFormatDescription?) throws
    -> (bytes: Data, metadata: [String: Any], fileExtension: String) {
    let format = CVPixelBufferGetPixelFormatType(buffer)
    let isNV12: Bool, fourCC: String, range: String
    switch format {
    case kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange:
        (isNV12, fourCC, range) = (true, "420v", "video")
    case kCVPixelFormatType_420YpCbCr8BiPlanarFullRange:
        (isNV12, fourCC, range) = (true, "420f", "full")
    case kCVPixelFormatType_422YpCbCr8_yuvs:
        (isNV12, fourCC, range) = (false, "yuvs", "video")
    default:
        throw RawFrameError.unsupportedPixelFormat
    }
    let width = CVPixelBufferGetWidth(buffer), height = CVPixelBufferGetHeight(buffer)
    guard width > 0, height > 0, width % 2 == 0 else {
        throw RawFrameError.invalidLayout
    }
    if isNV12 {
        guard height % 2 == 0, CVPixelBufferGetPlaneCount(buffer) == 2,
              CVPixelBufferGetWidthOfPlane(buffer, 0) == width,
              CVPixelBufferGetHeightOfPlane(buffer, 0) == height,
              CVPixelBufferGetWidthOfPlane(buffer, 1) == width / 2,
              CVPixelBufferGetHeightOfPlane(buffer, 1) == height / 2 else {
            throw RawFrameError.invalidLayout
        }
    } else if CVPixelBufferIsPlanar(buffer) || CVPixelBufferGetPlaneCount(buffer) != 0 {
        throw RawFrameError.invalidLayout
    }
    guard CVPixelBufferLockBaseAddress(buffer, .readOnly) == kCVReturnSuccess else {
        throw RawFrameError.lockFailed
    }
    defer { CVPixelBufferUnlockBaseAddress(buffer, .readOnly) }
    var bytes = Data(), planes: [[String: Any]] = []
    let (packedRowBytes, overflow) = width.multipliedReportingOverflow(by: 2)
    guard !overflow else { throw RawFrameError.invalidLayout }
    for plane in 0..<(isNV12 ? 2 : 1) {
        guard let base = isNV12 ? CVPixelBufferGetBaseAddressOfPlane(buffer, plane)
                               : CVPixelBufferGetBaseAddress(buffer) else {
            throw RawFrameError.invalidLayout
        }
        let rows = isNV12 ? CVPixelBufferGetHeightOfPlane(buffer, plane) : height
        let stride = isNV12 ? CVPixelBufferGetBytesPerRowOfPlane(buffer, plane)
                           : CVPixelBufferGetBytesPerRow(buffer)
        let rowBytes = isNV12 ? width : packedRowBytes
        let active = try copyActivePlane(base, stride: stride, rowBytes: rowBytes, rows: rows)
        planes.append([
            "plane": plane, "components": isNV12 ? (plane == 0 ? "Y" : "CbCr") : "Y0_Cb_Y1_Cr",
            "width_samples": isNV12 ? CVPixelBufferGetWidthOfPlane(buffer, plane) : width,
            "height": rows, "source_bytes_per_row": stride, "stored_bytes_per_row": rowBytes,
            "offset_bytes": bytes.count, "size_bytes": active.count,
        ])
        bytes.append(active)
    }
    let colorKeys = [kCVImageBufferYCbCrMatrixKey, kCVImageBufferColorPrimariesKey,
                     kCVImageBufferTransferFunctionKey, kCVImageBufferChromaLocationTopFieldKey,
                     kCVImageBufferChromaLocationBottomFieldKey]
    func colors(_ values: [String: Any]) -> [String: Any] {
        var result: [String: Any] = [:]
        for key in colorKeys {
            result[key as String] = values[key as String] ?? NSNull()
        }
        return result
    }
    let propagated = CVBufferCopyAttachments(buffer, .shouldPropagate) as? [String: Any] ?? [:]
    let nonpropagated = CVBufferCopyAttachments(buffer, .shouldNotPropagate) as? [String: Any] ?? [:]
    let extensions = formatDescription.flatMap {
        CMFormatDescriptionGetExtensions($0) as? [String: Any]
    } ?? [:]
    var formatColors = colors(extensions)
    formatColors[kCMFormatDescriptionExtension_FullRangeVideo as String] =
        extensions[kCMFormatDescriptionExtension_FullRangeVideo as String] ?? NSNull()
    let metadata: [String: Any] = [
        "width": width, "height": height,
        "layout": isNV12 ? "NV12_Y_then_interleaved_CbCr" : "YUY2_packed_Y0_Cb_Y1_Cr",
        "pixel_format": format, "pixel_format_fourcc": fourCC, "nominal_range": range,
        "planes": planes, "size_bytes": bytes.count,
        "image_buffer_attachments": ["should_propagate": colors(propagated),
                                     "should_not_propagate": colors(nonpropagated)],
        "format_description_color_extensions": formatColors,
    ]
    guard JSONSerialization.isValidJSONObject(metadata) else { throw RawFrameError.invalidMetadata }
    return (bytes, metadata, isNV12 ? "nv12" : "yuy2")
}

final class RawFrameSnapshots {
    private let directory: URL
    private let requestedIndices: [Int]
    private let selected: Set<Int>
    private let diskQueue: DispatchQueue
    // Only the capture queue uses submitted; all remaining mutable state belongs
    // to diskQueue. At most 128 owned frame copies can be pending.
    private var submitted: Set<Int> = []
    private var frames: [[String: Any]] = []
    private var failures: [[String: Any]] = []
    private var firstFailure: Error?

    init(directory: URL, indices: [Int],
         diskQueue: DispatchQueue = DispatchQueue(label: "v1simple.camera.raw-frames", qos: .utility)) throws {
        guard (1...maximumRawFrameSnapshots).contains(indices.count), indices.allSatisfy({ $0 >= 0 }),
              Set(indices).count == indices.count else { throw RawFrameError.invalidSelection }
        self.directory = directory
        self.requestedIndices = indices.sorted()
        self.selected = Set(indices)
        self.diskQueue = diskQueue
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        guard try FileManager.default.contentsOfDirectory(atPath: directory.path).isEmpty else {
            throw RawFrameError.nonemptyOutputDirectory
        }
        try writeManifest(result: "IN_PROGRESS")
    }

    private func writeManifest(result: String) throws {
        let saved = Set(frames.compactMap { $0["video_frame_index"] as? Int })
        try writeMarker(directory.appendingPathComponent("manifest.json"), [
            "schema_version": 1, "kind": "diagnostic_raw_camera_snapshots",
            "result": result, "phase": "recording", "index_basis": "zero_based_successfully_appended_recording_frames",
            "requested_video_frame_indices": requestedIndices,
            "missing_video_frame_indices": requestedIndices.filter { !saved.contains($0) },
            "frames": frames, "failures": failures,
        ])
    }

    func captureAppended(_ buffer: CVPixelBuffer?, formatDescription: CMFormatDescription?,
                         videoFrameIndex: Int, timingRecord: [String: Any],
                         onFailure: @escaping (Error) -> Void) {
        guard timingRecord["phase"] as? String == "recording",
              timingRecord["status"] as? String == "written",
              selected.contains(videoFrameIndex), submitted.insert(videoFrameIndex).inserted else { return }
        let copied: Result<(bytes: Data, metadata: [String: Any], fileExtension: String), Error> = Result {
            guard let buffer else { throw RawFrameError.missingImageBuffer }
            return try rawFrameCopy(buffer, formatDescription: formatDescription)
        }
        // The closure captures only copied Data and scalar metadata, not buffer
        // or formatDescription. No disk write or wait occurs on the capture queue.
        diskQueue.async {
            do {
                let snapshot = try copied.get()
                let name = String(format: "frame-%06d", videoFrameIndex) + "." + snapshot.fileExtension
                try snapshot.bytes.write(to: self.directory.appendingPathComponent(name), options: .withoutOverwriting)
                var record = timingRecord
                record.merge(snapshot.metadata) { _, value in value }
                record["video_frame_index"] = videoFrameIndex
                record["source_frame_seq"] = timingRecord["frame_seq"] ?? NSNull()
                record["file"] = name
                self.frames.append(record)
                try self.writeManifest(result: self.firstFailure == nil ? "IN_PROGRESS" : "FAILED")
            } catch {
                self.firstFailure = self.firstFailure ?? error
                self.failures.append(["code": "snapshot_failed", "video_frame_index": videoFrameIndex,
                                      "error": sanitizedError(error)])
                try? self.writeManifest(result: "FAILED")
                onFailure(error)
            }
        }
    }

    func finish(_ completion: @escaping (Error?) -> Void) {
        diskQueue.async {
            let saved = Set(self.frames.compactMap { $0["video_frame_index"] as? Int })
            let missing = self.requestedIndices.filter { !saved.contains($0) }
            if !missing.isEmpty {
                self.firstFailure = self.firstFailure ?? RawFrameError.incompleteSelection
                self.failures.append(["code": "requested_frames_not_written", "video_frame_indices": missing])
            }
            do {
                try self.writeManifest(result: self.firstFailure == nil ? "PASS" : "FAILED")
            } catch {
                self.firstFailure = self.firstFailure ?? error
            }
            completion(self.firstFailure)
        }
    }
}

func runRawFrameSelfTest() -> Never {
    func require(_ condition: Bool, _ code: Int) throws {
        if !condition { throw NSError(domain: "v1simple.camera.raw-frames.selftest", code: code) }
    }
    func finish(_ snapshots: RawFrameSnapshots) throws -> Error? {
        let completed = DispatchSemaphore(value: 0)
        var result: Error?
        snapshots.finish { result = $0; completed.signal() }
        try require(completed.wait(timeout: .now() + 5) == .success, 1)
        return result
    }
    func manifest(_ directory: URL) throws -> [String: Any] {
        let data = try Data(contentsOf: directory.appendingPathComponent("manifest.json"))
        return try JSONSerialization.jsonObject(with: data) as! [String: Any]
    }
    let directory = FileManager.default.temporaryDirectory.appendingPathComponent(
        "v1simple-raw-frames-selftest-\(UUID().uuidString)", isDirectory: true
    )
    do {
        defer { try? FileManager.default.removeItem(at: directory) }
        let boundary = (0..<128).map(String.init).joined(separator: ",")
        try require(try parseRawFrameIndices(boundary) == Array(0..<128), 2)
        for invalid in [boundary + ",128", "", "-1", "1,1", "1,", "1.0", " 1"] {
            do {
                _ = try parseRawFrameIndices(invalid)
                throw NSError(domain: "v1simple.camera.raw-frames.selftest", code: 3)
            } catch RawFrameError.invalidSelection { }
        }
        do {
            _ = try RawFrameSnapshots(directory: directory, indices: Array(0...128))
            throw NSError(domain: "v1simple.camera.raw-frames.selftest", code: 4)
        } catch RawFrameError.invalidSelection { }

        // A small native buffer with padded rows proves the stored layout has
        // neither padding nor any pixel conversion. UV is interleaved CbCr.
        var created: CVPixelBuffer?
        let attributes = [kCVPixelBufferBytesPerRowAlignmentKey as String: 16] as CFDictionary
        try require(CVPixelBufferCreate(kCFAllocatorDefault, 6, 4,
                    kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
                    attributes, &created) == kCVReturnSuccess, 5)
        let buffer = created!
        func fill(_ usePattern: Bool) throws {
            try require(CVPixelBufferLockBaseAddress(buffer, []) == kCVReturnSuccess, 6)
            defer { CVPixelBufferUnlockBaseAddress(buffer, []) }
            for plane in 0..<2 {
                let stride = CVPixelBufferGetBytesPerRowOfPlane(buffer, plane)
                let rows = CVPixelBufferGetHeightOfPlane(buffer, plane)
                try require(stride > 6, 7)
                let base = CVPixelBufferGetBaseAddressOfPlane(buffer, plane)!
                memset(base, 0xED, stride * rows)
                if usePattern {
                    for row in 0..<rows {
                        for column in 0..<6 {
                            base.storeBytes(of: UInt8(plane * 80 + row * 6 + column),
                                            toByteOffset: row * stride + column, as: UInt8.self)
                        }
                    }
                }
            }
        }
        try fill(true)
        CVBufferSetAttachment(buffer, kCVImageBufferYCbCrMatrixKey,
                              kCVImageBufferYCbCrMatrix_ITU_R_709_2, .shouldPropagate)
        var format: CMVideoFormatDescription?
        try require(CMVideoFormatDescriptionCreateForImageBuffer(
                    allocator: kCFAllocatorDefault, imageBuffer: buffer,
                    formatDescriptionOut: &format) == noErr, 8)
        let expected = Data((0..<24).map(UInt8.init) + (80..<92).map(UInt8.init))
        let copied = try rawFrameCopy(buffer, formatDescription: format)
        let planes = copied.metadata["planes"] as! [[String: Any]]
        let attachments = copied.metadata["image_buffer_attachments"] as! [String: [String: Any]]
        try require(copied.bytes == expected && copied.metadata["nominal_range"] as? String == "video"
                    && copied.metadata["pixel_format_fourcc"] as? String == "420v"
                    && planes[0]["size_bytes"] as? Int == 24
                    && planes[1]["offset_bytes"] as? Int == 24
                    && planes[1]["stored_bytes_per_row"] as? Int == 6
                    && attachments["should_propagate"]?[kCVImageBufferYCbCrMatrixKey as String] as? String
                        == kCVImageBufferYCbCrMatrix_ITU_R_709_2 as String, 9)

        let diskQueue = DispatchQueue(label: "v1simple.camera.raw-frames.selftest")
        let snapshots = try RawFrameSnapshots(directory: directory, indices: [2, 0], diskQueue: diskQueue)
        // A suspended disk queue makes capture return before any disk write.
        // Mutating the source afterward proves queued writes own their bytes.
        diskQueue.suspend()
        var callbackFailure: Error?
        func submit(_ index: Int, _ sequence: Int, phase: String = "recording",
                    status: String = "written", image: CVPixelBuffer? = buffer) {
            snapshots.captureAppended(image, formatDescription: format, videoFrameIndex: index,
                timingRecord: ["phase": phase, "status": status, "frame_seq": sequence,
                               "source_pts_value": 1_000 + sequence, "source_pts_timescale": 600,
                               "source_duration_value": 5, "source_duration_timescale": 600,
                               "callback_host_ns": 40_000 + sequence, "host_capture_ns": 39_000 + sequence,
                               "video_pts_value": index * 5, "video_pts_timescale": 600]) {
                    callbackFailure = $0
                }
        }
        submit(0, 4, phase: "preflight", image: nil)
        submit(0, 8, status: "writer_drop", image: nil)
        submit(0, 9)
        submit(1, 11, image: nil)
        submit(2, 12, status: "capture_drop", image: nil)
        submit(2, 13)
        submit(2, 14, image: nil) // Duplicate submission must not replace the source.
        do { try fill(false) } catch { diskQueue.resume(); throw error }
        diskQueue.resume()
        try require(try finish(snapshots) == nil && callbackFailure == nil, 10)
        let saved = try manifest(directory)
        let frames = saved["frames"] as! [[String: Any]]
        try require(saved["result"] as? String == "PASS" && frames.count == 2
                    && saved["requested_video_frame_indices"] as? [Int] == [0, 2]
                    && saved["missing_video_frame_indices"] as? [Int] == []
                    && frames.compactMap { $0["video_frame_index"] as? Int } == [0, 2]
                    && frames.compactMap { $0["source_frame_seq"] as? Int } == [9, 13]
                    && frames[1]["source_pts_value"] as? Int == 1_013
                    && frames[1]["source_pts_timescale"] as? Int == 600
                    && frames[1]["host_capture_ns"] as? Int == 39_013
                    && frames[1]["video_pts_value"] as? Int == 10, 11)
        for frame in frames {
            try require(try Data(contentsOf: directory.appendingPathComponent(frame["file"] as! String)) == expected, 12)
        }
        try require(try FileManager.default.contentsOfDirectory(atPath: directory.path).count == 3, 13)
        do {
            _ = try RawFrameSnapshots(directory: directory, indices: [0])
            throw NSError(domain: "v1simple.camera.raw-frames.selftest", code: 14)
        } catch RawFrameError.nonemptyOutputDirectory { }

        let missingDirectory = directory.appendingPathComponent("missing")
        let missing = try RawFrameSnapshots(directory: missingDirectory, indices: Array(0..<128))
        try require(try finish(missing) != nil, 15)
        let missingManifest = try manifest(missingDirectory)
        try require(missingManifest["result"] as? String == "FAILED"
                    && missingManifest["missing_video_frame_indices"] as? [Int] == Array(0..<128), 16)
        let failureDirectory = directory.appendingPathComponent("write-failure")
        let failure = try RawFrameSnapshots(directory: failureDirectory, indices: [0])
        let collision = failureDirectory.appendingPathComponent("frame-000000.nv12")
        try Data([7]).write(to: collision)
        failure.captureAppended(buffer, formatDescription: format, videoFrameIndex: 0,
                                timingRecord: ["phase": "recording", "status": "written", "frame_seq": 20]) {
            callbackFailure = $0
        }
        try require(try finish(failure) != nil && callbackFailure != nil, 17)
        try require(try manifest(failureDirectory)["result"] as? String == "FAILED"
                    && Data(contentsOf: collision) == Data([7]), 18)

        // Packed YUY2 has width * 2 active bytes per row and no vertical
        // subsampling. An odd height and padded rows catch a mistaken NV12 path.
        var packed: CVPixelBuffer?
        try require(CVPixelBufferCreate(kCFAllocatorDefault, 6, 3,
                    kCVPixelFormatType_422YpCbCr8_yuvs,
                    attributes, &packed) == kCVReturnSuccess, 19)
        let packedBuffer = packed!
        let packedExpected = Data((0..<36).map { UInt8(20 + $0 * 3) })
        let packedStride = CVPixelBufferGetBytesPerRow(packedBuffer)
        try require(packedStride > 12, 21)
        try require(CVPixelBufferLockBaseAddress(packedBuffer, []) == kCVReturnSuccess, 20)
        let packedBase = CVPixelBufferGetBaseAddress(packedBuffer)!
        memset(packedBase, 0xED, packedStride * 3)
        packedExpected.withUnsafeBytes { source in
            for row in 0..<3 {
                packedBase.advanced(by: row * packedStride).copyMemory(
                    from: source.baseAddress!.advanced(by: row * 12), byteCount: 12)
            }
        }
        CVPixelBufferUnlockBaseAddress(packedBuffer, [])
        CVBufferSetAttachment(packedBuffer, kCVImageBufferYCbCrMatrixKey,
                              kCVImageBufferYCbCrMatrix_ITU_R_601_4, .shouldNotPropagate)
        let packedCopy = try rawFrameCopy(packedBuffer, formatDescription: nil)
        let packedPlanes = packedCopy.metadata["planes"] as! [[String: Any]]
        let packedAttachments = packedCopy.metadata["image_buffer_attachments"] as! [String: [String: Any]]
        try require(packedCopy.bytes == packedExpected && packedCopy.fileExtension == "yuy2"
                    && packedCopy.metadata["layout"] as? String == "YUY2_packed_Y0_Cb_Y1_Cr"
                    && packedCopy.metadata["pixel_format_fourcc"] as? String == "yuvs"
                    && packedCopy.metadata["nominal_range"] as? String == "video"
                    && packedCopy.metadata["height"] as? Int == 3 && packedPlanes.count == 1
                    && packedPlanes[0]["source_bytes_per_row"] as? Int == packedStride
                    && packedPlanes[0]["stored_bytes_per_row"] as? Int == 12
                    && packedPlanes[0]["offset_bytes"] as? Int == 0
                    && packedPlanes[0]["size_bytes"] as? Int == 36
                    && packedAttachments["should_not_propagate"]?[kCVImageBufferYCbCrMatrixKey as String] as? String
                        == kCVImageBufferYCbCrMatrix_ITU_R_601_4 as String, 22)
        let packedDirectory = directory.appendingPathComponent("packed")
        let packedSnapshots = try RawFrameSnapshots(directory: packedDirectory, indices: [7], diskQueue: diskQueue)
        callbackFailure = nil
        diskQueue.suspend()
        packedSnapshots.captureAppended(packedBuffer, formatDescription: nil, videoFrameIndex: 7,
            timingRecord: ["phase": "recording", "status": "written", "frame_seq": 30,
                           "source_pts_value": 1_030, "source_pts_timescale": 600]) { callbackFailure = $0 }
        let locked = CVPixelBufferLockBaseAddress(packedBuffer, [])
        if locked == kCVReturnSuccess {
            memset(CVPixelBufferGetBaseAddress(packedBuffer)!, 0xED, packedStride * 3)
            CVPixelBufferUnlockBaseAddress(packedBuffer, [])
        }
        diskQueue.resume()
        try require(locked == kCVReturnSuccess, 23)
        try require(try finish(packedSnapshots) == nil && callbackFailure == nil, 24)
        let packedManifest = try manifest(packedDirectory)
        let packedFrame = (packedManifest["frames"] as! [[String: Any]])[0]
        try require(packedManifest["kind"] as? String == "diagnostic_raw_camera_snapshots"
                    && packedFrame["file"] as? String == "frame-000007.yuy2"
                    && packedFrame["video_frame_index"] as? Int == 7
                    && packedFrame["source_frame_seq"] as? Int == 30
                    && packedFrame["source_pts_value"] as? Int == 1_030, 25)
        try require(try Data(contentsOf: packedDirectory.appendingPathComponent("frame-000007.yuy2"))
                    == packedExpected, 26)
        var unsupported: CVPixelBuffer?
        try require(CVPixelBufferCreate(kCFAllocatorDefault, 6, 4, kCVPixelFormatType_32BGRA,
                    nil, &unsupported) == kCVReturnSuccess, 27)
        do {
            _ = try rawFrameCopy(unsupported!, formatDescription: nil)
            throw NSError(domain: "v1simple.camera.raw-frames.selftest", code: 28)
        } catch RawFrameError.unsupportedPixelFormat { }
    } catch {
        let nsError = error as NSError
        fputs("camera recorder raw frames self-test: FAIL \(nsError.domain) \(nsError.code)\n", stderr)
        exit(1)
    }
    print("camera recorder raw frames self-test: PASS")
    exit(0)
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
    private let rawFrameSnapshots: RawFrameSnapshots?
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

    init(width: Int32, height: Int32, frameRate: Int32, rawFrameSnapshots: RawFrameSnapshots? = nil) {
        self.width = width
        self.height = height
        self.frameRate = frameRate
        self.rawFrameSnapshots = rawFrameSnapshots
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
        rawFrameSnapshots?.captureAppended(
            CMSampleBufferGetImageBuffer(sampleBuffer),
            formatDescription: CMSampleBufferGetFormatDescription(sampleBuffer),
            videoFrameIndex: frameCount - 1, timingRecord: prepared.record
        ) { error in
            self.queue.async {
                self.recordFailure(code: "raw_frame_snapshot_failed",
                                   message: "diagnostic raw camera frame snapshot failed", error: error)
            }
        }
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
        if phase == "recording", let rawFrameSnapshots {
            rawFrameSnapshots.finish { error in
                self.queue.async {
                    if let error {
                        self.recordFailure(code: "raw_frame_finalize_failed",
                                           message: "diagnostic raw camera frame output failed to finalize", error: error)
                    }
                    self.completeStopMarkers(completed)
                }
            }
        } else {
            completeStopMarkers(completed)
        }
    }

    private func completeStopMarkers(_ completed: DispatchSemaphore) {
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
if CommandLine.arguments.contains("--self-test-raw-frames") {
    runRawFrameSelfTest()
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

let rawFrameSnapshots: RawFrameSnapshots?
do {
    rawFrameSnapshots = try options.rawFrameOutputDirectory.map {
        try RawFrameSnapshots(directory: $0, indices: options.rawFrameIndices)
    }
} catch {
    try? writeMarker(options.failureMarker, ["result": "CAPTURE_FAILED",
                     "code": "raw_frame_setup_failed", "error": sanitizedError(error)])
    fputs("raw camera frame diagnostic output could not be initialized\n", stderr)
    exit(3)
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
let recorder = FrameRecorder(width: options.width, height: options.height, frameRate: options.frameRate,
                             rawFrameSnapshots: rawFrameSnapshots)
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

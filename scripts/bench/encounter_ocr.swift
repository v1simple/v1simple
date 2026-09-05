// Local-only OCR bridge. Each input line contains base64 PNG crops; output
// retains all recognized candidates and their locations without interpretation.
import Foundation
import Vision
import AppKit
import Darwin

func processLine(_ line: String) {
    var requestID: String? = nil
    do {
        guard let data = line.data(using: .utf8) else {
            throw NSError(domain: "encounterOCR", code: 1)
        }
        let value = try JSONSerialization.jsonObject(with: data)
        let input: [String]
        if let legacy = value as? [String] {
            input = legacy
        } else if let envelope = value as? [String: Any],
                  Set(envelope.keys) == Set(["request_id", "crops"]),
                  let identifier = envelope["request_id"] as? String,
                  let crops = envelope["crops"] as? [String] {
            requestID = identifier
            input = crops
        } else {
            throw NSError(domain: "encounterOCR", code: 1)
        }
        var output: [[String: Any]] = []
        for encoded in input {
            guard let bytes = Data(base64Encoded: encoded),
                  let image = NSImage(data: bytes),
                  let cg = image.cgImage(forProposedRect: nil, context: nil, hints: nil) else {
                throw NSError(domain: "encounterOCR", code: 2)
            }
            let request = VNRecognizeTextRequest()
            request.recognitionLevel = .accurate
            request.usesLanguageCorrection = false
            request.recognitionLanguages = ["en-US"]
            request.revision = VNRecognizeTextRequestRevision3
            request.usesCPUOnly = true
            try VNImageRequestHandler(cgImage: cg).perform([request])
            let rows: [[String: Any]] = (request.results ?? []).map { observation in
                let box = observation.boundingBox
                return ["box": [box.minX, box.minY, box.maxX, box.maxY],
                        "candidates": observation.topCandidates(3).map {
                            ["text": $0.string, "confidence": $0.confidence] as [String: Any]
                        }]
            }
            output.append(["revision": request.revision, "rows": rows])
        }
        var response: [String: Any] = ["crops": output]
        if let identifier = requestID { response["request_id"] = identifier }
        let bytes = try JSONSerialization.data(withJSONObject: response, options: [.sortedKeys])
        print(String(data: bytes, encoding: .utf8)!)
    } catch {
        let bytes = try! JSONSerialization.data(withJSONObject: ["error": "local Vision OCR failed", "detail": String(describing: error)])
        print(String(data: bytes, encoding: .utf8)!)
    }
    fflush(stdout)
}

while let line = readLine() {
    autoreleasepool { processLine(line) }
}

// Local-only OCR bridge. Each input line contains base64 PNG crops; output
// retains all recognized candidates and their locations without interpretation.
import Foundation
import Vision
import AppKit

while let line = readLine() {
    do {
        guard let data = line.data(using: .utf8),
              let input = try JSONSerialization.jsonObject(with: data) as? [String] else {
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
        let bytes = try JSONSerialization.data(withJSONObject: ["crops": output], options: [.sortedKeys])
        print(String(data: bytes, encoding: .utf8)!)
    } catch {
        let bytes = try! JSONSerialization.data(withJSONObject: ["error": "local Vision OCR failed", "detail": String(describing: error)])
        print(String(data: bytes, encoding: .utf8)!)
    }
}

import XCTest
@testable import v1replay

final class V1BenchIdleLeadTests: XCTestCase {
    func testDefaultPreservesAuthoredScenarioTiming() throws {
        XCTAssertEqual(try parseBenchIdleLeadSeconds(nil, hasExternalScenario: false), 0)
        XCTAssertEqual(try parseBenchIdleLeadSeconds(nil, hasExternalScenario: true), 0)
    }

    func testExternalScenarioAcceptsBoundedExplicitIdleLead() throws {
        for (raw, expected) in [("0", 0.0), ("3", 3.0), ("0.5", 0.5), ("30", 30.0)] {
            XCTAssertEqual(try parseBenchIdleLeadSeconds(raw, hasExternalScenario: true), expected)
        }
    }

    func testBuiltInScenariosAndInvalidDurationsRejectOverride() {
        XCTAssertThrowsError(try parseBenchIdleLeadSeconds("3", hasExternalScenario: false))
        for raw in ["-1", "30.001", "nan", "inf", "-inf", "true", "bad", ""] {
            XCTAssertThrowsError(try parseBenchIdleLeadSeconds(raw, hasExternalScenario: true), raw)
        }
    }
}

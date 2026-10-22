import Foundation
import XCTest
@testable import v1replay

final class V1CommandAckFaultTests: XCTestCase {
    func testCLIOptionIsExplicitBoundedAndBenchOnly() throws {
        XCTAssertNil(try parseCommandAckHoldMilliseconds(nil, bench: false))
        XCTAssertEqual(try parseCommandAckHoldMilliseconds("0", bench: true), 0)
        XCTAssertEqual(try parseCommandAckHoldMilliseconds("7000", bench: true), 7000)
        XCTAssertEqual(try parseCommandAckHoldMilliseconds("10000", bench: true), 10000)
        XCTAssertThrowsError(try parseCommandAckHoldMilliseconds("7000", bench: false))
        for value in ["-1", "10001", "1.5", "true"] {
            XCTAssertThrowsError(try parseCommandAckHoldMilliseconds(value, bench: true))
        }
    }

    func testFaultEventsRetainOperationAndTimeWithoutCentralIdentity() throws {
        let event = ReplayCommandAckFaultEvent(state: .held, characteristic: "B6D4",
                                              holdMilliseconds: 7000, hostMonotonicNs: 123)
        let json = String(event.machineEventLine.dropFirst("V1REPLAY_EVENT ".count))
        let value = try XCTUnwrap(JSONSerialization.jsonObject(with: Data(json.utf8)) as? [String: Any])
        XCTAssertEqual(Set(value.keys), ["state", "schemaVersion", "commandID", "characteristic",
                                         "holdMilliseconds", "hostMonotonicNs"])
        XCTAssertEqual(value["state"] as? String, "command_ack_held")
        XCTAssertEqual(value["commandID"] as? Int, 0x41)
        XCTAssertEqual(value["hostMonotonicNs"] as? Int, 123)
    }
}

import XCTest
@testable import v1replay

final class V1PersistenceScenarioTests: XCTestCase {
    func testPersistenceStimulusProvidesRetentionClearAndLivePreemptionOpportunities() {
        let encounter = PersistenceScenario.make()
        XCTAssertEqual(encounter.samples.count, 64 * 3)
        func at(_ second: Int) -> TimedSample { encounter.samples[second * 3] }
        XCTAssertEqual(at(6).priorityAlert?.frequencyMHz, 24_150)
        XCTAssertTrue(at(10).alerts.isEmpty)
        XCTAssertTrue(at(17).alerts.isEmpty) // Longer than every selectable persistence value.
        XCTAssertEqual(at(18).priorityAlert?.frequencyMHz, 10_525)
        XCTAssertTrue(at(22).alerts.isEmpty)
        XCTAssertEqual(at(23).priorityAlert?.frequencyMHz, 34_700) // New live alert after 1s idle.
        XCTAssertEqual(at(31).secondaryAlerts.first?.frequencyMHz, 10_525)
        XCTAssertEqual(at(35).priorityAlert?.frequencyMHz, at(31).priorityAlert?.frequencyMHz)
        XCTAssertTrue(at(35).secondaryAlerts.isEmpty)
        XCTAssertTrue(at(42).secondaryAlerts.isEmpty)
        XCTAssertEqual(at(43).priorityAlert?.frequencyMHz, 24_150)
        XCTAssertEqual(at(51).secondaryAlerts.first?.frequencyMHz, 10_525)
        XCTAssertTrue(at(55).alerts.isEmpty)
        XCTAssertTrue(at(63).alerts.isEmpty)
        XCTAssertTrue(encounter.samples.allSatisfy { !$0.scenarioArrowBlink && !$0.muted })
        XCTAssertEqual(BenchScenario.make().samples.count, BenchScenario.durationSeconds * BenchScenario.cadenceHz)
    }
}

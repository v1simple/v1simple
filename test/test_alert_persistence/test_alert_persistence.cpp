/**
 * test_alert_persistence.cpp - Unit tests for AlertPersistenceModule
 *
 * Tests the alert persistence logic which keeps alerts visible
 * for a configurable duration after V1 clears them.
 */
#include <unity.h>

#include "../../src/packet_parser_types.h"

// ============================================================================
// Test Helpers
// ============================================================================

static AlertData makeAlert(Band band,
                           Direction direction,
                           uint8_t frontStrength,
                           uint8_t rearStrength,
                           uint32_t frequency,
                           bool valid = true) {
    return AlertData::create(band, direction, frontStrength, rearStrength, frequency, valid);
}

// ============================================================================
// Mock AlertPersistenceModule (minimal implementation for testing logic)
// ============================================================================

class TestableAlertPersistence {
public:
    void setPersistedAlert(const AlertData& alert) {
        persistedAlert = alert;
        alertPersistenceActive = false;
        alertClearedTime = 0;
    }
    
    void startPersistence(unsigned long now) {
        if (persistedAlert.isValid && alertClearedTime == 0) {
            alertClearedTime = now;
            alertPersistenceActive = true;
        }
    }
    
    void clearPersistence() {
        persistedAlert = AlertData();
        alertPersistenceActive = false;
        alertClearedTime = 0;
    }
    
    bool shouldShowPersisted(unsigned long now, unsigned long persistMs) const {
        return alertPersistenceActive && (now - alertClearedTime) < persistMs;
    }
    
    const AlertData& getPersistedAlert() const { return persistedAlert; }
    bool isPersistenceActive() const { return alertPersistenceActive; }
    
private:
    AlertData persistedAlert;
    unsigned long alertClearedTime = 0;
    bool alertPersistenceActive = false;
};

// Global test instance
static TestableAlertPersistence persistence;

// ============================================================================
// Test Setup/Teardown
// ============================================================================

void setUp() {
    persistence.clearPersistence();
}

void tearDown() {
    // Nothing to clean up
}

// ============================================================================
// Test Cases: Basic Persistence
// ============================================================================

void test_initial_state_no_persistence() {
    // Fresh instance should have no persistence
    TEST_ASSERT_FALSE(persistence.isPersistenceActive());
    TEST_ASSERT_FALSE(persistence.getPersistedAlert().isValid);
}

void test_set_persisted_alert_stores_alert() {
    AlertData alert = makeAlert(BAND_KA, DIR_FRONT, 5, 0, 34700, true);
    
    persistence.setPersistedAlert(alert);
    
    // Alert should be stored but persistence not yet active
    const AlertData& stored = persistence.getPersistedAlert();
    TEST_ASSERT_TRUE(stored.isValid);
    TEST_ASSERT_EQUAL(BAND_KA, stored.band);
    TEST_ASSERT_EQUAL(DIR_FRONT, stored.direction);
    TEST_ASSERT_EQUAL(34700, stored.frequency);
    TEST_ASSERT_FALSE(persistence.isPersistenceActive());
}

void test_start_persistence_activates_timer() {
    AlertData alert = makeAlert(BAND_KA, DIR_FRONT, 5, 0, 34700, true);
    persistence.setPersistedAlert(alert);
    
    persistence.startPersistence(1000);  // Start at 1000ms
    
    TEST_ASSERT_TRUE(persistence.isPersistenceActive());
}

void test_start_persistence_requires_valid_alert() {
    // Don't set any alert - persistence should not activate
    persistence.startPersistence(1000);
    
    TEST_ASSERT_FALSE(persistence.isPersistenceActive());
}

void test_start_persistence_only_once() {
    AlertData alert = makeAlert(BAND_KA, DIR_FRONT, 5, 0, 34700, true);
    persistence.setPersistedAlert(alert);
    
    // First start
    persistence.startPersistence(1000);
    TEST_ASSERT_TRUE(persistence.isPersistenceActive());
    
    // Second start should have no effect (alertClearedTime already set)
    persistence.startPersistence(2000);  // Different time
    
    // Should still show persisted at original time + persistence duration
    TEST_ASSERT_TRUE(persistence.shouldShowPersisted(1500, 3000));  // Within 3s of 1000ms
}

// ============================================================================
// Test Cases: Persistence Duration
// ============================================================================

void test_should_show_persisted_within_duration() {
    AlertData alert = makeAlert(BAND_KA, DIR_FRONT, 5, 0, 34700, true);
    persistence.setPersistedAlert(alert);
    persistence.startPersistence(1000);
    
    // Check at various points within 3-second persistence
    TEST_ASSERT_TRUE(persistence.shouldShowPersisted(1000, 3000));   // At start
    TEST_ASSERT_TRUE(persistence.shouldShowPersisted(1500, 3000));   // 500ms in
    TEST_ASSERT_TRUE(persistence.shouldShowPersisted(2000, 3000));   // 1000ms in
    TEST_ASSERT_TRUE(persistence.shouldShowPersisted(3999, 3000));   // 2999ms in (just under)
}

void test_should_not_show_persisted_after_duration() {
    AlertData alert = makeAlert(BAND_KA, DIR_FRONT, 5, 0, 34700, true);
    persistence.setPersistedAlert(alert);
    persistence.startPersistence(1000);
    
    // Check after 3-second persistence expires
    TEST_ASSERT_FALSE(persistence.shouldShowPersisted(4000, 3000));  // Exactly at expiry
    TEST_ASSERT_FALSE(persistence.shouldShowPersisted(5000, 3000));  // 1s after expiry
    TEST_ASSERT_FALSE(persistence.shouldShowPersisted(10000, 3000)); // Long after
}

void test_different_persistence_durations() {
    AlertData alert = makeAlert(BAND_KA, DIR_FRONT, 5, 0, 34700, true);
    persistence.setPersistedAlert(alert);
    persistence.startPersistence(1000);
    
    // 1 second persistence
    TEST_ASSERT_TRUE(persistence.shouldShowPersisted(1500, 1000));   // Within 1s
    TEST_ASSERT_FALSE(persistence.shouldShowPersisted(2000, 1000));  // At 1s (expired)
    
    // 5 second persistence (same start time)
    TEST_ASSERT_TRUE(persistence.shouldShowPersisted(4000, 5000));   // Within 5s
    TEST_ASSERT_TRUE(persistence.shouldShowPersisted(5999, 5000));   // Just under 5s
    TEST_ASSERT_FALSE(persistence.shouldShowPersisted(6000, 5000));  // At 5s (expired)
}

void test_zero_persistence_duration() {
    AlertData alert = makeAlert(BAND_KA, DIR_FRONT, 5, 0, 34700, true);
    persistence.setPersistedAlert(alert);
    persistence.startPersistence(1000);
    
    // Zero duration means never show persisted (0 is disabled)
    TEST_ASSERT_FALSE(persistence.shouldShowPersisted(1000, 0));
    TEST_ASSERT_FALSE(persistence.shouldShowPersisted(1001, 0));
}

// ============================================================================
// Test Cases: Clear Persistence
// ============================================================================

void test_clear_persistence_resets_all_state() {
    AlertData alert = makeAlert(BAND_KA, DIR_FRONT, 5, 0, 34700, true);
    persistence.setPersistedAlert(alert);
    persistence.startPersistence(1000);
    
    // Verify active
    TEST_ASSERT_TRUE(persistence.isPersistenceActive());
    TEST_ASSERT_TRUE(persistence.getPersistedAlert().isValid);
    
    // Clear
    persistence.clearPersistence();
    
    // Verify cleared
    TEST_ASSERT_FALSE(persistence.isPersistenceActive());
    TEST_ASSERT_FALSE(persistence.getPersistedAlert().isValid);
    TEST_ASSERT_FALSE(persistence.shouldShowPersisted(1500, 3000));
}

void test_clear_allows_new_persistence() {
    // First alert
    AlertData alert1 = makeAlert(BAND_KA, DIR_FRONT, 5, 0, 34700, true);
    persistence.setPersistedAlert(alert1);
    persistence.startPersistence(1000);
    
    // Clear
    persistence.clearPersistence();
    
    // Second alert (different)
    AlertData alert2 = makeAlert(BAND_K, DIR_REAR, 3, 4, 24150, true);
    persistence.setPersistedAlert(alert2);
    persistence.startPersistence(5000);  // Different start time
    
    // Should show new alert's persistence
    TEST_ASSERT_TRUE(persistence.isPersistenceActive());
    const AlertData& stored = persistence.getPersistedAlert();
    TEST_ASSERT_EQUAL(BAND_K, stored.band);
    TEST_ASSERT_EQUAL(DIR_REAR, stored.direction);
    TEST_ASSERT_EQUAL(24150, stored.frequency);
    
    // Check timing from new start
    TEST_ASSERT_TRUE(persistence.shouldShowPersisted(6000, 3000));   // 1s after new start
    TEST_ASSERT_FALSE(persistence.shouldShowPersisted(8001, 3000));  // After new expiry
}

// ============================================================================
// Test Cases: Different Alert Types
// ============================================================================

void test_persistence_laser_alert() {
    AlertData laser = makeAlert(BAND_LASER, DIR_FRONT, 6, 0, 0, true);  // Laser has no frequency
    persistence.setPersistedAlert(laser);
    persistence.startPersistence(1000);
    
    TEST_ASSERT_TRUE(persistence.isPersistenceActive());
    TEST_ASSERT_EQUAL(BAND_LASER, persistence.getPersistedAlert().band);
    TEST_ASSERT_TRUE(persistence.shouldShowPersisted(2000, 5000));
}

void test_persistence_k_band_alert() {
    AlertData kband = makeAlert(BAND_K, DIR_SIDE, 2, 3, 24100, true);
    persistence.setPersistedAlert(kband);
    persistence.startPersistence(1000);
    
    TEST_ASSERT_TRUE(persistence.isPersistenceActive());
    const AlertData& stored = persistence.getPersistedAlert();
    TEST_ASSERT_EQUAL(BAND_K, stored.band);
    TEST_ASSERT_EQUAL(DIR_SIDE, stored.direction);
    TEST_ASSERT_EQUAL(2, stored.frontStrength);
    TEST_ASSERT_EQUAL(3, stored.rearStrength);
}

void test_persistence_x_band_alert() {
    AlertData xband = makeAlert(BAND_X, DIR_REAR, 0, 4, 10525, true);
    persistence.setPersistedAlert(xband);
    persistence.startPersistence(1000);
    
    TEST_ASSERT_TRUE(persistence.isPersistenceActive());
    TEST_ASSERT_EQUAL(BAND_X, persistence.getPersistedAlert().band);
}

// ============================================================================
// Test Cases: Edge Cases
// ============================================================================

void test_invalid_alert_cannot_start_persistence() {
    AlertData invalid;  // Default constructed - isValid = false
    persistence.setPersistedAlert(invalid);
    persistence.startPersistence(1000);
    
    TEST_ASSERT_FALSE(persistence.isPersistenceActive());
    TEST_ASSERT_FALSE(persistence.shouldShowPersisted(1500, 3000));
}

void test_replacing_alert_before_starting_persistence() {
    AlertData alert1 = makeAlert(BAND_KA, DIR_FRONT, 5, 0, 34700, true);
    persistence.setPersistedAlert(alert1);
    
    // Replace before starting
    AlertData alert2 = makeAlert(BAND_K, DIR_REAR, 3, 4, 24150, true);
    persistence.setPersistedAlert(alert2);
    
    persistence.startPersistence(1000);
    
    // Should have alert2
    TEST_ASSERT_TRUE(persistence.isPersistenceActive());
    TEST_ASSERT_EQUAL(BAND_K, persistence.getPersistedAlert().band);
}

void test_time_wraparound_handling() {
    // Test behavior near unsigned long wraparound
    // This simulates running for ~50 days where millis() wraps
    AlertData alert = makeAlert(BAND_KA, DIR_FRONT, 5, 0, 34700, true);
    persistence.setPersistedAlert(alert);
    
    // Start just before wraparound
    unsigned long nearWrap = 0xFFFFFFFF - 1000;  // ~4 billion - 1 second
    persistence.startPersistence(nearWrap);
    
    TEST_ASSERT_TRUE(persistence.isPersistenceActive());
    
    // Check within 3s persistence (will wrap around)
    // Note: This test may behave differently depending on implementation
    // If persistence does not handle wraparound, this could fail
    TEST_ASSERT_TRUE(persistence.shouldShowPersisted(nearWrap + 1000, 3000));
}

// ============================================================================
// Main Test Runner
// ============================================================================

void runAllTests() {
    // Basic persistence
    RUN_TEST(test_initial_state_no_persistence);
    RUN_TEST(test_set_persisted_alert_stores_alert);
    RUN_TEST(test_start_persistence_activates_timer);
    RUN_TEST(test_start_persistence_requires_valid_alert);
    RUN_TEST(test_start_persistence_only_once);
    
    // Persistence duration
    RUN_TEST(test_should_show_persisted_within_duration);
    RUN_TEST(test_should_not_show_persisted_after_duration);
    RUN_TEST(test_different_persistence_durations);
    RUN_TEST(test_zero_persistence_duration);
    
    // Clear persistence
    RUN_TEST(test_clear_persistence_resets_all_state);
    RUN_TEST(test_clear_allows_new_persistence);
    
    // Different alert types
    RUN_TEST(test_persistence_laser_alert);
    RUN_TEST(test_persistence_k_band_alert);
    RUN_TEST(test_persistence_x_band_alert);
    
    // Edge cases
    RUN_TEST(test_invalid_alert_cannot_start_persistence);
    RUN_TEST(test_replacing_alert_before_starting_persistence);
    RUN_TEST(test_time_wraparound_handling);
}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    runAllTests();
    return UNITY_END();
}

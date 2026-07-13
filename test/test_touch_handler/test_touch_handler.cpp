#include <vector>

#include <unity.h>

#include "../mocks/Arduino.h"
#include "../mocks/Wire.h"
#include "../../src/touch_handler.cpp"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

namespace {

TouchHandler touchHandler;

void queueTouchHandlerInit() {
    Wire.queueEndTransmission(0);                  // Device probe
    Wire.queueEndTransmission(0);                  // readRegister write
    Wire.queueRequestFrom(1, std::vector<uint8_t>{0x00});  // readRegister read
}

std::vector<uint8_t> makeTouchFrame(uint16_t x, uint16_t y, uint8_t numPoints = 1) {
    std::vector<uint8_t> frame(32, 0);
    frame[1] = numPoints;
    frame[2] = static_cast<uint8_t>((x >> 8) & 0x0F);
    frame[3] = static_cast<uint8_t>(x & 0xFF);
    frame[4] = static_cast<uint8_t>((y >> 8) & 0x0F);
    frame[5] = static_cast<uint8_t>(y & 0xFF);
    return frame;
}

void beginTouchHandler() {
    queueTouchHandlerInit();
    TEST_ASSERT_TRUE(touchHandler.begin(17, 18, AXS_TOUCH_ADDR, -1));
    TEST_ASSERT_EQUAL(1, Wire.beginCalls);
    TEST_ASSERT_EQUAL(400000u, Wire.lastClock);
    TEST_ASSERT_EQUAL(5u, Wire.lastTimeout);
}

void queueReadFailure(std::size_t bytesRead = 0) {
    Wire.queueEndTransmission(0);
    Wire.queueRequestFrom(bytesRead, std::vector<uint8_t>(bytesRead, 0));
}

}  // namespace

void setUp() {
    mockMillis = 0;
    mockMicros = 0;
    Wire.resetMock();
    touchHandler = TouchHandler();
}

void tearDown() {}

void test_successful_touch_read_returns_coordinates() {
    beginTouchHandler();
    TEST_ASSERT_TRUE(touchHandler.isAvailable());

    Wire.queueEndTransmission(0);
    Wire.queueRequestFrom(32, makeTouchFrame(0x0123, 0x0088));
    mockMillis = 250;

    int16_t x = 0;
    int16_t y = 0;
    TEST_ASSERT_TRUE(touchHandler.getTouchPoint(x, y));
    TEST_ASSERT_EQUAL(0x0123, x);
    TEST_ASSERT_EQUAL(0x0088, y);
    TEST_ASSERT_EQUAL_UINT32(0, touchHandler.getI2cStallCount());
    TEST_ASSERT_EQUAL_UINT32(0, touchHandler.getI2cRecoveryCount());
}

void test_failed_init_disables_future_bus_polling() {
    Wire.queueEndTransmission(4);
    TEST_ASSERT_FALSE(touchHandler.begin(17, 18, AXS_TOUCH_ADDR, -1));
    TEST_ASSERT_FALSE(touchHandler.isAvailable());

    const int beginTransmissionCalls = Wire.beginTransmissionCalls;
    const int endTransmissionCalls = Wire.endTransmissionCalls;
    const int requestFromCalls = Wire.requestFromCalls;

    int16_t x = 0;
    int16_t y = 0;
    mockMillis = 100;
    TEST_ASSERT_FALSE(touchHandler.getTouchPoint(x, y));
    mockMillis = 200;
    TEST_ASSERT_FALSE(touchHandler.isTouched());

    TEST_ASSERT_EQUAL(beginTransmissionCalls, Wire.beginTransmissionCalls);
    TEST_ASSERT_EQUAL(endTransmissionCalls, Wire.endTransmissionCalls);
    TEST_ASSERT_EQUAL(requestFromCalls, Wire.requestFromCalls);
    TEST_ASSERT_EQUAL_UINT32(0, touchHandler.getI2cStallCount());
    TEST_ASSERT_EQUAL_UINT32(0, touchHandler.getI2cRecoveryCount());
}

void test_short_read_counts_as_transaction_failure() {
    beginTouchHandler();

    queueReadFailure(16);
    mockMillis = 100;

    int16_t x = 0;
    int16_t y = 0;
    TEST_ASSERT_FALSE(touchHandler.getTouchPoint(x, y));
    TEST_ASSERT_EQUAL_UINT32(1, touchHandler.getI2cStallCount());
    TEST_ASSERT_EQUAL_UINT32(0, touchHandler.getI2cRecoveryCount());
}

void test_end_transmission_error_counts_as_transaction_failure() {
    beginTouchHandler();

    Wire.queueEndTransmission(4);
    mockMillis = 100;

    int16_t x = 0;
    int16_t y = 0;
    TEST_ASSERT_FALSE(touchHandler.getTouchPoint(x, y));
    TEST_ASSERT_EQUAL_UINT32(1, touchHandler.getI2cStallCount());
    TEST_ASSERT_EQUAL_UINT32(0, touchHandler.getI2cRecoveryCount());
}

void test_three_consecutive_failures_trigger_single_recovery_attempt() {
    beginTouchHandler();

    queueReadFailure();
    queueReadFailure();
    queueReadFailure();
    mockMillis = 100;

    int16_t x = 0;
    int16_t y = 0;
    TEST_ASSERT_FALSE(touchHandler.getTouchPoint(x, y));
    mockMillis = 101;
    TEST_ASSERT_FALSE(touchHandler.getTouchPoint(x, y));
    mockMillis = 102;
    TEST_ASSERT_FALSE(touchHandler.getTouchPoint(x, y));

    TEST_ASSERT_EQUAL_UINT32(3, touchHandler.getI2cStallCount());
    TEST_ASSERT_EQUAL_UINT32(1, touchHandler.getI2cRecoveryCount());
    TEST_ASSERT_EQUAL(1, Wire.endCalls);
    TEST_ASSERT_EQUAL(2, Wire.beginCalls);
}

void test_recovery_cooldown_blocks_repeat_attempts_inside_250ms() {
    beginTouchHandler();

    queueReadFailure();
    queueReadFailure();
    queueReadFailure();
    queueReadFailure();
    queueReadFailure();
    queueReadFailure();

    int16_t x = 0;
    int16_t y = 0;
    mockMillis = 100;
    TEST_ASSERT_FALSE(touchHandler.getTouchPoint(x, y));
    mockMillis = 101;
    TEST_ASSERT_FALSE(touchHandler.getTouchPoint(x, y));
    mockMillis = 102;
    TEST_ASSERT_FALSE(touchHandler.getTouchPoint(x, y));

    mockMillis = 160;
    TEST_ASSERT_FALSE(touchHandler.getTouchPoint(x, y));
    mockMillis = 161;
    TEST_ASSERT_FALSE(touchHandler.getTouchPoint(x, y));
    mockMillis = 162;
    TEST_ASSERT_FALSE(touchHandler.getTouchPoint(x, y));

    TEST_ASSERT_EQUAL_UINT32(1, touchHandler.getI2cRecoveryCount());
    TEST_ASSERT_EQUAL(1, Wire.endCalls);
    TEST_ASSERT_EQUAL(2, Wire.beginCalls);
}

void test_post_recovery_backoff_skips_bus_access_for_50ms() {
    beginTouchHandler();

    queueReadFailure();
    queueReadFailure();
    queueReadFailure();

    int16_t x = 0;
    int16_t y = 0;
    mockMillis = 100;
    TEST_ASSERT_FALSE(touchHandler.getTouchPoint(x, y));
    mockMillis = 101;
    TEST_ASSERT_FALSE(touchHandler.getTouchPoint(x, y));
    mockMillis = 102;
    TEST_ASSERT_FALSE(touchHandler.getTouchPoint(x, y));

    const int endTransmissionCallsBeforeBackoff = Wire.endTransmissionCalls;
    const int requestFromCallsBeforeBackoff = Wire.requestFromCalls;

    mockMillis = 120;
    TEST_ASSERT_FALSE(touchHandler.getTouchPoint(x, y));

    TEST_ASSERT_EQUAL(endTransmissionCallsBeforeBackoff, Wire.endTransmissionCalls);
    TEST_ASSERT_EQUAL(requestFromCallsBeforeBackoff, Wire.requestFromCalls);
}

void test_post_recovery_backoff_expires_correctly_across_32bit_wrap() {
    beginTouchHandler();

    queueReadFailure();
    queueReadFailure();
    queueReadFailure();

    int16_t x = 0;
    int16_t y = 0;
    mockMillis = 0xFFFFFFF0UL;
    TEST_ASSERT_FALSE(touchHandler.getTouchPoint(x, y));
    mockMillis = 0xFFFFFFF1UL;
    TEST_ASSERT_FALSE(touchHandler.getTouchPoint(x, y));
    mockMillis = 0xFFFFFFF2UL;
    TEST_ASSERT_FALSE(touchHandler.getTouchPoint(x, y));

    Wire.queueEndTransmission(0);
    Wire.queueRequestFrom(32, makeTouchFrame(55, 66));

    const int endTransmissionCallsBeforeBackoff = Wire.endTransmissionCalls;
    const int requestFromCallsBeforeBackoff = Wire.requestFromCalls;

    mockMillis = 20;
    TEST_ASSERT_FALSE(touchHandler.getTouchPoint(x, y));
    TEST_ASSERT_EQUAL(endTransmissionCallsBeforeBackoff, Wire.endTransmissionCalls);
    TEST_ASSERT_EQUAL(requestFromCallsBeforeBackoff, Wire.requestFromCalls);

    mockMillis = 300;
    TEST_ASSERT_TRUE(touchHandler.getTouchPoint(x, y));
    TEST_ASSERT_EQUAL(55, x);
    TEST_ASSERT_EQUAL(66, y);
}

void test_release_debounce_blocks_retap_within_100ms() {
    beginTouchHandler();

    // Register a first tap at t=200 (past touch debounce of 200ms from init)
    Wire.queueEndTransmission(0);
    Wire.queueRequestFrom(32, makeTouchFrame(100, 200));
    mockMillis = 200;
    int16_t x = 0, y = 0;
    TEST_ASSERT_TRUE(touchHandler.getTouchPoint(x, y));  // First tap registered

    // Release finger at t=400 (numPoints=0)
    std::vector<uint8_t> noTouch(32, 0);
    Wire.queueEndTransmission(0);
    Wire.queueRequestFrom(32, noTouch);
    mockMillis = 400;
    TEST_ASSERT_FALSE(touchHandler.getTouchPoint(x, y));  // Release noted

    // At t=499: only 99ms since release — must NOT register a new tap (< 100ms debounce)
    Wire.queueEndTransmission(0);
    Wire.queueRequestFrom(32, makeTouchFrame(100, 200));
    mockMillis = 499;
    TEST_ASSERT_FALSE(touchHandler.getTouchPoint(x, y));
}

void test_release_debounce_allows_retap_at_100ms() {
    beginTouchHandler();

    // Register a first tap at t=200
    Wire.queueEndTransmission(0);
    Wire.queueRequestFrom(32, makeTouchFrame(100, 200));
    mockMillis = 200;
    int16_t x = 0, y = 0;
    TEST_ASSERT_TRUE(touchHandler.getTouchPoint(x, y));  // First tap registered

    // Release finger at t=400 (numPoints=0)
    std::vector<uint8_t> noTouch(32, 0);
    Wire.queueEndTransmission(0);
    Wire.queueRequestFrom(32, noTouch);
    mockMillis = 400;
    TEST_ASSERT_FALSE(touchHandler.getTouchPoint(x, y));  // Release noted

    // At t=500: exactly 100ms since release — SHOULD register a new tap (>= 100ms debounce)
    Wire.queueEndTransmission(0);
    Wire.queueRequestFrom(32, makeTouchFrame(150, 250));
    mockMillis = 500;
    TEST_ASSERT_TRUE(touchHandler.getTouchPoint(x, y));
    TEST_ASSERT_EQUAL(150, x);
    TEST_ASSERT_EQUAL(250, y);
}

void test_i2c_failure_while_finger_down_does_not_register_duplicate_tap() {
    beginTouchHandler();

    Wire.queueEndTransmission(0);
    Wire.queueRequestFrom(32, makeTouchFrame(100, 200));
    mockMillis = 250;
    int16_t x = 0, y = 0;
    TEST_ASSERT_TRUE(touchHandler.getTouchPoint(x, y));

    // A bus failure means the touch state is unknown; it is not evidence that
    // the finger was physically lifted from the panel.
    queueReadFailure();
    mockMillis = 350;
    TEST_ASSERT_FALSE(touchHandler.getTouchPoint(x, y));

    // Same finger still down after both debounce windows. This must remain a
    // held touch, not a second tap generated by an I2C hiccup.
    Wire.queueEndTransmission(0);
    Wire.queueRequestFrom(32, makeTouchFrame(100, 200));
    mockMillis = 451;
    TEST_ASSERT_FALSE(touchHandler.getTouchPoint(x, y));
}

void test_successful_read_resets_failure_streak() {
    beginTouchHandler();

    queueReadFailure();
    queueReadFailure();
    Wire.queueEndTransmission(0);
    Wire.queueRequestFrom(32, makeTouchFrame(0x0100, 0x0020));
    queueReadFailure();
    queueReadFailure();

    int16_t x = 0;
    int16_t y = 0;
    mockMillis = 100;
    TEST_ASSERT_FALSE(touchHandler.getTouchPoint(x, y));
    mockMillis = 101;
    TEST_ASSERT_FALSE(touchHandler.getTouchPoint(x, y));
    mockMillis = 250;
    TEST_ASSERT_TRUE(touchHandler.getTouchPoint(x, y));
    mockMillis = 201;
    TEST_ASSERT_FALSE(touchHandler.getTouchPoint(x, y));
    mockMillis = 202;
    TEST_ASSERT_FALSE(touchHandler.getTouchPoint(x, y));

    TEST_ASSERT_EQUAL_UINT32(4, touchHandler.getI2cStallCount());
    TEST_ASSERT_EQUAL_UINT32(0, touchHandler.getI2cRecoveryCount());
    TEST_ASSERT_EQUAL(0, Wire.endCalls);
    TEST_ASSERT_EQUAL(1, Wire.beginCalls);
}

int main() {
    UNITY_BEGIN();

    RUN_TEST(test_successful_touch_read_returns_coordinates);
    RUN_TEST(test_failed_init_disables_future_bus_polling);
    RUN_TEST(test_short_read_counts_as_transaction_failure);
    RUN_TEST(test_end_transmission_error_counts_as_transaction_failure);
    RUN_TEST(test_three_consecutive_failures_trigger_single_recovery_attempt);
    RUN_TEST(test_recovery_cooldown_blocks_repeat_attempts_inside_250ms);
    RUN_TEST(test_post_recovery_backoff_skips_bus_access_for_50ms);
    RUN_TEST(test_post_recovery_backoff_expires_correctly_across_32bit_wrap);
    RUN_TEST(test_successful_read_resets_failure_streak);
    RUN_TEST(test_release_debounce_blocks_retap_within_100ms);
    RUN_TEST(test_release_debounce_allows_retap_at_100ms);
    RUN_TEST(test_i2c_failure_while_finger_down_does_not_register_duplicate_tap);

    return UNITY_END();
}

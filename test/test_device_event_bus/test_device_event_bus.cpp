/**
 * Device SystemEventBus Concurrency Tests
 *
 * The native event bus tests use std::atomic_flag for locking. On device, the
 * bus uses portMUX_TYPE critical sections. This suite exercises real
 * multi-core contention:
 *   - Producer on Core 0, consumer on Core 1
 *   - Rapid publish under portMUX
 *   - Overflow under contention (drop-oldest policy)
 *   - Event type filtering under concurrent access
 *
 * SAFETY RULES (learned from production freeze):
 *   - Use counting semaphore (not binary) for multi-task start gates.
 *   - Spawned tasks use bounded timeouts (2 s), never portMAX_DELAY.
 *   - Never force-delete worker tasks while they might hold portMUX locks.
 *   - Resources are always released before assertions that can fail.
 */

#include <unity.h>
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

// Include the real SystemEventBus (header-only, uses portMUX on device)
#include "../../src/modules/system/system_event_bus.h"
#include "../device_test_reset.h"

static SystemEventBus bus;

void setUp() {
    bus.reset();
}
void tearDown() {}

// 0 = run full suite, 1..6 = run specific test only.
#ifndef DEVICE_EVENT_BUS_TEST_ID
#define DEVICE_EVENT_BUS_TEST_ID 0
#endif

// ===========================================================================
// BASIC SINGLE-CORE OPERATION ON DEVICE
// ===========================================================================

void test_device_bus_publish_consume() {
    SystemEvent ev;
    ev.type = SystemEventType::BLE_FRAME_PARSED;
    ev.tsMs = millis();
    ev.seq  = 1;

    TEST_ASSERT_TRUE(bus.publish(ev));
    TEST_ASSERT_EQUAL_UINT32(1, bus.getPublishCount());

    SystemEvent out;
    TEST_ASSERT_TRUE(bus.consume(out));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)SystemEventType::BLE_FRAME_PARSED, (uint8_t)out.type);
    TEST_ASSERT_EQUAL_UINT32(1, out.seq);
}

void test_device_bus_overflow_drops_frame_first() {
    // Fill with one control event + rest frames
    SystemEvent control;
    control.type = SystemEventType::BLE_CONNECTED;
    control.seq  = 1;
    bus.publish(control);

    for (uint32_t i = 1; i < SystemEventBus::kCapacity; i++) {
        SystemEvent frame;
        frame.type = SystemEventType::BLE_FRAME_PARSED;
        frame.seq  = i + 1;
        bus.publish(frame);
    }

    // Overflow: should drop oldest frame, preserve control
    SystemEvent overflow;
    overflow.type = SystemEventType::BLE_DISCONNECTED;
    overflow.seq  = 999;
    bus.publish(overflow);

    TEST_ASSERT_EQUAL_UINT32(1, bus.getDropCount());
    deviceTestMetricU32("overflow_drop_count", "overflow", bus.getDropCount(), "count");

    // First consumed should be the control event (preserved)
    SystemEvent out;
    TEST_ASSERT_TRUE(bus.consume(out));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)SystemEventType::BLE_CONNECTED, (uint8_t)out.type);
}

// ===========================================================================
// CROSS-CORE CONTENTION
//
// Safety: producer uses bounded semaphore take (2 s) and bounded bus
// operations (no blocking). Consumer has 5 s deadline. Producer task
// is vTaskDelete'd if it hasn't finished by deadline.
// ===========================================================================

struct ProducerArgs {
    SystemEventBus*   bus;
    uint32_t          count;
    SemaphoreHandle_t startSem;
    TaskHandle_t      notifyTask;
};

static void busProducerTask(void* param) {
    ProducerArgs* args = (ProducerArgs*)param;
    // Bounded wait — bail if start signal never comes
    if (xSemaphoreTake(args->startSem, pdMS_TO_TICKS(2000)) != pdTRUE) {
        xTaskNotifyGive(args->notifyTask);
        vTaskDelete(NULL);
        return;
    }

    for (uint32_t i = 0; i < args->count; i++) {
        SystemEvent ev;
        ev.type = SystemEventType::BLE_FRAME_PARSED;
        ev.tsMs = millis();
        ev.seq  = i;
        args->bus->publish(ev);
        // Always yield one tick; this suite validates contention correctness,
        // not max-throughput publish rate.
        vTaskDelay(1);
    }

    xTaskNotifyGive(args->notifyTask);
    vTaskDelete(NULL);
}

void test_device_bus_cross_core_no_crash() {
    static constexpr uint32_t PRODUCE_COUNT = 200;
    const unsigned long startMs = millis();

    SemaphoreHandle_t startSem = xSemaphoreCreateBinary();
    TEST_ASSERT_NOT_NULL(startSem);

    ProducerArgs args = {&bus, PRODUCE_COUNT, startSem, xTaskGetCurrentTaskHandle()};

    TaskHandle_t producer = NULL;
    BaseType_t created = xTaskCreatePinnedToCore(
        busProducerTask, "bus_prod", 4096, &args, 2, &producer, 0);
    TEST_ASSERT_EQUAL(pdPASS, created);

    // Start producer
    xSemaphoreGive(startSem);

    // Consumer loop on current core
    uint32_t consumed = 0;
    bool producerDone = false;
    unsigned long deadline = millis() + 5000;

    while (millis() < deadline) {
        if (ulTaskNotifyTake(pdTRUE, 0) > 0) {
            producerDone = true;
        }
        SystemEvent ev;
        while (bus.consume(ev)) {
            consumed++;
        }
        if (producerDone && bus.size() == 0) break;
        vTaskDelay(1);
    }

    if (!producerDone && ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100)) > 0) {
        producerDone = true;
    }

    // Drain any remaining
    SystemEvent ev;
    while (bus.consume(ev)) consumed++;

    // Timeout is a hard failure; do not force-delete potentially locked task.
    vSemaphoreDelete(startSem);
    TEST_ASSERT_TRUE_MESSAGE(producerDone, "Producer task did not complete before timeout");

    uint32_t published = bus.getPublishCount();
    uint32_t dropped   = bus.getDropCount();
    uint32_t durationMs = millis() - startMs;

    Serial.printf("  [bus] published=%lu consumed=%lu dropped=%lu\n",
                  (unsigned long)published, (unsigned long)consumed, (unsigned long)dropped);
    deviceTestMetricU32("cross_core_published_total", "cross_core", published, "count");
    deviceTestMetricU32("cross_core_consumed_total", "cross_core", consumed, "count");
    deviceTestMetricU32("cross_core_dropped_total", "cross_core", dropped, "count");
    deviceTestMetricU32("cross_core_duration_ms", "cross_core", durationMs, "ms");

    // All published events should be either consumed or dropped
    TEST_ASSERT_EQUAL_UINT32(published, consumed + dropped);
    TEST_ASSERT_EQUAL_UINT32(PRODUCE_COUNT, published);
}

// ===========================================================================
// RAPID PUBLISH/CONSUME STRESS
// ===========================================================================

void test_device_bus_rapid_publish_consume_stress() {
    static constexpr int ITERATIONS = 1000;

    for (int i = 0; i < ITERATIONS; i++) {
        SystemEvent ev;
        ev.type = (i % 3 == 0) ? SystemEventType::BLE_CONNECTED
                                : SystemEventType::BLE_FRAME_PARSED;
        ev.seq = i;
        bus.publish(ev);

        // Consume every 5th iteration to let queue build up then drain
        if (i % 5 == 4) {
            SystemEvent out;
            while (bus.consume(out)) {}
        }
    }

    // Final drain
    SystemEvent out;
    while (bus.consume(out)) {}
    deviceTestMetricU32("rapid_publish_total", "rapid_stress", bus.getPublishCount(), "count");
    deviceTestMetricU32("rapid_drop_total", "rapid_stress", bus.getDropCount(), "count");

    TEST_ASSERT_EQUAL_UINT32(0, bus.size());
    TEST_ASSERT_EQUAL_UINT32(ITERATIONS, bus.getPublishCount());
}

// ===========================================================================
// CONSUME-BY-TYPE UNDER LOAD
// ===========================================================================

void test_device_bus_consume_by_type_correctness() {
    for (int i = 0; i < 10; i++) {
        SystemEvent ev;
        ev.type = SystemEventType::BLE_FRAME_PARSED;
        ev.seq  = i;
        bus.publish(ev);
    }

    SystemEvent disc;
    disc.type = SystemEventType::BLE_DISCONNECTED;
    disc.seq  = 100;
    bus.publish(disc);

    SystemEvent conn;
    conn.type = SystemEventType::BLE_CONNECTED;
    conn.seq  = 200;
    bus.publish(conn);

    // Selectively consume DISCONNECTED event
    SystemEvent out;
    TEST_ASSERT_TRUE(bus.consumeByType(SystemEventType::BLE_DISCONNECTED, out));
    TEST_ASSERT_EQUAL_UINT32(100, out.seq);

    // Selectively consume CONNECTED event
    TEST_ASSERT_TRUE(bus.consumeByType(SystemEventType::BLE_CONNECTED, out));
    TEST_ASSERT_EQUAL_UINT32(200, out.seq);

    // DISCONNECTED and CONNECTED gone; 10 frame events remain
    TEST_ASSERT_EQUAL_UINT32(10, bus.size());
    TEST_ASSERT_FALSE(bus.consumeByType(SystemEventType::BLE_DISCONNECTED, out));
    deviceTestMetricU32("consume_by_type_remaining_events", "typed_consume", bus.size(), "count");
}

// ===========================================================================
// DUAL PRODUCER CONTENTION
//
// Safety: uses COUNTING semaphore (initial count 0, max 2) so both producers
// can be released reliably with two xSemaphoreGive calls. Each task uses
// bounded semaphore take (2 s). Tasks are killed on timeout.
// ===========================================================================

struct DualProducerArgs {
    SystemEventBus*   bus;
    uint32_t          count;
    SystemEventType   type;
    SemaphoreHandle_t startSem;
    TaskHandle_t      notifyTask;
};

static void dualProducerTask(void* param) {
    DualProducerArgs* args = (DualProducerArgs*)param;
    // Bounded wait — bail if start signal never comes
    if (xSemaphoreTake(args->startSem, pdMS_TO_TICKS(2000)) != pdTRUE) {
        xTaskNotifyGive(args->notifyTask);
        vTaskDelete(NULL);
        return;
    }

    for (uint32_t i = 0; i < args->count; i++) {
        SystemEvent ev;
        ev.type = args->type;
        ev.seq  = i;
        args->bus->publish(ev);
        vTaskDelay(1);
    }

    xTaskNotifyGive(args->notifyTask);
    vTaskDelete(NULL);
}

void test_device_bus_dual_producer_no_corruption() {
    static constexpr uint32_t PER_PRODUCER = 200;
    const unsigned long startMs = millis();

    // COUNTING semaphore: max=2, initial=0 — reliable release for 2 tasks
    SemaphoreHandle_t startSem = xSemaphoreCreateCounting(2, 0);
    TEST_ASSERT_NOT_NULL(startSem);

    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    DualProducerArgs args1 = {&bus, PER_PRODUCER, SystemEventType::BLE_FRAME_PARSED, startSem, self};
    DualProducerArgs args2 = {&bus, PER_PRODUCER, SystemEventType::BLE_DISCONNECTED,      startSem, self};

    TaskHandle_t t1 = NULL, t2 = NULL;
    BaseType_t c1 = xTaskCreatePinnedToCore(dualProducerTask, "prod1", 4096, &args1, 2, &t1, 0);
    BaseType_t c2 = xTaskCreatePinnedToCore(dualProducerTask, "prod2", 4096, &args2, 2, &t2, 1);
    TEST_ASSERT_EQUAL(pdPASS, c1);
    TEST_ASSERT_EQUAL(pdPASS, c2);

    // Let tasks reach their semaphore take
    vTaskDelay(pdMS_TO_TICKS(20));

    // Release both — counting semaphore increments to 2
    xSemaphoreGive(startSem);
    xSemaphoreGive(startSem);

    uint32_t doneSignals = 0;
    unsigned long deadline = millis() + 5000;
    while (millis() < deadline) {
        doneSignals += (uint32_t)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
        if (doneSignals >= 2) break;
    }

    if (doneSignals < 2) {
        doneSignals += (uint32_t)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
    }
    vSemaphoreDelete(startSem);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(2, doneSignals);

    uint32_t published = bus.getPublishCount();
    uint32_t dropped   = bus.getDropCount();
    uint32_t durationMs = millis() - startMs;

    Serial.printf("  [bus] dual-producer: published=%lu dropped=%lu\n",
                  (unsigned long)published, (unsigned long)dropped);

    // Total published should be sum of both producers
    TEST_ASSERT_EQUAL_UINT32(PER_PRODUCER * 2, published);

    // All events should be valid types (no corruption)
    SystemEvent out;
    uint32_t frameCount = 0, disconnectCount = 0, otherCount = 0;
    while (bus.consume(out)) {
        if (out.type == SystemEventType::BLE_FRAME_PARSED) frameCount++;
        else if (out.type == SystemEventType::BLE_DISCONNECTED)  disconnectCount++;
        else otherCount++;
    }
    deviceTestMetricU32("dual_producer_published_total", "dual_producer", published, "count");
    deviceTestMetricU32("dual_producer_dropped_total", "dual_producer", dropped, "count");
    deviceTestMetricU32("dual_producer_other_event_count", "dual_producer", otherCount, "count");
    deviceTestMetricU32("dual_producer_duration_ms", "dual_producer", durationMs, "ms");

    TEST_ASSERT_EQUAL_UINT32(0, otherCount);
    TEST_ASSERT_EQUAL_UINT32(published - dropped, frameCount + disconnectCount);
}

// ===========================================================================
// TEST RUNNER
// ===========================================================================

void setup() {
    if (deviceTestSetup("test_device_event_bus")) return;
    UNITY_BEGIN();

    if (DEVICE_EVENT_BUS_TEST_ID == 0 || DEVICE_EVENT_BUS_TEST_ID == 1) {
        RUN_TEST(test_device_bus_publish_consume);
    }
    if (DEVICE_EVENT_BUS_TEST_ID == 0 || DEVICE_EVENT_BUS_TEST_ID == 2) {
        RUN_TEST(test_device_bus_overflow_drops_frame_first);
    }
    if (DEVICE_EVENT_BUS_TEST_ID == 0 || DEVICE_EVENT_BUS_TEST_ID == 3) {
        RUN_TEST(test_device_bus_cross_core_no_crash);
    }
    if (DEVICE_EVENT_BUS_TEST_ID == 0 || DEVICE_EVENT_BUS_TEST_ID == 4) {
        RUN_TEST(test_device_bus_rapid_publish_consume_stress);
    }
    if (DEVICE_EVENT_BUS_TEST_ID == 0 || DEVICE_EVENT_BUS_TEST_ID == 5) {
        RUN_TEST(test_device_bus_consume_by_type_correctness);
    }
    if (DEVICE_EVENT_BUS_TEST_ID == 0 || DEVICE_EVENT_BUS_TEST_ID == 6) {
        RUN_TEST(test_device_bus_dual_producer_no_corruption);
    }

    UNITY_END();
    deviceTestFinish();
}

void loop() {
    delay(100);  // Keep USB CDC alive after post-test reboot
}

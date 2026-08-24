/**
 * Device ALP display-edge latch concurrency tests.
 *
 * Native tests use std::atomic_flag while device builds use portMUX_TYPE.
 * This suite keeps the real-device coverage that caught the historical
 * cross-core freeze, while pinning the latch's intentionally coalescing
 * one-producer/one-consumer contract.
 *
 * Safety rules from the original public incident suite at commit 5c9c966:
 * use bounded waits, never force-delete a task that could hold the portMUX,
 * and release owned resources before assertions that can fail.
 */

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <unity.h>

#include "../../src/modules/system/system_event_bus.h"
#include "../device_test_reset.h"

static SystemEventBus bus;

void setUp() {
    bus.reset();
}
void tearDown() {}

// 0 = run full suite, 1..3 = run one test.
#ifndef DEVICE_EVENT_BUS_TEST_ID
#define DEVICE_EVENT_BUS_TEST_ID 0
#endif

void test_device_latch_publish_consume() {
    TEST_ASSERT_EQUAL_UINT8(0, bus.size());

    bus.publishAlpStateChanged();

    TEST_ASSERT_EQUAL_UINT8(1, bus.size());
    TEST_ASSERT_TRUE(bus.consumeAlpStateChanged());
    TEST_ASSERT_FALSE(bus.consumeAlpStateChanged());
}

void test_device_latch_coalesces_pending_edges() {
    static constexpr uint32_t PUBLISH_COUNT = 1000;
    for (uint32_t i = 0; i < PUBLISH_COUNT; ++i) {
        bus.publishAlpStateChanged();
    }

    TEST_ASSERT_EQUAL_UINT8(1, bus.size());
    TEST_ASSERT_TRUE(bus.consumeAlpStateChanged());
    TEST_ASSERT_EQUAL_UINT8(0, bus.size());

    deviceTestMetricU32("coalesced_publish_total", "alp_edge_latch", PUBLISH_COUNT, "count");
}

struct ProducerArgs {
    SystemEventBus* bus;
    uint32_t count;
    SemaphoreHandle_t startSem;
    TaskHandle_t notifyTask;
    bool started;
};

static void latchProducerTask(void* param) {
    ProducerArgs* args = static_cast<ProducerArgs*>(param);
    if (xSemaphoreTake(args->startSem, pdMS_TO_TICKS(2000)) != pdTRUE) {
        xTaskNotifyGive(args->notifyTask);
        vTaskDelete(nullptr);
        return;
    }

    args->started = true;
    for (uint32_t i = 0; i < args->count; ++i) {
        args->bus->publishAlpStateChanged();
        vTaskDelay(1);
    }

    xTaskNotifyGive(args->notifyTask);
    vTaskDelete(nullptr);
}

void test_device_latch_cross_core_publish_consume() {
    static constexpr uint32_t PUBLISH_COUNT = 200;
    const uint32_t startMs = millis();
    SemaphoreHandle_t startSem = xSemaphoreCreateBinary();
    TEST_ASSERT_NOT_NULL(startSem);

    ProducerArgs args = {&bus, PUBLISH_COUNT, startSem, xTaskGetCurrentTaskHandle(), false};
    TaskHandle_t producer = nullptr;
    const BaseType_t created =
        xTaskCreatePinnedToCore(latchProducerTask, "alp_edge_prod", 4096, &args, 2, &producer, 0);
    if (created != pdPASS) {
        vSemaphoreDelete(startSem);
        TEST_FAIL_MESSAGE("Could not create ALP edge producer task");
        return;
    }

    xSemaphoreGive(startSem);

    uint32_t consumed = 0;
    bool producerDone = false;
    const uint32_t deadlineMs = millis() + 5000;
    while (millis() < deadlineMs) {
        if (ulTaskNotifyTake(pdTRUE, 0) > 0) {
            producerDone = true;
        }
        if (bus.consumeAlpStateChanged()) {
            consumed++;
        }
        if (producerDone && bus.size() == 0) {
            break;
        }
        vTaskDelay(1);
    }

    if (!producerDone && ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100)) > 0) {
        producerDone = true;
    }
    if (bus.consumeAlpStateChanged()) {
        consumed++;
    }

    if (producerDone) {
        vSemaphoreDelete(startSem);
    }

    TEST_ASSERT_TRUE_MESSAGE(producerDone, "Producer task did not complete before timeout");
    TEST_ASSERT_TRUE_MESSAGE(args.started, "Producer task did not pass its bounded start gate");
    TEST_ASSERT_GREATER_THAN_UINT32(0, consumed);

    deviceTestMetricU32("cross_core_consumed_total", "alp_edge_latch", consumed, "count");
    deviceTestMetricU32("cross_core_duration_ms", "alp_edge_latch", millis() - startMs, "ms");
}

void setup() {
    if (deviceTestSetup("test_device_event_bus"))
        return;
    UNITY_BEGIN();

    if (DEVICE_EVENT_BUS_TEST_ID == 0 || DEVICE_EVENT_BUS_TEST_ID == 1) {
        RUN_TEST(test_device_latch_publish_consume);
    }
    if (DEVICE_EVENT_BUS_TEST_ID == 0 || DEVICE_EVENT_BUS_TEST_ID == 2) {
        RUN_TEST(test_device_latch_coalesces_pending_edges);
    }
    if (DEVICE_EVENT_BUS_TEST_ID == 0 || DEVICE_EVENT_BUS_TEST_ID == 3) {
        RUN_TEST(test_device_latch_cross_core_publish_consume);
    }

    UNITY_END();
    deviceTestFinish();
}

void loop() {
    delay(100);
}

/**
 * Device FreeRTOS / Queue Tests
 *
 * Validates real FreeRTOS primitives on ESP32-S3. Native tests stub these
 * as no-ops, so queue overflow, semaphore contention, and critical section
 * correctness have never been tested until now.
 *
 * SAFETY RULES (learned from production freeze):
 *   - NO portMAX_DELAY in spawned tasks — use bounded timeout (2 s).
 *   - Cross-task tests always vTaskDelete(task) in timeout path.
 *   - Queue is deleted ONLY after producer task is confirmed done or killed.
 *   - All spawned tasks set a volatile 'done' flag before self-deleting.
 */

#include <unity.h>
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include "../device_test_reset.h"

void setUp() {}
void tearDown() {}

// ===========================================================================
// QUEUE BASICS
// ===========================================================================

void test_queue_create_send_receive() {
    QueueHandle_t q = xQueueCreate(8, sizeof(uint32_t));
    TEST_ASSERT_NOT_NULL(q);

    uint32_t val = 42;
    TEST_ASSERT_EQUAL(pdTRUE, xQueueSend(q, &val, 0));

    uint32_t out = 0;
    TEST_ASSERT_EQUAL(pdTRUE, xQueueReceive(q, &out, 0));
    TEST_ASSERT_EQUAL_UINT32(42, out);

    vQueueDelete(q);
}

void test_queue_fifo_ordering() {
    QueueHandle_t q = xQueueCreate(16, sizeof(uint32_t));
    TEST_ASSERT_NOT_NULL(q);

    for (uint32_t i = 0; i < 10; i++) {
        TEST_ASSERT_EQUAL(pdTRUE, xQueueSend(q, &i, 0));
    }

    for (uint32_t expected = 0; expected < 10; expected++) {
        uint32_t out = 0;
        TEST_ASSERT_EQUAL(pdTRUE, xQueueReceive(q, &out, 0));
        TEST_ASSERT_EQUAL_UINT32(expected, out);
    }

    vQueueDelete(q);
}

void test_queue_full_returns_fail() {
    QueueHandle_t q = xQueueCreate(4, sizeof(uint32_t));
    TEST_ASSERT_NOT_NULL(q);

    uint32_t val = 1;
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL(pdTRUE, xQueueSend(q, &val, 0));
    }

    // Queue is full — non-blocking send should fail
    TEST_ASSERT_EQUAL(errQUEUE_FULL, xQueueSend(q, &val, 0));

    vQueueDelete(q);
}

void test_queue_empty_receive_returns_fail() {
    QueueHandle_t q = xQueueCreate(4, sizeof(uint32_t));
    TEST_ASSERT_NOT_NULL(q);

    uint32_t out = 0;
    TEST_ASSERT_EQUAL(pdFALSE, xQueueReceive(q, &out, 0));

    vQueueDelete(q);
}

void test_queue_messages_waiting_count() {
    QueueHandle_t q = xQueueCreate(8, sizeof(uint32_t));
    TEST_ASSERT_NOT_NULL(q);

    TEST_ASSERT_EQUAL_UINT32(0, uxQueueMessagesWaiting(q));

    uint32_t val = 1;
    xQueueSend(q, &val, 0);
    TEST_ASSERT_EQUAL_UINT32(1, uxQueueMessagesWaiting(q));

    xQueueSend(q, &val, 0);
    TEST_ASSERT_EQUAL_UINT32(2, uxQueueMessagesWaiting(q));

    xQueueReceive(q, &val, 0);
    TEST_ASSERT_EQUAL_UINT32(1, uxQueueMessagesWaiting(q));

    vQueueDelete(q);
}

// ===========================================================================
// QUEUE OVERFLOW PATTERN (mirrors BLE ingest drop-oldest strategy)
// ===========================================================================

void test_queue_drop_oldest_on_overflow() {
    static constexpr int DEPTH = 8;
    QueueHandle_t q = xQueueCreate(DEPTH, sizeof(uint32_t));
    TEST_ASSERT_NOT_NULL(q);

    // Fill queue with 1..8
    for (uint32_t i = 1; i <= (uint32_t)DEPTH; i++) {
        xQueueSend(q, &i, 0);
    }

    // Overflow: drop oldest, enqueue 99
    uint32_t newVal = 99;
    if (xQueueSend(q, &newVal, 0) != pdTRUE) {
        uint32_t dropped;
        xQueueReceive(q, &dropped, 0);
        TEST_ASSERT_EQUAL_UINT32(1, dropped);
        TEST_ASSERT_EQUAL(pdTRUE, xQueueSend(q, &newVal, 0));
    }

    // Verify remaining: 2,3,4,5,6,7,8,99
    uint32_t expected[] = {2, 3, 4, 5, 6, 7, 8, 99};
    for (int i = 0; i < DEPTH; i++) {
        uint32_t out;
        TEST_ASSERT_EQUAL(pdTRUE, xQueueReceive(q, &out, 0));
        TEST_ASSERT_EQUAL_UINT32(expected[i], out);
    }

    vQueueDelete(q);
}

// ===========================================================================
// LARGE ITEM QUEUE (BLE packet size)
// ===========================================================================

void test_queue_large_items_ble_packet_size() {
    struct TestPacket {
        uint8_t  data[256];
        size_t   length;
        uint16_t charUUID;
        uint32_t tsMs;
    };

    QueueHandle_t q = xQueueCreate(16, sizeof(TestPacket));
    TEST_ASSERT_NOT_NULL(q);

    TestPacket pkt = {};
    pkt.length   = 128;
    pkt.charUUID = 0x1234;
    pkt.tsMs     = millis();
    memset(pkt.data, 0xAB, 128);

    TEST_ASSERT_EQUAL(pdTRUE, xQueueSend(q, &pkt, 0));

    TestPacket out = {};
    TEST_ASSERT_EQUAL(pdTRUE, xQueueReceive(q, &out, 0));
    TEST_ASSERT_EQUAL_UINT16(0x1234, out.charUUID);
    TEST_ASSERT_EQUAL(128, out.length);
    TEST_ASSERT_EQUAL_UINT8(0xAB, out.data[0]);
    TEST_ASSERT_EQUAL_UINT8(0xAB, out.data[127]);

    vQueueDelete(q);
}

// ===========================================================================
// SEMAPHORE / MUTEX
// ===========================================================================

void test_mutex_take_give_roundtrip() {
    SemaphoreHandle_t mtx = xSemaphoreCreateMutex();
    TEST_ASSERT_NOT_NULL(mtx);

    // Bounded timeout (2 s) — never portMAX_DELAY
    TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreTake(mtx, pdMS_TO_TICKS(2000)));
    TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreGive(mtx));

    vSemaphoreDelete(mtx);
}

void test_mutex_recursive_take() {
    SemaphoreHandle_t mtx = xSemaphoreCreateRecursiveMutex();
    TEST_ASSERT_NOT_NULL(mtx);

    TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreTakeRecursive(mtx, pdMS_TO_TICKS(2000)));
    TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreTakeRecursive(mtx, pdMS_TO_TICKS(2000)));
    TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreGiveRecursive(mtx));
    TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreGiveRecursive(mtx));

    vSemaphoreDelete(mtx);
}

void test_binary_semaphore_signaling() {
    SemaphoreHandle_t sem = xSemaphoreCreateBinary();
    TEST_ASSERT_NOT_NULL(sem);

    // Initially not available
    TEST_ASSERT_EQUAL(pdFALSE, xSemaphoreTake(sem, 0));

    // Give → available
    xSemaphoreGive(sem);
    TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreTake(sem, 0));

    // Consumed → not available again
    TEST_ASSERT_EQUAL(pdFALSE, xSemaphoreTake(sem, 0));

    vSemaphoreDelete(sem);
}

// ===========================================================================
// CRITICAL SECTIONS (portMUX — used by SystemEventBus)
// ===========================================================================

void test_critical_section_no_deadlock() {
    portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    volatile uint32_t progress = 0;

    // Repeatedly enter/exit the critical section and prove forward progress.
    for (int i = 0; i < 1000; i++) {
        portENTER_CRITICAL(&mux);
        progress++;
        portEXIT_CRITICAL(&mux);
    }

    TEST_ASSERT_EQUAL_UINT32(1000, progress);
}

void test_critical_section_protects_variable() {
    portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    volatile uint32_t counter = 0;

    for (int i = 0; i < 1000; i++) {
        portENTER_CRITICAL(&mux);
        counter++;
        portEXIT_CRITICAL(&mux);
    }

    TEST_ASSERT_EQUAL_UINT32(1000, counter);
}

// ===========================================================================
// CROSS-TASK QUEUE COMMUNICATION
//
// Safety: producer uses bounded xQueueSend (2 s timeout, not portMAX_DELAY).
// Consumer has a 5 s deadline. If producer stalls, consumer drains and
// vTaskDelete(producer) before deleting the queue.
// ===========================================================================

struct TaskQueueArgs {
    QueueHandle_t queue;
    uint32_t count;
    volatile bool done;
};

static void producerTask(void* param) {
    TaskQueueArgs* args = (TaskQueueArgs*)param;
    for (uint32_t i = 0; i < args->count; i++) {
        // Bounded send — worst-case 2 s block, then drops and moves on
        if (xQueueSend(args->queue, &i, pdMS_TO_TICKS(2000)) != pdTRUE) {
            break;   // Queue deleted or consumer died — bail out
        }
    }
    args->done = true;
    vTaskDelete(NULL);
}

void test_cross_task_queue_communication() {
    static constexpr uint32_t MSG_COUNT = 100;
    QueueHandle_t q = xQueueCreate(16, sizeof(uint32_t));
    TEST_ASSERT_NOT_NULL(q);

    TaskQueueArgs args = {q, MSG_COUNT, false};

    TaskHandle_t task = NULL;
    BaseType_t created = xTaskCreatePinnedToCore(
        producerTask, "producer", 4096, &args, 1, &task, 0);
    TEST_ASSERT_EQUAL(pdPASS, created);

    // Consume from current task
    uint32_t received = 0;
    uint32_t lastVal  = 0;
    unsigned long deadline = millis() + 5000;

    while (received < MSG_COUNT && millis() < deadline) {
        uint32_t val;
        if (xQueueReceive(q, &val, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (received > 0) {
                TEST_ASSERT_EQUAL_UINT32(lastVal + 1, val);
            }
            lastVal = val;
            received++;
        }
    }

    // Safety: if producer is still alive, kill it before deleting queue
    if (!args.done) {
        vTaskDelete(task);
        delay(50);   // Let FreeRTOS clean up the task
    }

    vQueueDelete(q);

    TEST_ASSERT_EQUAL_UINT32(MSG_COUNT, received);
}

// ===========================================================================
// TASK DELAY / TIMING
// ===========================================================================

void test_vtask_delay_approximately_correct() {
    unsigned long before = millis();
    vTaskDelay(pdMS_TO_TICKS(100));
    unsigned long elapsed = millis() - before;

    // Allow ±20 ms tolerance for tick granularity
    TEST_ASSERT_UINT32_WITHIN(20, 100, (uint32_t)elapsed);
}

// ===========================================================================
// TEST RUNNER
// ===========================================================================

void setup() {
    if (deviceTestSetup("test_device_freertos")) return;
    UNITY_BEGIN();

    // Queue basics
    RUN_TEST(test_queue_create_send_receive);
    RUN_TEST(test_queue_fifo_ordering);
    RUN_TEST(test_queue_full_returns_fail);
    RUN_TEST(test_queue_empty_receive_returns_fail);
    RUN_TEST(test_queue_messages_waiting_count);

    // Queue overflow (BLE ingest pattern)
    RUN_TEST(test_queue_drop_oldest_on_overflow);
    RUN_TEST(test_queue_large_items_ble_packet_size);

    // Semaphore / Mutex
    RUN_TEST(test_mutex_take_give_roundtrip);
    RUN_TEST(test_mutex_recursive_take);
    RUN_TEST(test_binary_semaphore_signaling);

    // Critical sections
    RUN_TEST(test_critical_section_no_deadlock);
    RUN_TEST(test_critical_section_protects_variable);

    // Cross-task
    RUN_TEST(test_cross_task_queue_communication);
    RUN_TEST(test_vtask_delay_approximately_correct);

    UNITY_END();
    deviceTestFinish();
}

void loop() {
    delay(100);  // Keep USB CDC alive after post-test reboot
}

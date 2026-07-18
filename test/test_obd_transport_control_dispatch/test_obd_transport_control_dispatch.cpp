#include <unity.h>

#include <cstring>

#include "../../src/modules/obd/obd_transport_control_dispatch.h"

namespace {

struct Request {
    uint32_t requestId = 0;
    uint32_t nowMs = 0;
    uint32_t dispatchEpoch = 0;
    bool deleteBond = false;
    char address[18] = {};
    uint8_t addrType = 0;
};

struct FakeClient {
    uint32_t generation = 3;
    bool downConfirmed = false;
    bool disconnectAccepted = true;
    bool deleteBondResult = true;
    uint32_t serviceCalls = 0;
    uint32_t disconnectCalls = 0;
    uint32_t deleteBondCalls = 0;

    void serviceDeferredLinkState() { serviceCalls++; }
    uint32_t activeLinkGeneration() const { return generation; }
    bool linkDownConfirmed(uint32_t target) const { return downConfirmed && target == generation; }
    bool disconnect() {
        disconnectCalls++;
        return disconnectAccepted;
    }
    bool deleteBond(const char*, uint8_t) {
        deleteBondCalls++;
        return deleteBondResult;
    }
};

QueueHandle_t controlQueue = nullptr;
QueueHandle_t requestQueue = nullptr;

} // namespace

void setUp() {
    controlQueue = xQueueCreate(1, sizeof(Request));
    requestQueue = xQueueCreate(1, sizeof(Request));
}

void tearDown() {
    vQueueDelete(controlQueue);
    vQueueDelete(requestQueue);
}

void test_control_purges_data_and_waits_for_confirmed_disconnect() {
    Request data{};
    data.requestId = 11;
    TEST_ASSERT_EQUAL(pdTRUE, xQueueSend(requestQueue, &data, 0));

    Request control{};
    control.requestId = 22;
    control.nowMs = 900;
    control.deleteBond = true;
    std::strcpy(control.address, "A4:C1:38:00:11:22");
    control.addrType = 1;
    TEST_ASSERT_EQUAL(pdTRUE, xQueueSend(controlQueue, &control, 0));

    FakeClient client;
    ObdTransportControlDispatch<Request> dispatch;
    uint32_t acknowledgements = 0;
    bool ackBondDeleted = false;
    auto acknowledge = [&](const Request& ack, bool attempted, bool deleted, bool success, bool timedOut) {
        acknowledgements++;
        TEST_ASSERT_EQUAL_UINT32(22, ack.requestId);
        TEST_ASSERT_EQUAL_UINT32(900, ack.nowMs);
        TEST_ASSERT_TRUE(attempted);
        TEST_ASSERT_TRUE(success);
        TEST_ASSERT_FALSE(timedOut);
        ackBondDeleted = deleted;
    };

    TEST_ASSERT_TRUE(dispatch.service(controlQueue, requestQueue, client, acknowledge));
    TEST_ASSERT_EQUAL_UINT32(0, uxQueueMessagesWaiting(requestQueue));
    TEST_ASSERT_EQUAL_UINT32(1, client.deleteBondCalls);
    TEST_ASSERT_EQUAL_UINT32(1, client.disconnectCalls);
    TEST_ASSERT_EQUAL_UINT32(0, acknowledgements);

    TEST_ASSERT_TRUE(dispatch.service(controlQueue, requestQueue, client, acknowledge));
    TEST_ASSERT_EQUAL_UINT32(0, acknowledgements);

    client.downConfirmed = true;
    TEST_ASSERT_TRUE(dispatch.service(controlQueue, requestQueue, client, acknowledge));
    TEST_ASSERT_EQUAL_UINT32(0, acknowledgements);
    TEST_ASSERT_TRUE(dispatch.service(controlQueue, requestQueue, client, acknowledge));
    TEST_ASSERT_EQUAL_UINT32(1, acknowledgements);
    TEST_ASSERT_TRUE(ackBondDeleted);
    TEST_ASSERT_FALSE(dispatch.active());
}

void test_rejected_disconnect_retries_without_false_ack() {
    Request control{};
    TEST_ASSERT_EQUAL(pdTRUE, xQueueSend(controlQueue, &control, 0));

    FakeClient client;
    client.disconnectAccepted = false;
    ObdTransportControlDispatch<Request> dispatch;
    uint32_t acknowledgements = 0;
    auto acknowledge = [&](const Request&, bool, bool, bool, bool) { acknowledgements++; };

    TEST_ASSERT_TRUE(dispatch.service(controlQueue, requestQueue, client, acknowledge));
    TEST_ASSERT_EQUAL_UINT32(1, client.disconnectCalls);
    TEST_ASSERT_EQUAL_UINT32(0, acknowledgements);

    client.disconnectAccepted = true;
    TEST_ASSERT_TRUE(dispatch.service(controlQueue, requestQueue, client, acknowledge));
    TEST_ASSERT_EQUAL_UINT32(2, client.disconnectCalls);
    TEST_ASSERT_EQUAL_UINT32(0, acknowledgements);

    client.downConfirmed = true;
    TEST_ASSERT_TRUE(dispatch.service(controlQueue, requestQueue, client, acknowledge));
    TEST_ASSERT_TRUE(dispatch.service(controlQueue, requestQueue, client, acknowledge));
    TEST_ASSERT_EQUAL_UINT32(1, acknowledgements);
}

void test_persistent_command_rejection_publishes_bounded_failure() {
    Request control{};
    TEST_ASSERT_EQUAL(pdTRUE, xQueueSend(controlQueue, &control, 0));

    FakeClient client;
    client.disconnectAccepted = false;
    ObdTransportControlDispatch<Request> dispatch;
    uint32_t acknowledgements = 0;
    bool success = true;
    bool timedOut = true;
    auto acknowledge = [&](const Request&, bool, bool, bool ackSuccess, bool ackTimedOut) {
        acknowledgements++;
        success = ackSuccess;
        timedOut = ackTimedOut;
    };

    for (uint8_t attempt = 0; attempt < ObdTransportControlDispatch<Request>::MAX_COMMAND_ATTEMPTS; ++attempt) {
        TEST_ASSERT_TRUE(dispatch.service(controlQueue, requestQueue, client, acknowledge));
    }

    TEST_ASSERT_EQUAL_UINT32(3, client.disconnectCalls);
    TEST_ASSERT_EQUAL_UINT32(1, acknowledgements);
    TEST_ASSERT_FALSE(success);
    TEST_ASSERT_FALSE(timedOut);
    TEST_ASSERT_FALSE(dispatch.active());
}

void test_missing_link_down_confirmation_publishes_bounded_timeout() {
    Request control{};
    TEST_ASSERT_EQUAL(pdTRUE, xQueueSend(controlQueue, &control, 0));

    FakeClient client;
    ObdTransportControlDispatch<Request> dispatch;
    uint32_t acknowledgements = 0;
    bool success = true;
    bool timedOut = false;
    auto acknowledge = [&](const Request&, bool, bool, bool ackSuccess, bool ackTimedOut) {
        acknowledgements++;
        success = ackSuccess;
        timedOut = ackTimedOut;
    };

    TEST_ASSERT_TRUE(dispatch.service(controlQueue, requestQueue, client, acknowledge));
    for (uint16_t pass = 0; pass < ObdTransportControlDispatch<Request>::MAX_CONFIRM_PASSES; ++pass) {
        TEST_ASSERT_TRUE(dispatch.service(controlQueue, requestQueue, client, acknowledge));
    }

    TEST_ASSERT_EQUAL_UINT32(1, acknowledgements);
    TEST_ASSERT_FALSE(success);
    TEST_ASSERT_TRUE(timedOut);
    TEST_ASSERT_FALSE(dispatch.active());
}

void test_disconnect_epoch_invalidates_unclaimed_work_but_preserves_claimed_work() {
    ObdTransportRequestEpoch epoch;
    const uint32_t stale = epoch.snapshot();
    epoch.cancelQueuedWork();
    TEST_ASSERT_FALSE(epoch.tryClaim(stale));

    const uint32_t current = epoch.snapshot();
    TEST_ASSERT_TRUE(epoch.tryClaim(current));

    // The request is now formally in flight. A newer disconnect epoch must
    // survive release so the next outgoing-session request cannot claim it.
    epoch.cancelQueuedWork();
    epoch.releaseClaim();
    TEST_ASSERT_FALSE(epoch.tryClaim(current));
    TEST_ASSERT_TRUE(epoch.tryClaim(epoch.snapshot()));
    epoch.releaseClaim();
}

void test_empty_control_queue_leaves_data_queue_untouched() {
    Request data{};
    TEST_ASSERT_EQUAL(pdTRUE, xQueueSend(requestQueue, &data, 0));

    FakeClient client;
    ObdTransportControlDispatch<Request> dispatch;
    auto acknowledge = [](const Request&, bool, bool, bool, bool) {};

    TEST_ASSERT_FALSE(dispatch.service(controlQueue, requestQueue, client, acknowledge));
    TEST_ASSERT_EQUAL_UINT32(1, uxQueueMessagesWaiting(requestQueue));
    TEST_ASSERT_EQUAL_UINT32(0, client.disconnectCalls);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_control_purges_data_and_waits_for_confirmed_disconnect);
    RUN_TEST(test_rejected_disconnect_retries_without_false_ack);
    RUN_TEST(test_persistent_command_rejection_publishes_bounded_failure);
    RUN_TEST(test_missing_link_down_confirmation_publishes_bounded_timeout);
    RUN_TEST(test_disconnect_epoch_invalidates_unclaimed_work_but_preserves_claimed_work);
    RUN_TEST(test_empty_control_queue_leaves_data_queue_untouched);
    return UNITY_END();
}

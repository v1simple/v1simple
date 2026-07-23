#include <unity.h>

#include "../../src/modules/ble/ble_proxy_epoch_observer.h"

void setUp() {}
void tearDown() {}

void test_gate_fails_closed_and_reenable_uses_a_new_epoch() {
    BleProxyEpochObserver observer;

    TEST_ASSERT_FALSE(observer.accepts(1));
    observer.open(1);
    TEST_ASSERT_TRUE(observer.accepts(1));
    observer.close();
    TEST_ASSERT_FALSE(observer.accepts(1));
    observer.open(2);
    TEST_ASSERT_FALSE(observer.accepts(1));
    TEST_ASSERT_TRUE(observer.accepts(2));

    const BleProxyEpochObserverSnapshot snapshot = observer.snapshot();
    TEST_ASSERT_EQUAL_UINT32(2, snapshot.currentEpoch);
    TEST_ASSERT_EQUAL_UINT32(2, snapshot.admittedEpoch);
    TEST_ASSERT_EQUAL_UINT32(2, snapshot.allocationCount);
    TEST_ASSERT_EQUAL_UINT32(1, snapshot.disableCount);
    TEST_ASSERT_EQUAL_UINT32(1, snapshot.reenableCount);
}

void test_callback_crossing_close_is_observed_without_holding_it_in_production() {
    BleProxyEpochObserver observer;
    observer.open(7);
    {
        BleProxyEpochObserver::CallbackLease lease(observer, BleProxyCallbackDirection::V1ToProxy, 7);
        TEST_ASSERT_EQUAL_UINT32(1, observer.snapshot().activeCallbacks);
        observer.close();
        TEST_ASSERT_FALSE(observer.snapshot().activeCallbackObserved);
    }

    const BleProxyEpochObserverSnapshot snapshot = observer.snapshot();
    TEST_ASSERT_EQUAL_UINT32(0, snapshot.activeCallbacks);
    TEST_ASSERT_EQUAL_UINT32(1, snapshot.v1ToProxyCallbackEntries);
    TEST_ASSERT_TRUE(snapshot.activeCallbackObserved);
}

void test_callback_while_gate_is_already_closed_is_not_a_false_overlap() {
    BleProxyEpochObserver observer;
    {
        BleProxyEpochObserver::CallbackLease lease(observer, BleProxyCallbackDirection::V1ToProxy, 0);
    }

    TEST_ASSERT_FALSE(observer.snapshot().activeCallbackObserved);
}

void test_release_opportunity_requires_a_natural_active_callback() {
    BleProxyEpochObserver observer;
    observer.open(9);
    observer.noteReleaseTryLockFailed(BleProxyCallbackDirection::ProxyToV1);
    TEST_ASSERT_FALSE(observer.snapshot().releaseOpportunityObserved);

    {
        BleProxyEpochObserver::CallbackLease lease(observer, BleProxyCallbackDirection::ProxyToV1, 9);
        observer.noteReleaseTryLockFailed(BleProxyCallbackDirection::ProxyToV1);
        TEST_ASSERT_FALSE(observer.snapshot().releaseOpportunityObserved);
        observer.noteQueueLockAcquired(BleProxyCallbackDirection::ProxyToV1);
        observer.noteReleaseTryLockFailed(BleProxyCallbackDirection::ProxyToV1);
        observer.noteQueueLockReleased(BleProxyCallbackDirection::ProxyToV1);
    }
    TEST_ASSERT_TRUE(observer.snapshot().releaseOpportunityObserved);
}

void test_old_epoch_rejection_and_fresh_bidirectional_admission_are_distinct() {
    BleProxyEpochObserver observer;
    observer.open(11);
    observer.close();
    observer.open(12);

    observer.noteAdmission(BleProxyCallbackDirection::V1ToProxy, 11, false);
    observer.noteAdmission(BleProxyCallbackDirection::ProxyToV1, 11, false);
    observer.noteAdmission(BleProxyCallbackDirection::V1ToProxy, 12, true);
    observer.noteAdmission(BleProxyCallbackDirection::ProxyToV1, 12, true);

    const BleProxyEpochObserverSnapshot snapshot = observer.snapshot();
    TEST_ASSERT_EQUAL_UINT32(1, snapshot.staleV1ToProxyRejections);
    TEST_ASSERT_EQUAL_UINT32(1, snapshot.staleProxyToV1Rejections);
    TEST_ASSERT_EQUAL_UINT32(1, snapshot.v1ToProxyAdmissions);
    TEST_ASSERT_EQUAL_UINT32(1, snapshot.proxyToV1Admissions);
    TEST_ASSERT_FALSE(snapshot.oldEpochForwarded);
}

void test_admitting_a_noncurrent_epoch_latches_corruption_evidence() {
    BleProxyEpochObserver observer;
    observer.open(20);
    observer.noteAdmission(BleProxyCallbackDirection::V1ToProxy, 19, true);

    TEST_ASSERT_TRUE(observer.snapshot().oldEpochForwarded);
}

int main(int argc, char** argv) {
    UNITY_BEGIN();
    RUN_TEST(test_gate_fails_closed_and_reenable_uses_a_new_epoch);
    RUN_TEST(test_callback_crossing_close_is_observed_without_holding_it_in_production);
    RUN_TEST(test_callback_while_gate_is_already_closed_is_not_a_false_overlap);
    RUN_TEST(test_release_opportunity_requires_a_natural_active_callback);
    RUN_TEST(test_old_epoch_rejection_and_fresh_bidirectional_admission_are_distinct);
    RUN_TEST(test_admitting_a_noncurrent_epoch_latches_corruption_evidence);
    return UNITY_END();
}

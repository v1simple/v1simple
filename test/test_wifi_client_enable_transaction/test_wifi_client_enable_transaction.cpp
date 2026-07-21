#include <unity.h>

#include "../../src/modules/wifi/wifi_client_enable_transaction.h"
#ifndef V1_LINKED_TEST_WIFI_CLIENT_ENABLE_TRANSACTION
#include "../../src/modules/wifi/wifi_client_enable_transaction.cpp"
#endif

namespace {

struct FakeRuntime {
    bool enabled = false;
    bool startResult = true;
    int sequence = 0;
    int startOrder = 0;
    int rollbackOrder = 0;
    int commitOrder = 0;
    int startCalls = 0;
    int rollbackCalls = 0;
    int commitCalls = 0;
    int logicalState = 0;
    int priorLogicalState = 0;
};

bool attemptStart(void* ctx) {
    auto* fake = static_cast<FakeRuntime*>(ctx);
    fake->startCalls++;
    fake->startOrder = ++fake->sequence;
    fake->logicalState = 1;
    return fake->startResult;
}

void rollbackFailedStart(void* ctx) {
    auto* fake = static_cast<FakeRuntime*>(ctx);
    fake->rollbackCalls++;
    fake->rollbackOrder = ++fake->sequence;
    fake->logicalState = fake->priorLogicalState;
}

void commitEnabled(void* ctx) {
    auto* fake = static_cast<FakeRuntime*>(ctx);
    fake->commitCalls++;
    fake->commitOrder = ++fake->sequence;
    fake->enabled = true;
}

WifiClientEnableTransaction::Runtime makeRuntime(FakeRuntime& fake) {
    WifiClientEnableTransaction::Runtime runtime;
    runtime.ctx = &fake;
    runtime.attemptStart = attemptStart;
    runtime.rollbackFailedStart = rollbackFailedStart;
    runtime.commitEnabled = commitEnabled;
    return runtime;
}

void test_failed_start_does_not_commit_disabled_state() {
    FakeRuntime fake;
    fake.startResult = false;

    TEST_ASSERT_FALSE(WifiClientEnableTransaction::execute(makeRuntime(fake)));
    TEST_ASSERT_EQUAL_INT(1, fake.startCalls);
    TEST_ASSERT_EQUAL_INT(1, fake.rollbackCalls);
    TEST_ASSERT_EQUAL_INT(0, fake.commitCalls);
    TEST_ASSERT_FALSE(fake.enabled);
    TEST_ASSERT_EQUAL_INT(0, fake.logicalState);
    TEST_ASSERT_EQUAL_INT(1, fake.startOrder);
    TEST_ASSERT_EQUAL_INT(2, fake.rollbackOrder);
}

void test_failed_reenable_preserves_prior_enabled_state() {
    FakeRuntime fake;
    fake.enabled = true;
    fake.startResult = false;
    fake.logicalState = 3;
    fake.priorLogicalState = 3;
    auto runtime = makeRuntime(fake);
    runtime.persistedEnabled = true;

    TEST_ASSERT_FALSE(WifiClientEnableTransaction::execute(runtime));
    TEST_ASSERT_EQUAL_INT(1, fake.startCalls);
    TEST_ASSERT_EQUAL_INT(1, fake.rollbackCalls);
    TEST_ASSERT_EQUAL_INT(0, fake.commitCalls);
    TEST_ASSERT_TRUE(fake.enabled);
    TEST_ASSERT_EQUAL_INT(3, fake.logicalState);
}

void test_persisted_but_not_admitted_retries_without_recommit() {
    FakeRuntime fake;
    fake.enabled = true;
    auto runtime = makeRuntime(fake);
    runtime.persistedEnabled = true;

    TEST_ASSERT_TRUE(WifiClientEnableTransaction::execute(runtime));
    TEST_ASSERT_EQUAL_INT(1, fake.startCalls);
    TEST_ASSERT_EQUAL_INT(0, fake.rollbackCalls);
    TEST_ASSERT_EQUAL_INT(0, fake.commitCalls);
    TEST_ASSERT_TRUE(fake.enabled);
}

void test_persisted_and_admitted_is_idempotent() {
    FakeRuntime fake;
    fake.enabled = true;
    auto runtime = makeRuntime(fake);
    runtime.persistedEnabled = true;
    runtime.lifecycleAdmitted = true;

    TEST_ASSERT_TRUE(WifiClientEnableTransaction::execute(runtime));
    TEST_ASSERT_EQUAL_INT(0, fake.startCalls);
    TEST_ASSERT_EQUAL_INT(0, fake.rollbackCalls);
    TEST_ASSERT_EQUAL_INT(0, fake.commitCalls);
    TEST_ASSERT_TRUE(fake.enabled);
}

void test_success_commits_once_after_start_admission() {
    FakeRuntime fake;

    TEST_ASSERT_TRUE(WifiClientEnableTransaction::execute(makeRuntime(fake)));
    TEST_ASSERT_TRUE(fake.enabled);
    TEST_ASSERT_EQUAL_INT(1, fake.startCalls);
    TEST_ASSERT_EQUAL_INT(0, fake.rollbackCalls);
    TEST_ASSERT_EQUAL_INT(1, fake.commitCalls);
    TEST_ASSERT_EQUAL_INT(1, fake.startOrder);
    TEST_ASSERT_EQUAL_INT(2, fake.commitOrder);
}

void test_missing_commit_callback_fails_before_start_side_effects() {
    FakeRuntime fake;
    auto runtime = makeRuntime(fake);
    runtime.commitEnabled = nullptr;

    TEST_ASSERT_FALSE(WifiClientEnableTransaction::execute(runtime));
    TEST_ASSERT_EQUAL_INT(0, fake.startCalls);
    TEST_ASSERT_EQUAL_INT(0, fake.rollbackCalls);
    TEST_ASSERT_EQUAL_INT(0, fake.commitCalls);
}

void test_missing_start_callback_fails_without_commit() {
    FakeRuntime fake;
    auto runtime = makeRuntime(fake);
    runtime.attemptStart = nullptr;

    TEST_ASSERT_FALSE(WifiClientEnableTransaction::execute(runtime));
    TEST_ASSERT_EQUAL_INT(0, fake.startCalls);
    TEST_ASSERT_EQUAL_INT(0, fake.rollbackCalls);
    TEST_ASSERT_EQUAL_INT(0, fake.commitCalls);
}

void test_missing_rollback_callback_fails_before_start_side_effects() {
    FakeRuntime fake;
    auto runtime = makeRuntime(fake);
    runtime.rollbackFailedStart = nullptr;

    TEST_ASSERT_FALSE(WifiClientEnableTransaction::execute(runtime));
    TEST_ASSERT_EQUAL_INT(0, fake.startCalls);
    TEST_ASSERT_EQUAL_INT(0, fake.rollbackCalls);
    TEST_ASSERT_EQUAL_INT(0, fake.commitCalls);
}

} // namespace

void setUp() {}
void tearDown() {}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_failed_start_does_not_commit_disabled_state);
    RUN_TEST(test_failed_reenable_preserves_prior_enabled_state);
    RUN_TEST(test_persisted_but_not_admitted_retries_without_recommit);
    RUN_TEST(test_persisted_and_admitted_is_idempotent);
    RUN_TEST(test_success_commits_once_after_start_admission);
    RUN_TEST(test_missing_commit_callback_fails_before_start_side_effects);
    RUN_TEST(test_missing_start_callback_fails_without_commit);
    RUN_TEST(test_missing_rollback_callback_fails_before_start_side_effects);
    return UNITY_END();
}

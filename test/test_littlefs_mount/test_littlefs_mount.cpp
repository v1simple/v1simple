#include <unity.h>

#include "../mocks/Arduino.h"
#include "../mocks/LittleFS.h"

#include "../../include/littlefs_mount.h"
#include "../../src/littlefs_mount.cpp"

void setUp() {
    LittleFSClass::resetBeginRecording();
}

void tearDown() {}

void test_mount_storage_uses_storage_partition_contract() {
    TEST_ASSERT_TRUE(fsmount::mountStorage());

    TEST_ASSERT_EQUAL_INT(1, LittleFSClass::beginCallCount);
    TEST_ASSERT_FALSE(LittleFSClass::lastFormatOnFail);
    TEST_ASSERT_EQUAL_STRING("/littlefs", LittleFSClass::lastBasePath.c_str());
    TEST_ASSERT_EQUAL_UINT8(10, LittleFSClass::lastMaxOpenFiles);
    TEST_ASSERT_EQUAL_STRING("storage", LittleFSClass::lastPartitionLabel.c_str());
}

void test_mount_storage_returns_mount_failure() {
    LittleFSClass::beginReturnValue = false;

    TEST_ASSERT_FALSE(fsmount::mountStorage());
    TEST_ASSERT_EQUAL_INT(1, LittleFSClass::beginCallCount);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_mount_storage_uses_storage_partition_contract);
    RUN_TEST(test_mount_storage_returns_mount_failure);
    return UNITY_END();
}

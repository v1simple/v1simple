#include <unity.h>

#include "../../src/modules/wifi/wifi_static_path_guard.h"
#include "../../src/modules/wifi/wifi_static_path_guard.cpp"  // Pull implementation for UNIT_TEST.

void setUp() {}
void tearDown() {}

void test_accepts_normal_static_paths() {
    TEST_ASSERT_TRUE(WifiStaticPathGuard::isSafe("/"));
    TEST_ASSERT_TRUE(WifiStaticPathGuard::isSafe("/index.html"));
    TEST_ASSERT_TRUE(WifiStaticPathGuard::isSafe("/settings"));
    TEST_ASSERT_TRUE(WifiStaticPathGuard::isSafe("/_app/immutable/start-abc123.js"));
    TEST_ASSERT_TRUE(WifiStaticPathGuard::isSafe("/notes..txt"));
}

void test_rejects_missing_or_relative_paths() {
    TEST_ASSERT_FALSE(WifiStaticPathGuard::isSafe(nullptr));
    TEST_ASSERT_FALSE(WifiStaticPathGuard::isSafe(""));
    TEST_ASSERT_FALSE(WifiStaticPathGuard::isSafe("index.html"));
}

void test_rejects_parent_directory_segments() {
    TEST_ASSERT_FALSE(WifiStaticPathGuard::isSafe("/.."));
    TEST_ASSERT_FALSE(WifiStaticPathGuard::isSafe("/../panic.txt"));
    TEST_ASSERT_FALSE(WifiStaticPathGuard::isSafe("/_app/../../panic.txt"));
    TEST_ASSERT_FALSE(WifiStaticPathGuard::isSafe("/_app/../env.js"));
    TEST_ASSERT_FALSE(WifiStaticPathGuard::isSafe("/foo/../bar.js"));
}

void test_rejects_backslash_separators() {
    TEST_ASSERT_FALSE(WifiStaticPathGuard::isSafe("/_app\\env.js"));
    TEST_ASSERT_FALSE(WifiStaticPathGuard::isSafe("/_app\\..\\panic.txt"));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_accepts_normal_static_paths);
    RUN_TEST(test_rejects_missing_or_relative_paths);
    RUN_TEST(test_rejects_parent_directory_segments);
    RUN_TEST(test_rejects_backslash_separators);
    return UNITY_END();
}

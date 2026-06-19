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

void test_allows_shipped_ui_pages_and_assets() {
    TEST_ASSERT_TRUE(WifiStaticPathGuard::isAllowedServedPath("/"));
    TEST_ASSERT_TRUE(WifiStaticPathGuard::isAllowedServedPath("/index.html"));
    TEST_ASSERT_TRUE(WifiStaticPathGuard::isAllowedServedPath("/settings"));
    TEST_ASSERT_TRUE(WifiStaticPathGuard::isAllowedServedPath("/settings.html"));
    TEST_ASSERT_TRUE(WifiStaticPathGuard::isAllowedServedPath("/_app/env.js"));
    TEST_ASSERT_TRUE(WifiStaticPathGuard::isAllowedServedPath("/_app/version.json"));
    TEST_ASSERT_TRUE(WifiStaticPathGuard::isAllowedServedPath("/_app/immutable/entry/start-abc123.js"));
    TEST_ASSERT_TRUE(WifiStaticPathGuard::isAllowedServedPath("/audio/band_ka.mul"));
    TEST_ASSERT_TRUE(WifiStaticPathGuard::isAllowedServedPath("/branding/v1simple-logo-transparent.png"));
}

void test_rejects_unlisted_littlefs_paths() {
    TEST_ASSERT_FALSE(WifiStaticPathGuard::isAllowedServedPath("/secret.json"));
    TEST_ASSERT_FALSE(WifiStaticPathGuard::isAllowedServedPath("/_app/secret.json"));
    TEST_ASSERT_FALSE(WifiStaticPathGuard::isAllowedServedPath("/audio/secret.txt"));
    TEST_ASSERT_FALSE(WifiStaticPathGuard::isAllowedServedPath("/audio/nested/band_ka.mul"));
    TEST_ASSERT_FALSE(WifiStaticPathGuard::isAllowedServedPath("/branding/secret.txt"));
    TEST_ASSERT_FALSE(WifiStaticPathGuard::isAllowedServedPath("/dev"));
    TEST_ASSERT_FALSE(WifiStaticPathGuard::isAllowedServedPath("/../secret.json"));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_accepts_normal_static_paths);
    RUN_TEST(test_rejects_missing_or_relative_paths);
    RUN_TEST(test_rejects_parent_directory_segments);
    RUN_TEST(test_rejects_backslash_separators);
    RUN_TEST(test_allows_shipped_ui_pages_and_assets);
    RUN_TEST(test_rejects_unlisted_littlefs_paths);
    return UNITY_END();
}

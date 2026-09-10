/**
 * test_wifi_static_path_guard.cpp
 *
 * Pure-logic tests for the maintenance file server's dependency-free path
 * safety and served-path allowlist.
 */

#include <unity.h>

#include "../../src/modules/wifi/wifi_static_path_guard.cpp"

void setUp() {}
void tearDown() {}

void test_safe_path_rejects_null_empty_and_relative_inputs() {
    TEST_ASSERT_FALSE(WifiStaticPathGuard::isSafe(nullptr));
    TEST_ASSERT_FALSE(WifiStaticPathGuard::isSafe(""));
    TEST_ASSERT_FALSE(WifiStaticPathGuard::isSafe("index.html"));
}

void test_safe_path_rejects_whole_dot_dot_segments() {
    TEST_ASSERT_FALSE(WifiStaticPathGuard::isSafe("/../index.html"));
    TEST_ASSERT_FALSE(WifiStaticPathGuard::isSafe("/audio/../index.html"));
    TEST_ASSERT_FALSE(WifiStaticPathGuard::isSafe("/audio/.."));
}

void test_safe_path_allows_dot_dot_substrings() {
    TEST_ASSERT_TRUE(WifiStaticPathGuard::isSafe("/a..b"));
}

void test_safe_path_rejects_backslashes_anywhere() {
    TEST_ASSERT_FALSE(WifiStaticPathGuard::isSafe("/audio\\x.mul"));
    TEST_ASSERT_FALSE(WifiStaticPathGuard::isSafe("/audio/x.mul\\"));
}

void test_html_allowlist_accepts_every_page_path() {
    static constexpr const char* kPagePaths[] = {
        "/",         "/index.html",    "/alp",      "/alp.html",      "/audio",   "/audio.html",
        "/autopush", "/autopush.html", "/colors",   "/colors.html",   "/devices", "/devices.html",
        "/gps",      "/gps.html",      "/obd",      "/obd.html",
        "/profiles", "/profiles.html", "/settings", "/settings.html",
    };

    for (const char* path : kPagePaths) {
        TEST_ASSERT_TRUE_MESSAGE(WifiStaticPathGuard::isHtmlPagePath(path), path);
        TEST_ASSERT_TRUE_MESSAGE(WifiStaticPathGuard::isAllowedServedPath(path), path);
    }
}

void test_app_assets_are_served() {
    TEST_ASSERT_TRUE(WifiStaticPathGuard::isAllowedServedPath("/_app/env.js"));
    TEST_ASSERT_TRUE(WifiStaticPathGuard::isAllowedServedPath("/_app/version.json"));
    TEST_ASSERT_TRUE(WifiStaticPathGuard::isAllowedServedPath("/_app/immutable/chunks/app.js"));
}

void test_single_file_audio_and_branding_assets_are_served() {
    TEST_ASSERT_TRUE(WifiStaticPathGuard::isAllowedServedPath("/audio/x.mul"));
    TEST_ASSERT_TRUE(WifiStaticPathGuard::isAllowedServedPath("/branding/x.png"));
}

void test_single_file_assets_reject_nested_paths_and_wrong_suffixes() {
    TEST_ASSERT_FALSE(WifiStaticPathGuard::isAllowedServedPath("/audio/sub/x.mul"));
    TEST_ASSERT_FALSE(WifiStaticPathGuard::isAllowedServedPath("/branding/sub/x.png"));
    TEST_ASSERT_FALSE(WifiStaticPathGuard::isAllowedServedPath("/audio/x.png"));
    TEST_ASSERT_FALSE(WifiStaticPathGuard::isAllowedServedPath("/branding/x.jpg"));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_safe_path_rejects_null_empty_and_relative_inputs);
    RUN_TEST(test_safe_path_rejects_whole_dot_dot_segments);
    RUN_TEST(test_safe_path_allows_dot_dot_substrings);
    RUN_TEST(test_safe_path_rejects_backslashes_anywhere);
    RUN_TEST(test_html_allowlist_accepts_every_page_path);
    RUN_TEST(test_app_assets_are_served);
    RUN_TEST(test_single_file_audio_and_branding_assets_are_served);
    RUN_TEST(test_single_file_assets_reject_nested_paths_and_wrong_suffixes);
    return UNITY_END();
}

#include <unity.h>

#include <fstream>
#include <string>

namespace {

std::string readTextFile(const char* path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        return {};
    }
    return std::string((std::istreambuf_iterator<char>(stream)),
                       std::istreambuf_iterator<char>());
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_release_action_is_immutably_pinned_and_default_scope_is_read_only() {
    const std::string workflow = readTextFile(".github/workflows/release.yml");

    TEST_ASSERT_FALSE_MESSAGE(workflow.empty(), "failed to read release workflow");
    TEST_ASSERT_NOT_EQUAL(
        std::string::npos,
        workflow.find(
            "uses: softprops/action-gh-release@3bb12739c298aeb8a4eeaf626c5b8d85266b0e65"));
    TEST_ASSERT_EQUAL(std::string::npos,
                      workflow.find("uses: softprops/action-gh-release@v2"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          workflow.find("permissions:\n  contents: read"));
    TEST_ASSERT_NOT_EQUAL(
        std::string::npos,
        workflow.find("timeout-minutes: 45\n    permissions:\n      contents: write\n      pages: write"));
}

void test_runtime_dependency_notices_bundle_required_license_terms() {
    const std::string notices = readTextFile("THIRD_PARTY_NOTICES.md");
    const std::string arduinoJson = readTextFile("licenses/ArduinoJson-LICENSE.txt");
    const std::string nimbleLicense =
        readTextFile("licenses/NimBLE-Arduino-LICENSE.txt");
    const std::string nimbleNotice =
        readTextFile("licenses/NimBLE-Arduino-NOTICE.txt");
    const std::string gfx = readTextFile("licenses/Arduino-GFX-LICENSE.txt");
    const std::string ofr = readTextFile("licenses/OpenFontRender-LICENSE.txt");
    const std::string ftl = readTextFile("licenses/FreeType-FTL.txt");
    const std::string svelte = readTextFile("licenses/Svelte-LICENSE.md");
    const std::string svelteKit = readTextFile("licenses/SvelteKit-LICENSE.txt");
    const std::string daisyUi = readTextFile("licenses/daisyUI-LICENSE.txt");
    const std::string tailwind = readTextFile("licenses/Tailwind-CSS-LICENSE.txt");

    TEST_ASSERT_FALSE_MESSAGE(notices.empty(), "failed to read third-party notices");
    TEST_ASSERT_FALSE_MESSAGE(arduinoJson.empty(), "missing ArduinoJson license");
    TEST_ASSERT_FALSE_MESSAGE(nimbleLicense.empty(), "missing NimBLE-Arduino license");
    TEST_ASSERT_FALSE_MESSAGE(nimbleNotice.empty(), "missing NimBLE-Arduino notice");
    TEST_ASSERT_FALSE_MESSAGE(gfx.empty(), "missing Arduino GFX license");
    TEST_ASSERT_FALSE_MESSAGE(ofr.empty(), "missing OpenFontRender license");
    TEST_ASSERT_FALSE_MESSAGE(ftl.empty(), "missing FreeType license");
    TEST_ASSERT_FALSE_MESSAGE(svelte.empty(), "missing Svelte license");
    TEST_ASSERT_FALSE_MESSAGE(svelteKit.empty(), "missing SvelteKit license");
    TEST_ASSERT_FALSE_MESSAGE(daisyUi.empty(), "missing daisyUI license");
    TEST_ASSERT_FALSE_MESSAGE(tailwind.empty(), "missing Tailwind CSS license");
    TEST_ASSERT_EQUAL(std::string::npos,
                      notices.find("OpenFontRender** — MIT (bundles FreeType)"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          notices.find("This software is based in part on the work of the FreeType Team"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          notices.find("claim to complete that broader review"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          arduinoJson.find("Copyright © 2014-2026, Benoit BLANCHON"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          nimbleLicense.find("Apache License"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          nimbleLicense.find("Version 2.0, January 2004"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          nimbleNotice.find("Copyright 2020-2026 Ryan Powell"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          gfx.find("Software License Agreement (BSD License)"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          ofr.find("provided under the FTL license"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          ftl.find("The FreeType Project LICENSE"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          ftl.find("software is based in part of the work of the"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, ftl.find("FreeType Team"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          svelte.find("Copyright (c) 2016-2025"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          svelteKit.find("Copyright (c) 2020"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          daisyUi.find("Copyright (c) 2020 Pouya Saadeghi"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          tailwind.find("Copyright (c) Tailwind Labs, Inc."));
}

void test_release_distribution_ships_and_links_runtime_notices() {
    const std::string workflow = readTextFile(".github/workflows/release.yml");
    const std::string installer = readTextFile("web-installer/index.html");

    TEST_ASSERT_FALSE_MESSAGE(workflow.empty(), "failed to read release workflow");
    TEST_ASSERT_FALSE_MESSAGE(installer.empty(), "failed to read installer page");
    for (const char* path : {
             "THIRD_PARTY_NOTICES.md",
             "ArduinoJson-LICENSE.txt",
             "NimBLE-Arduino-LICENSE.txt",
             "NimBLE-Arduino-NOTICE.txt",
             "Arduino-GFX-LICENSE.txt",
             "OpenFontRender-LICENSE.txt",
             "FreeType-FTL.txt",
             "Svelte-LICENSE.md",
             "SvelteKit-LICENSE.txt",
             "daisyUI-LICENSE.txt",
             "Tailwind-CSS-LICENSE.txt",
         }) {
        TEST_ASSERT_NOT_EQUAL(std::string::npos, workflow.find(path));
    }
    for (const char* href : {
             "href=\"THIRD_PARTY_NOTICES.md\"",
             "href=\"licenses/ArduinoJson-LICENSE.txt\"",
             "href=\"licenses/NimBLE-Arduino-LICENSE.txt\"",
             "href=\"licenses/NimBLE-Arduino-NOTICE.txt\"",
             "href=\"licenses/Arduino-GFX-LICENSE.txt\"",
             "href=\"licenses/OpenFontRender-LICENSE.txt\"",
             "href=\"licenses/FreeType-FTL.txt\"",
             "href=\"licenses/Svelte-LICENSE.md\"",
             "href=\"licenses/SvelteKit-LICENSE.txt\"",
             "href=\"licenses/daisyUI-LICENSE.txt\"",
             "href=\"licenses/Tailwind-CSS-LICENSE.txt\"",
         }) {
        TEST_ASSERT_NOT_EQUAL(std::string::npos, installer.find(href));
    }
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_release_action_is_immutably_pinned_and_default_scope_is_read_only);
    RUN_TEST(test_runtime_dependency_notices_bundle_required_license_terms);
    RUN_TEST(test_release_distribution_ships_and_links_runtime_notices);
    return UNITY_END();
}

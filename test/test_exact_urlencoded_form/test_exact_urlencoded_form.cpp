#include <unity.h>

#include <cstring>

#include "../../src/modules/wifi/exact_urlencoded_form.h"

void setUp() {}
void tearDown() {}

void test_exact_form_decodes_complete_values_and_preserves_presence() {
    const char body[] = "slot=2&profile=Road+Trip&name=K%2FKa";
    ExactUrlEncodedForm form(reinterpret_cast<const uint8_t*>(body), sizeof(body) - 1u);
    TEST_ASSERT_TRUE(form.valid());
    TEST_ASSERT_TRUE(form.has("slot"));
    TEST_ASSERT_TRUE(form.has("profile"));
    TEST_ASSERT_FALSE(form.has("mode"));
    String profile;
    String name;
    TEST_ASSERT_TRUE(form.read("profile", profile));
    TEST_ASSERT_EQUAL_STRING("Road Trip", profile.c_str());
    TEST_ASSERT_TRUE(form.read("name", name));
    TEST_ASSERT_EQUAL_STRING("K/Ka", name.c_str());
}

void test_exact_form_rejects_duplicate_and_escaped_equivalent_keys() {
    const char duplicate[] = "slot=1&slot=2";
    TEST_ASSERT_FALSE(ExactUrlEncodedForm(reinterpret_cast<const uint8_t*>(duplicate),
                                          sizeof(duplicate) - 1u).valid());
    const char escaped[] = "profile=Road&prof%69le=Other";
    TEST_ASSERT_FALSE(ExactUrlEncodedForm(reinterpret_cast<const uint8_t*>(escaped),
                                          sizeof(escaped) - 1u).valid());
}

void test_exact_form_rejects_truncation_invalid_utf8_and_unsafe_controls() {
    const char percent[] = "slot=%";
    TEST_ASSERT_FALSE(ExactUrlEncodedForm(reinterpret_cast<const uint8_t*>(percent),
                                          sizeof(percent) - 1u).valid());
    const uint8_t utf8[] = {'n','a','m','e','=',0xC0,0xAF};
    TEST_ASSERT_FALSE(ExactUrlEncodedForm(utf8, sizeof(utf8)).valid());
    const char control[] = "name=%01";
    TEST_ASSERT_FALSE(ExactUrlEncodedForm(reinterpret_cast<const uint8_t*>(control),
                                          sizeof(control) - 1u).valid());
}

void test_exact_form_allows_valid_unicode_and_intentional_empty_value() {
    const uint8_t body[] = {'n','a','m','e','=',0xE2,0x98,0x83,'&','p','r','o','f','i','l','e','='};
    ExactUrlEncodedForm form(body, sizeof(body));
    TEST_ASSERT_TRUE(form.valid());
    String name;
    String profile;
    TEST_ASSERT_TRUE(form.read("name", name));
    TEST_ASSERT_EQUAL_UINT(3u, name.length());
    TEST_ASSERT_TRUE(form.read("profile", profile, true));
    TEST_ASSERT_EQUAL_UINT(0u, profile.length());
}

void test_exact_form_rejects_field_flood_and_malformed_pairs() {
    String flood;
    for (int i = 0; i < 33; ++i) {
        if (i != 0) flood += '&';
        flood += 'k';
        flood += String(i);
        flood += "=v";
    }
    TEST_ASSERT_FALSE(ExactUrlEncodedForm(reinterpret_cast<const uint8_t*>(flood.c_str()),
                                          flood.length()).valid());
    const char missingEquals[] = "slot=1&profile";
    TEST_ASSERT_FALSE(ExactUrlEncodedForm(reinterpret_cast<const uint8_t*>(missingEquals),
                                          sizeof(missingEquals) - 1u).valid());
}

void test_exact_form_allowed_key_set_rejects_unknown_typo() {
    const char body[] = "slot=1&profile=Road&priorityArrowOnyl=true";
    ExactUrlEncodedForm form(reinterpret_cast<const uint8_t*>(body), sizeof(body) - 1u);
    TEST_ASSERT_TRUE(form.valid());
    static constexpr const char* ALLOWED[] = {"slot", "profile", "priorityArrowOnly"};
    TEST_ASSERT_FALSE(form.hasOnly(ALLOWED, sizeof(ALLOWED) / sizeof(ALLOWED[0])));
    static constexpr const char* INCLUDING_TYPO[] = {"slot", "profile", "priorityArrowOnyl"};
    TEST_ASSERT_TRUE(form.hasOnly(INCLUDING_TYPO, sizeof(INCLUDING_TYPO) / sizeof(INCLUDING_TYPO[0])));
}

void test_exact_multipart_accepts_shipped_string_form_shape() {
    const char boundary[] = "----WebKitFormBoundaryV1Simple";
    const char body[] =
        "------WebKitFormBoundaryV1Simple\r\n"
        "Content-Disposition: form-data; name=\"slot\"\r\n\r\n"
        "2\r\n"
        "------WebKitFormBoundaryV1Simple\r\n"
        "Content-Disposition: form-data; name=\"profile\"\r\n\r\n"
        "Road+Trip%20Literal\r\n"
        "------WebKitFormBoundaryV1Simple--\r\n";
    ExactUrlEncodedForm form(reinterpret_cast<const uint8_t*>(body), sizeof(body) - 1u,
                             boundary, sizeof(boundary) - 1u);
    TEST_ASSERT_TRUE(form.valid());
    TEST_ASSERT_EQUAL_UINT(2u, form.fieldCount());
    String slot;
    String profile;
    TEST_ASSERT_TRUE(form.read("slot", slot));
    TEST_ASSERT_TRUE(form.read("profile", profile));
    TEST_ASSERT_EQUAL_STRING("2", slot.c_str());
    TEST_ASSERT_EQUAL_STRING("Road+Trip%20Literal", profile.c_str());
}

void test_exact_multipart_rejects_duplicates_unknown_headers_and_epilogue() {
    const char boundary[] = "legacy";
    const char duplicate[] =
        "--legacy\r\nContent-Disposition: form-data; name=\"slot\"\r\n\r\n1\r\n"
        "--legacy\r\nContent-Disposition: form-data; name=\"slot\"\r\n\r\n2\r\n"
        "--legacy--\r\n";
    TEST_ASSERT_FALSE(ExactUrlEncodedForm(reinterpret_cast<const uint8_t*>(duplicate),
                                           sizeof(duplicate) - 1u, boundary,
                                           sizeof(boundary) - 1u).valid());

    const char filename[] =
        "--legacy\r\nContent-Disposition: form-data; name=\"slot\"; filename=\"x\"\r\n\r\n1\r\n"
        "--legacy--\r\n";
    TEST_ASSERT_FALSE(ExactUrlEncodedForm(reinterpret_cast<const uint8_t*>(filename),
                                           sizeof(filename) - 1u, boundary,
                                           sizeof(boundary) - 1u).valid());

    const char epilogue[] =
        "--legacy\r\nContent-Disposition: form-data; name=\"slot\"\r\n\r\n1\r\n"
        "--legacy--\r\nextra";
    TEST_ASSERT_FALSE(ExactUrlEncodedForm(reinterpret_cast<const uint8_t*>(epilogue),
                                           sizeof(epilogue) - 1u, boundary,
                                           sizeof(boundary) - 1u).valid());
}

void test_exact_multipart_rejects_wrong_boundary_and_unsafe_value() {
    const char boundary[] = "legacy";
    const char wrong[] =
        "--other\r\nContent-Disposition: form-data; name=\"slot\"\r\n\r\n1\r\n--other--\r\n";
    TEST_ASSERT_FALSE(ExactUrlEncodedForm(reinterpret_cast<const uint8_t*>(wrong), sizeof(wrong) - 1u,
                                           boundary, sizeof(boundary) - 1u).valid());
    const uint8_t unsafe[] = {
        '-','-','l','e','g','a','c','y','\r','\n',
        'C','o','n','t','e','n','t','-','D','i','s','p','o','s','i','t','i','o','n',':',' ',
        'f','o','r','m','-','d','a','t','a',';',' ','n','a','m','e','=','\"','n','a','m','e','\"',
        '\r','\n','\r','\n',0xFF,'\r','\n','-','-','l','e','g','a','c','y','-','-','\r','\n'};
    TEST_ASSERT_FALSE(ExactUrlEncodedForm(unsafe, sizeof(unsafe), boundary,
                                           sizeof(boundary) - 1u).valid());
}

int main(int argc, char** argv) {
    UNITY_BEGIN();
    RUN_TEST(test_exact_form_decodes_complete_values_and_preserves_presence);
    RUN_TEST(test_exact_form_rejects_duplicate_and_escaped_equivalent_keys);
    RUN_TEST(test_exact_form_rejects_truncation_invalid_utf8_and_unsafe_controls);
    RUN_TEST(test_exact_form_allows_valid_unicode_and_intentional_empty_value);
    RUN_TEST(test_exact_form_rejects_field_flood_and_malformed_pairs);
    RUN_TEST(test_exact_form_allowed_key_set_rejects_unknown_typo);
    RUN_TEST(test_exact_multipart_accepts_shipped_string_form_shape);
    RUN_TEST(test_exact_multipart_rejects_duplicates_unknown_headers_and_epilogue);
    RUN_TEST(test_exact_multipart_rejects_wrong_boundary_and_unsafe_value);
    return UNITY_END();
}

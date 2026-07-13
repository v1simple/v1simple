#include <unity.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

std::string readTextFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        return {};
    }
    return std::string((std::istreambuf_iterator<char>(stream)),
                       std::istreambuf_iterator<char>());
}

bool isNetworkApiSource(const std::filesystem::path& path) {
    const std::string normalized = path.generic_string();
    const std::string filename = path.filename().string();
    return normalized.find("src/modules/wifi/") == 0 ||
           filename == "wifi_routes.cpp" ||
           filename == "wifi_runtimes.cpp" ||
           filename == "wifi_manager.cpp" ||
           filename == "wifi_manager_helpers.cpp" ||
           filename == "wifi_manager_lifecycle.cpp";
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_network_api_sources_do_not_serialize_credential_keys() {
    static const std::vector<std::string> forbiddenJsonOutputs = {
        "doc[\"apPassword\"]",
        "doc[\"passwordObf\"]",
        "doc[\"wifiClientPasswordObf\"]",
        "doc[\"stationPassword\"]",
    };

    size_t checkedFiles = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator("src")) {
        if (!entry.is_regular_file() || !isNetworkApiSource(entry.path())) {
            continue;
        }
        const std::string extension = entry.path().extension().string();
        if (extension != ".cpp" && extension != ".h") {
            continue;
        }

        const std::string source = readTextFile(entry.path());
        TEST_ASSERT_FALSE_MESSAGE(source.empty(), entry.path().c_str());
        for (const std::string& forbidden : forbiddenJsonOutputs) {
            TEST_ASSERT_EQUAL_MESSAGE(std::string::npos,
                                      source.find(forbidden),
                                      entry.path().c_str());
        }
        ++checkedFiles;
    }
    TEST_ASSERT_GREATER_THAN_UINT32(10u, checkedFiles);
}

void test_http_backup_builder_guards_ap_secret_to_sd_transport() {
    const std::string source = readTextFile("src/backup_payload_builder.cpp");

    TEST_ASSERT_FALSE_MESSAGE(source.empty(), "failed to read backup payload builder");
    TEST_ASSERT_NOT_EQUAL(
        std::string::npos,
        source.find("if (transport == BackupTransport::SdBackup)"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, source.find("doc[\"apPassword\"]"));
    TEST_ASSERT_EQUAL(std::string::npos, source.find("includeWifiPasswords"));
    TEST_ASSERT_EQUAL(std::string::npos, source.find("doc[WIFI_STA_SLOT_PASSWORD_OBF_KEY]"));
}

void test_ui_and_api_docs_offer_only_sanitized_network_backup() {
    const std::string page =
        readTextFile("interface/src/lib/features/settings/SettingsPage.svelte");
    const std::string card =
        readTextFile("interface/src/lib/features/settings/SettingsBackupCard.svelte");
    const std::string api = readTextFile("docs/API.md");

    TEST_ASSERT_FALSE_MESSAGE(page.empty(), "failed to read settings page");
    TEST_ASSERT_FALSE_MESSAGE(card.empty(), "failed to read backup card");
    TEST_ASSERT_FALSE_MESSAGE(api.empty(), "failed to read API docs");
    TEST_ASSERT_EQUAL(std::string::npos, page.find("includePasswords"));
    TEST_ASSERT_EQUAL(std::string::npos, card.find("Backup with WiFi Passwords"));
    TEST_ASSERT_NOT_EQUAL(
        std::string::npos,
        api.find("network-accessible backups never contain credential material"));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_network_api_sources_do_not_serialize_credential_keys);
    RUN_TEST(test_http_backup_builder_guards_ap_secret_to_sd_transport);
    RUN_TEST(test_ui_and_api_docs_offer_only_sanitized_network_backup);
    return UNITY_END();
}

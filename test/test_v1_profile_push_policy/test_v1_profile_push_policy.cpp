#include <unity.h>

#include <cstring>

#include "../../src/settings.h"
#include "../../src/v1_profile_push_policy.h"
#include "../../src/v1_profiles.h"

namespace {

constexpr size_t kUserSettingsSize = 6;
constexpr uint8_t kLaserEnabled = 0x08;

void copyUserSettings(V1UserSettings& settings, const uint8_t (&bytes)[kUserSettingsSize]) {
    std::memcpy(settings.bytes, bytes, sizeof(bytes));
}

} // namespace

void setUp(void) {}
void tearDown(void) {}

void test_production_defaults_preserve_v1_laser_until_alp_is_enabled() {
    V1Settings settings;
    V1UserSettings userSettings;
    const uint8_t original[kUserSettingsSize] = {0xAF, 0x12, 0x34, 0x56, 0x78, 0x9A};
    copyUserSettings(userSettings, original);

    TEST_ASSERT_FALSE(settings.alpEnabled);
    TEST_ASSERT_TRUE(settings.alpDisableV1LaserOnPush);
    TEST_ASSERT_FALSE(V1ProfilePushPolicy::shouldDisableV1Laser(settings));

    V1ProfilePushPolicy::applyBeforePushToUserSettings(settings, userSettings);

    TEST_ASSERT_EQUAL_UINT8_ARRAY(original, userSettings.bytes, kUserSettingsSize);
}

void test_policy_requires_both_production_settings_flags() {
    struct Case {
        bool alpEnabled;
        bool disableV1LaserOnPush;
        bool shouldDisable;
    };
    const Case cases[] = {
        {false, false, false},
        {false, true, false},
        {true, false, false},
        {true, true, true},
    };
    const uint8_t original[kUserSettingsSize] = {0xFF, 0x12, 0x34, 0x56, 0x78, 0x9A};

    for (const Case& testCase : cases) {
        V1Settings settings;
        settings.alpEnabled = testCase.alpEnabled;
        settings.alpDisableV1LaserOnPush = testCase.disableV1LaserOnPush;
        uint8_t actual[kUserSettingsSize];
        std::memcpy(actual, original, sizeof(actual));

        TEST_ASSERT_EQUAL(testCase.shouldDisable, V1ProfilePushPolicy::shouldDisableV1Laser(settings));
        V1ProfilePushPolicy::applyBeforePush(settings, actual);

        uint8_t expected[kUserSettingsSize];
        std::memcpy(expected, original, sizeof(expected));
        if (testCase.shouldDisable) {
            expected[0] &= static_cast<uint8_t>(~kLaserEnabled);
        }
        TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, actual, kUserSettingsSize);
    }
}

void test_user_settings_push_clears_only_the_v1_laser_enable_bit() {
    V1Settings settings;
    settings.alpEnabled = true;
    V1UserSettings userSettings;
    const uint8_t original[kUserSettingsSize] = {0xBF, 0x12, 0x34, 0x56, 0x78, 0x9A};
    copyUserSettings(userSettings, original);

    V1ProfilePushPolicy::applyBeforePushToUserSettings(settings, userSettings);

    const uint8_t expected[kUserSettingsSize] = {0xB7, 0x12, 0x34, 0x56, 0x78, 0x9A};
    TEST_ASSERT_FALSE(userSettings.laserEnabled());
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, userSettings.bytes, kUserSettingsSize);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_production_defaults_preserve_v1_laser_until_alp_is_enabled);
    RUN_TEST(test_policy_requires_both_production_settings_flags);
    RUN_TEST(test_user_settings_push_clears_only_the_v1_laser_enable_bit);
    return UNITY_END();
}

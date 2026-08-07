#include <unity.h>

#include "../../src/settings.h"

void setUp(void) {}
void tearDown(void) {}

void test_v1_settings_defaults_cover_current_runtime_shape() {
	V1Settings settings;

	TEST_ASSERT_TRUE(settings.enableWifi);
	TEST_ASSERT_EQUAL_INT(V1_WIFI_AP, settings.wifiMode);
	TEST_ASSERT_EQUAL_STRING("V1-Simple", settings.apSSID.c_str());
	TEST_ASSERT_TRUE(settings.proxyBLE);
	TEST_ASSERT_EQUAL_UINT8(200, settings.brightness);
	TEST_ASSERT_EQUAL_HEX16(0x001F, settings.colorObd);
	TEST_ASSERT_EQUAL(kDefaultAutoPushEnabled, settings.autoPushEnabled);
	TEST_ASSERT_EQUAL_INT(0, settings.activeSlot);
	TEST_ASSERT_EQUAL_STRING("DEFAULT", settings.slot0Name.c_str());
	TEST_ASSERT_EQUAL_INT(V1_MODE_UNKNOWN, settings.slot0_default.mode);
	TEST_ASSERT_FALSE(settings.obdEnabled);
	TEST_ASSERT_EQUAL_STRING("", settings.obdSavedName.c_str());
	TEST_ASSERT_EQUAL_UINT8(0, settings.obdSavedAddrType);
	TEST_ASSERT_EQUAL_INT8(-90, settings.obdMinRssi);
	TEST_ASSERT_FALSE(settings.alpEnabled);
	TEST_ASSERT_TRUE(settings.alpDisableV1LaserOnPush);
}

void test_auto_push_slot_view_maps_mutable_fields_to_selected_slot() {
	V1Settings settings;
	auto slot = settings.autoPushSlotView(1);

	slot.name = "TRAVEL";
	slot.color = 0x1234;
	slot.volume = 4;
	slot.muteVolume = 2;
	slot.darkMode = true;
	slot.muteToZero = true;
	slot.alertPersist = 5;
	slot.priorityArrow = true;
	slot.config.profileName = "Highway";
	slot.config.mode = V1_MODE_ALL_BOGEYS;

	TEST_ASSERT_EQUAL_STRING("TRAVEL", settings.slot1Name.c_str());
	TEST_ASSERT_EQUAL_UINT16(0x1234, settings.slot1Color);
	TEST_ASSERT_EQUAL_UINT8(4, settings.slot1Volume);
	TEST_ASSERT_EQUAL_UINT8(2, settings.slot1MuteVolume);
	TEST_ASSERT_TRUE(settings.slot1DarkMode);
	TEST_ASSERT_TRUE(settings.slot1MuteToZero);
	TEST_ASSERT_EQUAL_UINT8(5, settings.slot1AlertPersist);
	TEST_ASSERT_TRUE(settings.slot1PriorityArrow);
	TEST_ASSERT_EQUAL_STRING("Highway", settings.slot1_highway.profileName.c_str());
	TEST_ASSERT_EQUAL_INT(V1_MODE_ALL_BOGEYS, settings.slot1_highway.mode);
}

void test_auto_push_slot_view_defaults_invalid_slot_to_slot_zero() {
	V1Settings settings;
	settings.slot0Name = "DEFAULT-X";
	settings.slot0_default.profileName = "City";
	settings.slot2Name = "COMFORT-X";
	settings.slot2_comfort.profileName = "Quiet";

	const auto slot = settings.autoPushSlotView(99);

	TEST_ASSERT_EQUAL_STRING("DEFAULT-X", slot.name.c_str());
	TEST_ASSERT_EQUAL_STRING("City", slot.config.profileName.c_str());
}

int main() {
	UNITY_BEGIN();
	RUN_TEST(test_v1_settings_defaults_cover_current_runtime_shape);
	RUN_TEST(test_auto_push_slot_view_maps_mutable_fields_to_selected_slot);
	RUN_TEST(test_auto_push_slot_view_defaults_invalid_slot_to_slot_zero);
	return UNITY_END();
}

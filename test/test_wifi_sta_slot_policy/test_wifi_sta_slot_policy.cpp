#include <unity.h>

#include "../mocks/Arduino.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../src/modules/wifi/wifi_sta_slot_policy.cpp"

namespace {

void clearWifiSlots(V1Settings& settings) {
    for (size_t i = 0; i < kWifiStaSlotCount; ++i) {
        settings.wifiStaSlots[i] = WifiStaSlot();
    }
}

void setSlot(V1Settings& settings,
             size_t index,
             const char* ssid,
             uint8_t priority,
             uint32_t lastConnectedAtSec) {
    settings.wifiStaSlots[index].ssid = ssid;
    settings.wifiStaSlots[index].label = ssid;
    settings.wifiStaSlots[index].priority = priority;
    settings.wifiStaSlots[index].lastConnectedAtSec = lastConnectedAtSec;
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_order_configured_slots_uses_priority_then_last_connected_then_index() {
    V1Settings settings;
    clearWifiSlots(settings);
    setSlot(settings, 0, "Bench", 5, 100);
    setSlot(settings, 1, "Garage", 0, 10);
    setSlot(settings, 2, "Phone", 0, 300);
    setSlot(settings, 3, "Car", 0, 300);

    size_t ordered[kWifiStaSlotCount] = {};
    const size_t count = WifiStaSlotPolicy::orderConfiguredSlots(settings,
                                                                 ordered,
                                                                 kWifiStaSlotCount);

    TEST_ASSERT_EQUAL_UINT(4, count);
    TEST_ASSERT_EQUAL_UINT(2, ordered[0]);  // highest last-connected among priority-0 slots
    TEST_ASSERT_EQUAL_UINT(3, ordered[1]);  // same priority/last-connected: lower index first
    TEST_ASSERT_EQUAL_UINT(1, ordered[2]);
    TEST_ASSERT_EQUAL_UINT(0, ordered[3]);
}

void test_select_in_range_slots_skips_out_of_range_ssids_but_preserves_priority_order() {
    V1Settings settings;
    clearWifiSlots(settings);
    setSlot(settings, 0, "Car", 0, 0);
    setSlot(settings, 1, "Garage", 1, 20);
    setSlot(settings, 2, "Phone", 1, 200);
    setSlot(settings, 3, "Bench", 9, 999);
    const String scanned[] = {"Phone", "Garage"};

    size_t selected[kWifiStaSlotCount] = {};
    const size_t count = WifiStaSlotPolicy::selectInRangeSlots(settings,
                                                               scanned,
                                                               2,
                                                               selected,
                                                               kWifiStaSlotCount);

    TEST_ASSERT_EQUAL_UINT(2, count);
    TEST_ASSERT_EQUAL_UINT(2, selected[0]);
    TEST_ASSERT_EQUAL_UINT(1, selected[1]);
}

void test_select_in_range_slots_returns_empty_when_no_saved_ssids_match() {
    V1Settings settings;
    clearWifiSlots(settings);
    setSlot(settings, 0, "Car", 0, 0);
    setSlot(settings, 1, "Garage", 1, 20);
    const String scanned[] = {"CoffeeShop", "Neighbor"};

    size_t selected[kWifiStaSlotCount] = {};
    const size_t count = WifiStaSlotPolicy::selectInRangeSlots(settings,
                                                               scanned,
                                                               2,
                                                               selected,
                                                               kWifiStaSlotCount);

    TEST_ASSERT_EQUAL_UINT(0, count);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_order_configured_slots_uses_priority_then_last_connected_then_index);
    RUN_TEST(test_select_in_range_slots_skips_out_of_range_ssids_but_preserves_priority_order);
    RUN_TEST(test_select_in_range_slots_returns_empty_when_no_saved_ssids_match);
    return UNITY_END();
}

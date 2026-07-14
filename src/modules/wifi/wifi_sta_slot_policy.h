#pragma once

#include <Arduino.h>

#include <cstddef>

#include "../../settings.h"

namespace WifiStaSlotPolicy {

size_t orderConfiguredSlots(const V1Settings& settings, size_t* indicesOut, size_t maxIndices);

bool scanContainsSsid(const String* scannedSsids, size_t scannedCount, const String& ssid);

size_t selectInRangeSlots(const V1Settings& settings, const String* scannedSsids, size_t scannedCount,
                          size_t* indicesOut, size_t maxIndices);

} // namespace WifiStaSlotPolicy

#pragma once

#include <Arduino.h>

#include <cstddef>

#include "../../settings_types.h"

namespace WifiStaSlotPolicy {

size_t orderConfiguredSlots(const V1Settings& settings, size_t* indicesOut, size_t maxIndices);

} // namespace WifiStaSlotPolicy

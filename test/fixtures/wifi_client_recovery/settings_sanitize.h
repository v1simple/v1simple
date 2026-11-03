#pragma once
#include "Arduino.h"
// Credential-owner fixture uses the production SSID truncation contract.
inline String sanitizeWifiClientSsidValue(const String& value) { return value.substring(0, 32); }

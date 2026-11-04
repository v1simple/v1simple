#pragma once
#include "presentation_inputs_fixture.h"
#include "../../src/display_presentation.cpp"

// These isolated leaf suites provide no ALP event. Session tests execute the
// real parser and real label lookup through their complete renderer fixture.
const char* alpGunAbbrev(AlpGunType) {
    TEST_FAIL_MESSAGE("leaf fixture received an unexpected ALP gun event");
    return "";
}

#include <unity.h>
#include <cstring>

#include "../mocks/Arduino.h"
#include "../mocks/display.h"

#include "../../src/modules/display/display_preview_module.cpp"
#include "../../src/modules/wifi/wifi_display_visual_api_service.h"
#include "../../src/modules/wifi/wifi_display_visual_api_service.cpp"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

PerfCounters perfCounters;
PerfExtendedMetrics perfExtended;

namespace {
V1Settings settings;
V1Display display;
DisplayPreviewModule preview;
PerfDisplayRenderScenario lastScenario = PerfDisplayRenderScenario::None;
}

void perfSetDisplayRenderScenario(PerfDisplayRenderScenario scenario) {
    lastScenario = scenario;
}

PerfDisplayRenderScenario perfGetDisplayRenderScenario() {
    return lastScenario;
}

void perfClearDisplayRenderScenario() {
    lastScenario = PerfDisplayRenderScenario::None;
}

void perfRecordDisplayScenarioRenderUs(uint32_t /*us*/) {}

static WifiDisplayVisualApiService::Runtime makeRuntime(bool maintenance = true) {
    WifiDisplayVisualApiService::Runtime r;
    r.preview = &preview;
    r.display = &display;
    r.getSettings = [](void* /*ctx*/) -> const V1Settings& {
        return settings;
    };
    r.firmwareVersion = "test-fw";
    r.firmwareSha = "test-sha";
    r.maintenanceBootActive = maintenance;
    return r;
}

static bool bodyContains(const WebServer& server, const char* needle) {
    return std::strstr(server.lastBody.c_str(), needle) != nullptr;
}

static void assertRectInBounds(JsonVariantConst value) {
    TEST_ASSERT_TRUE(value.is<JsonObjectConst>());
    JsonObjectConst rect = value.as<JsonObjectConst>();
    TEST_ASSERT_FALSE(rect["x"].isNull());
    TEST_ASSERT_FALSE(rect["y"].isNull());
    TEST_ASSERT_FALSE(rect["w"].isNull());
    TEST_ASSERT_FALSE(rect["h"].isNull());
    const int x = rect["x"].as<int>();
    const int y = rect["y"].as<int>();
    const int w = rect["w"].as<int>();
    const int h = rect["h"].as<int>();
    TEST_ASSERT_TRUE(x >= 0);
    TEST_ASSERT_TRUE(y >= 0);
    TEST_ASSERT_TRUE(w > 0);
    TEST_ASSERT_TRUE(h > 0);
    TEST_ASSERT_TRUE(x + w <= V1Display::logicalFramebufferWidth());
    TEST_ASSERT_TRUE(y + h <= V1Display::logicalFramebufferHeight());
}

static void assertArrayRectsInBounds(JsonArrayConst elements) {
    for (JsonVariantConst element : elements) {
        TEST_ASSERT_TRUE(element.is<JsonObjectConst>());
        assertRectInBounds(element["rect"]);
    }
}

static bool paletteHasRole(JsonObjectConst palette, const char* role) {
    if (!role || role[0] == '\0') return false;
    const char* dot = std::strchr(role, '.');
    if (!dot) return !palette[role].isNull();

    const size_t groupLen = static_cast<size_t>(dot - role);
    if (groupLen == 0 || groupLen >= 32 || dot[1] == '\0') return false;
    char group[32];
    std::memcpy(group, role, groupLen);
    group[groupLen] = '\0';
    return !palette[group][dot + 1].isNull();
}

void setUp() {
    settings = V1Settings{};
    display.reset();
    preview = DisplayPreviewModule{};
    preview.begin(&display);
    mockMillis = 5000;
    mockMicros = 100000;
    lastScenario = PerfDisplayRenderScenario::None;
}

void tearDown() {}

void test_steps_requires_maintenance_boot() {
    WebServer server(80);

    WifiDisplayVisualApiService::handleSteps(server, makeRuntime(false));

    TEST_ASSERT_EQUAL_INT(403, server.lastStatusCode);
    TEST_ASSERT_TRUE(bodyContains(server, "maintenance_required"));
}

void test_steps_manifest_streams_complete_resolved_payload() {
    WebServer server(80);

    WifiDisplayVisualApiService::handleSteps(server, makeRuntime());

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_STRING("application/json", server.lastContentType.c_str());
    TEST_ASSERT_EQUAL_UINT(server.lastContentLength, server.lastBody.length());
    TEST_ASSERT_TRUE(bodyContains(server, "\"manifest\":\"display-visual-steps\""));
    TEST_ASSERT_TRUE(bodyContains(server, "\"stepCount\":"));
    TEST_ASSERT_TRUE(bodyContains(server, "\"complete\":true"));
    TEST_ASSERT_TRUE(bodyContains(server, "\"mainMeterCount\":8"));
    TEST_ASSERT_TRUE(bodyContains(server, "\"frequencyText\":\"35.500\""));
    TEST_ASSERT_TRUE(bodyContains(server, "\"frequencyRole\":\"frequency\""));
    TEST_ASSERT_TRUE(bodyContains(server, "\"frequencyRole\":\"muted\""));
    TEST_ASSERT_TRUE(bodyContains(server, "\"topCounterRole\":"));
    TEST_ASSERT_TRUE(bodyContains(server, "\"muteBadgeRole\":\"muted\""));
    TEST_ASSERT_TRUE(bodyContains(server, "\"alpBadgeRole\":\"status.alpDli\""));
    TEST_ASSERT_TRUE(bodyContains(server, "\"alpBadgeRole\":\"status.alpLidActive\""));
    TEST_ASSERT_TRUE(bodyContains(server, "\"alpBadgeRole\":\"status.alpAlert\""));
    TEST_ASSERT_TRUE(bodyContains(server, "\"obdBadgeRole\":\"status.obdAttention\""));
    TEST_ASSERT_TRUE(bodyContains(server, "\"bleBadgeRole\":\"status.bleAdvertising\""));
    TEST_ASSERT_TRUE(bodyContains(server, "\"bleBadgeRole\":\"status.bleConnected\""));
    TEST_ASSERT_TRUE(bodyContains(server, "\"settingsFingerprint\":\"0x"));

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, server.lastBody.c_str());
    TEST_ASSERT_FALSE(err);
    JsonArray steps = doc["steps"].as<JsonArray>();
    TEST_ASSERT_EQUAL_UINT(44, steps.size());
    constexpr char kProtocolGlyphs[] = "0123456789&uJLCU#cdFPAEb";
    constexpr const char* kNumericFrequencies[] = {
        "10.525", "24.150", "24.199", "33.800", "34.700", "35.500",
    };
    constexpr int kAlphaFrequencyIndices[] = {15, 16, 17, 30, 31};
    bool seen[128] = {};
    bool seenFrequency[sizeof(kNumericFrequencies) / sizeof(kNumericFrequencies[0])] = {};
    size_t numericFrequencySteps = 0;
    for (JsonObject step : steps) {
        const char* bogey = step["resolved"]["status"]["bogeyChar"].as<const char*>();
        TEST_ASSERT_NOT_NULL(bogey);
        TEST_ASSERT_EQUAL_UINT(1, std::strlen(bogey));
        TEST_ASSERT_NOT_EQUAL(' ', bogey[0]);
        const unsigned char value = static_cast<unsigned char>(bogey[0]);
        TEST_ASSERT_LESS_THAN_UINT(128, value);
        seen[value] = true;

        const char* frequency =
            step["resolved"]["primary"]["frequencyText"].as<const char*>();
        TEST_ASSERT_NOT_NULL(frequency);
        const bool numeric =
            std::strlen(frequency) == 6 && frequency[2] == '.' &&
            frequency[0] >= '0' && frequency[0] <= '9' &&
            frequency[1] >= '0' && frequency[1] <= '9' &&
            frequency[3] >= '0' && frequency[3] <= '9' &&
            frequency[4] >= '0' && frequency[4] <= '9' &&
            frequency[5] >= '0' && frequency[5] <= '9';
        if (numeric) {
            ++numericFrequencySteps;
            bool recognized = false;
            for (size_t i = 0; i < sizeof(kNumericFrequencies) / sizeof(kNumericFrequencies[0]); ++i) {
                if (std::strcmp(frequency, kNumericFrequencies[i]) == 0) {
                    seenFrequency[i] = true;
                    recognized = true;
                }
            }
            TEST_ASSERT_TRUE_MESSAGE(
                recognized,
                "serialized steps manifest contains an unexpected numeric frequency");
        } else {
            const int stepIndex = step["index"].as<int>();
            bool expectedAlphaIndex = false;
            for (int alphaIndex : kAlphaFrequencyIndices) {
                if (stepIndex == alphaIndex) expectedAlphaIndex = true;
            }
            TEST_ASSERT_TRUE_MESSAGE(
                expectedAlphaIndex,
                "serialized steps manifest moved an alpha frequency into a numeric step");
        }
    }
    for (char glyph : kProtocolGlyphs) {
        if (glyph == '\0') break;
        TEST_ASSERT_TRUE_MESSAGE(
            seen[static_cast<unsigned char>(glyph)],
            "serialized steps manifest must cover every protocol bogey glyph");
    }
    TEST_ASSERT_EQUAL_UINT(39, numericFrequencySteps);
    for (bool frequencySeen : seenFrequency) {
        TEST_ASSERT_TRUE_MESSAGE(
            frequencySeen,
            "serialized steps manifest must cover every qualified numeric frequency");
    }
}

void test_layout_manifest_includes_framebuffer_transform_and_palette() {
    WebServer server(80);

    WifiDisplayVisualApiService::handleLayout(server, makeRuntime());

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_UINT(server.lastContentLength, server.lastBody.length());
    TEST_ASSERT_TRUE(bodyContains(server, "\"manifest\":\"display-visual-layout\""));
    TEST_ASSERT_TRUE(bodyContains(server, "\"width\":640"));
    TEST_ASSERT_TRUE(bodyContains(server, "\"height\":172"));
    TEST_ASSERT_TRUE(bodyContains(server, "\"format\":\"RGB565LE\""));
    TEST_ASSERT_TRUE(bodyContains(server, "\"transform\":\"canvas-rotation-1\""));
    TEST_ASSERT_TRUE(bodyContains(server, "\"mainMeterRamp\""));
    TEST_ASSERT_TRUE(bodyContains(server, "\"mainSignalBars\""));
    TEST_ASSERT_TRUE(bodyContains(server, "\"bandCells\""));
    TEST_ASSERT_TRUE(bodyContains(server, "\"rect\""));
    TEST_ASSERT_TRUE(bodyContains(server, "\"directionArrows\""));
    TEST_ASSERT_TRUE(bodyContains(server, "\"cardMeterBars\""));
    TEST_ASSERT_TRUE(bodyContains(server, "\"frequency\""));
    TEST_ASSERT_TRUE(bodyContains(server, "\"statusText\""));
    TEST_ASSERT_TRUE(bodyContains(server, "\"statusBadges\""));
    TEST_ASSERT_TRUE(bodyContains(server, "\"roleSource\""));
    TEST_ASSERT_TRUE(bodyContains(server, "\"emptyRect\""));
    TEST_ASSERT_TRUE(bodyContains(server, "\"coverageRect\""));
    TEST_ASSERT_TRUE(bodyContains(server, "\"ignored\""));
    TEST_ASSERT_TRUE(bodyContains(server, "\"overlaps\""));
    TEST_ASSERT_TRUE(bodyContains(server, "\"masks\":[]"));
    TEST_ASSERT_TRUE(bodyContains(server, "\"complete\":true"));

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, server.lastBody.c_str());
    TEST_ASSERT_FALSE(err);
    JsonObject elements = doc["elements"].as<JsonObject>();
    TEST_ASSERT_EQUAL(4, elements["bandCells"].as<JsonArray>().size());
    TEST_ASSERT_EQUAL(3, elements["directionArrows"].as<JsonArray>().size());
    TEST_ASSERT_EQUAL(8, elements["mainSignalBars"].as<JsonArray>().size());
    TEST_ASSERT_TRUE(elements["frequency"].is<JsonObject>());
    TEST_ASSERT_EQUAL(2, elements["cardSlots"].as<JsonArray>().size());
    TEST_ASSERT_EQUAL(12, elements["cardMeterBars"].as<JsonArray>().size());
    TEST_ASSERT_TRUE(elements["statusText"].is<JsonArray>());
    TEST_ASSERT_EQUAL(4, elements["statusBadges"].as<JsonArray>().size());
    TEST_ASSERT_TRUE(elements["ignored"].is<JsonArray>());
    TEST_ASSERT_TRUE(doc["masks"].is<JsonArray>());
}

void test_layout_manifest_geometry_matches_host_contract() {
    WebServer server(80);

    WifiDisplayVisualApiService::handleLayout(server, makeRuntime());

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, server.lastBody.c_str());
    TEST_ASSERT_FALSE(err);
    TEST_ASSERT_EQUAL_INT(1, doc["schemaVersion"].as<int>());
    TEST_ASSERT_EQUAL_STRING("test-fw", doc["firmwareVersion"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("test-sha", doc["firmwareSha"].as<const char*>());
    TEST_ASSERT_TRUE(doc["settingsFingerprint"].as<const char*>() != nullptr);

    JsonObject screen = doc["screen"].as<JsonObject>();
    TEST_ASSERT_EQUAL_INT(640, screen["logical"]["width"].as<int>());
    TEST_ASSERT_EQUAL_INT(172, screen["logical"]["height"].as<int>());
    TEST_ASSERT_EQUAL_INT(172, screen["raw"]["width"].as<int>());
    TEST_ASSERT_EQUAL_INT(640, screen["raw"]["height"].as<int>());

    JsonObject elements = doc["elements"].as<JsonObject>();
    JsonArray bandCells = elements["bandCells"].as<JsonArray>();
    JsonArray arrows = elements["directionArrows"].as<JsonArray>();
    JsonArray mainBars = elements["mainSignalBars"].as<JsonArray>();
    JsonArray cardSlots = elements["cardSlots"].as<JsonArray>();
    JsonArray cardBars = elements["cardMeterBars"].as<JsonArray>();
    JsonArray statusText = elements["statusText"].as<JsonArray>();
    JsonArray statusBadges = elements["statusBadges"].as<JsonArray>();
    JsonArray ignored = elements["ignored"].as<JsonArray>();

    TEST_ASSERT_EQUAL_UINT(4, bandCells.size());
    TEST_ASSERT_EQUAL_STRING("laser", bandCells[0]["band"].as<const char*>());
    TEST_ASSERT_EQUAL_INT(BAND_LASER, bandCells[0]["bandMask"].as<int>());
    TEST_ASSERT_EQUAL_STRING("k", bandCells[2]["band"].as<const char*>());
    TEST_ASSERT_EQUAL_INT(BAND_K | BAND_KU, bandCells[2]["bandMask"].as<int>());
    TEST_ASSERT_EQUAL_STRING("x", bandCells[3]["band"].as<const char*>());
    TEST_ASSERT_EQUAL_INT(BAND_X, bandCells[3]["bandMask"].as<int>());
    TEST_ASSERT_EQUAL_INT(82, bandCells[0]["rect"]["x"].as<int>());
    TEST_ASSERT_EQUAL_INT(5, bandCells[0]["rect"]["y"].as<int>());
    TEST_ASSERT_EQUAL_INT(23, bandCells[0]["rect"]["w"].as<int>());
    TEST_ASSERT_EQUAL_INT(34, bandCells[0]["rect"]["h"].as<int>());
    TEST_ASSERT_EQUAL_INT(82, bandCells[2]["rect"]["x"].as<int>());
    TEST_ASSERT_EQUAL_INT(91, bandCells[2]["rect"]["y"].as<int>());
    TEST_ASSERT_EQUAL_INT(30, bandCells[2]["rect"]["w"].as<int>());
    TEST_ASSERT_EQUAL_INT(35, bandCells[2]["rect"]["h"].as<int>());
    TEST_ASSERT_TRUE(
        bandCells[2]["rect"]["x"].as<int>() + bandCells[2]["rect"]["w"].as<int>() <=
        cardSlots[0]["emptyRect"]["x"].as<int>());
    TEST_ASSERT_EQUAL_INT(82, bandCells[3]["rect"]["x"].as<int>());
    TEST_ASSERT_EQUAL_INT(134, bandCells[3]["rect"]["y"].as<int>());
    TEST_ASSERT_EQUAL_INT(30, bandCells[3]["rect"]["w"].as<int>());
    TEST_ASSERT_EQUAL_INT(34, bandCells[3]["rect"]["h"].as<int>());
    assertArrayRectsInBounds(bandCells);

    TEST_ASSERT_EQUAL_UINT(3, arrows.size());
    TEST_ASSERT_EQUAL_STRING("front", arrows[0]["direction"].as<const char*>());
    TEST_ASSERT_EQUAL_INT(DIR_FRONT, arrows[0]["directionMask"].as<int>());
    TEST_ASSERT_EQUAL_STRING("side", arrows[1]["direction"].as<const char*>());
    TEST_ASSERT_EQUAL_INT(DIR_SIDE, arrows[1]["directionMask"].as<int>());
    TEST_ASSERT_EQUAL_STRING("rear", arrows[2]["direction"].as<const char*>());
    TEST_ASSERT_EQUAL_INT(DIR_REAR, arrows[2]["directionMask"].as<int>());
    assertArrayRectsInBounds(arrows);

    TEST_ASSERT_EQUAL_UINT(8, mainBars.size());
    for (size_t i = 0; i < mainBars.size(); ++i) {
        TEST_ASSERT_EQUAL_INT(static_cast<int>(i), mainBars[i]["index"].as<int>());
    }
    assertArrayRectsInBounds(mainBars);

    TEST_ASSERT_TRUE(elements["frequency"].is<JsonObject>());
    assertRectInBounds(elements["frequency"]["rect"]);
    TEST_ASSERT_EQUAL_STRING("frequency", elements["frequency"]["textRole"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("frequencyRole", elements["frequency"]["roleSource"].as<const char*>());

    TEST_ASSERT_EQUAL_UINT(2, cardSlots.size());
    assertArrayRectsInBounds(cardSlots);
    for (JsonVariantConst slot : cardSlots) {
        assertRectInBounds(slot["emptyRect"]);
    }
    TEST_ASSERT_EQUAL_UINT(12, cardBars.size());
    for (size_t i = 0; i < cardBars.size(); ++i) {
        const int slot = static_cast<int>(i / 6);
        const int bar = static_cast<int>(i % 6);
        TEST_ASSERT_EQUAL_INT(slot, cardBars[i]["slot"].as<int>());
        TEST_ASSERT_EQUAL_INT(bar, cardBars[i]["index"].as<int>());
        // The renderer fills active bars but outlines inactive bars.  A one-row
        // shared assertion strip is the only geometry that can truthfully meet
        // the host's near-total coverage rule in both states.
        TEST_ASSERT_EQUAL_INT(1, cardBars[i]["rect"]["h"].as<int>());
    }
    assertArrayRectsInBounds(cardBars);

    TEST_ASSERT_EQUAL_UINT(3, statusText.size());
    TEST_ASSERT_EQUAL_STRING("bogeyChar", statusText[0]["source"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("topCounterRole", statusText[0]["roleSource"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("mainVolume", statusText[1]["source"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("muteVolume", statusText[2]["source"].as<const char*>());
    assertArrayRectsInBounds(statusText);
    TEST_ASSERT_EQUAL_UINT(4, statusBadges.size());
    TEST_ASSERT_EQUAL_STRING("mute", statusBadges[0]["id"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("muteBadgeRole", statusBadges[0]["roleSource"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("muted", statusBadges[0]["source"].as<const char*>());
    assertRectInBounds(statusBadges[0]["coverageRect"]);
    TEST_ASSERT_EQUAL_INT(225, statusBadges[0]["coverageRect"]["x"].as<int>());
    TEST_ASSERT_EQUAL_INT(110, statusBadges[0]["coverageRect"]["w"].as<int>());
    TEST_ASSERT_EQUAL_STRING("alp", statusBadges[1]["id"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("alpBadgeRole", statusBadges[1]["roleSource"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("obd", statusBadges[2]["id"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("obdBadgeRole", statusBadges[2]["roleSource"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("ble", statusBadges[3]["id"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("bleBadgeRole", statusBadges[3]["roleSource"].as<const char*>());
    assertArrayRectsInBounds(statusBadges);
    TEST_ASSERT_EQUAL_UINT(7, ignored.size());
    assertArrayRectsInBounds(ignored);
    TEST_ASSERT_EQUAL_STRING("directionArrowClusterRaised", ignored[0]["id"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("wifiBadge", ignored[1]["id"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("rssi", ignored[2]["id"].as<const char*>());
    TEST_ASSERT_EQUAL_INT(8, ignored[2]["rect"]["x"].as<int>());
    TEST_ASSERT_EQUAL_INT(99, ignored[2]["rect"]["y"].as<int>());
    TEST_ASSERT_EQUAL_INT(70, ignored[2]["rect"]["w"].as<int>());
    TEST_ASSERT_EQUAL_INT(44, ignored[2]["rect"]["h"].as<int>());
    TEST_ASSERT_EQUAL_STRING("profile", ignored[3]["id"].as<const char*>());
    TEST_ASSERT_EQUAL_INT(444, ignored[3]["rect"]["x"].as<int>());
    TEST_ASSERT_EQUAL_INT(196, ignored[3]["rect"]["w"].as<int>());
    TEST_ASSERT_EQUAL_STRING("batteryPercent", ignored[4]["id"].as<const char*>());
    TEST_ASSERT_EQUAL_INT(590, ignored[4]["rect"]["x"].as<int>());
    TEST_ASSERT_EQUAL_INT(30, ignored[4]["rect"]["h"].as<int>());
    TEST_ASSERT_EQUAL_STRING("batteryIcon", ignored[5]["id"].as<const char*>());
    TEST_ASSERT_EQUAL_INT(616, ignored[5]["rect"]["x"].as<int>());
    TEST_ASSERT_EQUAL_INT(129, ignored[5]["rect"]["y"].as<int>());
    TEST_ASSERT_EQUAL_INT(18, ignored[5]["rect"]["w"].as<int>());
    TEST_ASSERT_EQUAL_INT(37, ignored[5]["rect"]["h"].as<int>());
    TEST_ASSERT_EQUAL_STRING("gpsBadge", ignored[6]["id"].as<const char*>());

    JsonArray overlaps = doc["overlaps"].as<JsonArray>();
    TEST_ASSERT_EQUAL_UINT(3, overlaps.size());
    TEST_ASSERT_EQUAL_STRING("profile_battery", overlaps[0]["id"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("profile", overlaps[0]["elements"][0].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("batteryIcon", overlaps[0]["elements"][1].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("band_gps", overlaps[1]["id"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("bandCells[0]", overlaps[1]["elements"][0].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("gpsBadge", overlaps[1]["elements"][1].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("arrow_battery", overlaps[2]["id"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("directionArrowClusterRaised", overlaps[2]["elements"][0].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("batteryPercent", overlaps[2]["elements"][1].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("batteryIcon", overlaps[2]["elements"][2].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("0xF800", doc["palette"]["status"]["obdAttention"].as<const char*>());
    TEST_ASSERT_FALSE(doc["palette"]["status"]["bleAdvertising"].isNull());
    TEST_ASSERT_FALSE(doc["palette"]["status"]["bleConnectedStale"].isNull());
    TEST_ASSERT_TRUE(doc["masks"].as<JsonArray>().size() == 0);
    TEST_ASSERT_TRUE(doc["complete"].as<bool>());
}

void test_steps_and_layout_share_the_same_binding_tuple() {
    WebServer stepsServer(80);
    WebServer layoutServer(80);

    WifiDisplayVisualApiService::handleSteps(stepsServer, makeRuntime());
    WifiDisplayVisualApiService::handleLayout(layoutServer, makeRuntime());

    JsonDocument steps;
    JsonDocument layout;
    TEST_ASSERT_FALSE(deserializeJson(steps, stepsServer.lastBody.c_str()));
    TEST_ASSERT_FALSE(deserializeJson(layout, layoutServer.lastBody.c_str()));
    TEST_ASSERT_EQUAL_INT(steps["schemaVersion"].as<int>(), layout["schemaVersion"].as<int>());
    TEST_ASSERT_EQUAL_STRING(steps["firmwareVersion"].as<const char*>(), layout["firmwareVersion"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING(steps["firmwareSha"].as<const char*>(), layout["firmwareSha"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING(steps["settingsFingerprint"].as<const char*>(), layout["settingsFingerprint"].as<const char*>());
}

void test_every_resolved_role_exists_in_the_layout_palette() {
    WebServer stepsServer(80);
    WebServer layoutServer(80);
    WifiDisplayVisualApiService::handleSteps(stepsServer, makeRuntime());
    WifiDisplayVisualApiService::handleLayout(layoutServer, makeRuntime());

    JsonDocument steps;
    JsonDocument layout;
    TEST_ASSERT_FALSE(deserializeJson(steps, stepsServer.lastBody.c_str()));
    TEST_ASSERT_FALSE(deserializeJson(layout, layoutServer.lastBody.c_str()));
    JsonObjectConst palette = layout["palette"].as<JsonObjectConst>();
    const char* statusRoleFields[] = {
        "topCounterRole",
        "muteBadgeRole",
        "alpBadgeRole",
        "obdBadgeRole",
        "bleBadgeRole",
    };

    for (JsonVariantConst rawStep : steps["steps"].as<JsonArrayConst>()) {
        JsonObjectConst resolved = rawStep["resolved"].as<JsonObjectConst>();
        TEST_ASSERT_TRUE(paletteHasRole(palette, resolved["frequencyRole"].as<const char*>()));
        JsonObjectConst status = resolved["status"].as<JsonObjectConst>();
        for (const char* field : statusRoleFields) {
            TEST_ASSERT_TRUE(paletteHasRole(palette, status[field].as<const char*>()));
        }
    }
}

void test_hidden_settings_remove_volume_assertions_and_ble_expectations() {
    settings.hideVolumeIndicator = true;
    settings.hideBleIcon = true;
    WebServer stepsServer(80);
    WebServer layoutServer(80);
    WifiDisplayVisualApiService::handleSteps(stepsServer, makeRuntime());
    WifiDisplayVisualApiService::handleLayout(layoutServer, makeRuntime());

    JsonDocument steps;
    JsonDocument layout;
    TEST_ASSERT_FALSE(deserializeJson(steps, stepsServer.lastBody.c_str()));
    TEST_ASSERT_FALSE(deserializeJson(layout, layoutServer.lastBody.c_str()));
    JsonArrayConst statusText = layout["elements"]["statusText"].as<JsonArrayConst>();
    TEST_ASSERT_EQUAL_UINT(1, statusText.size());
    TEST_ASSERT_EQUAL_STRING("topCounter", statusText[0]["id"].as<const char*>());

    bool sawEnabledBleStep = false;
    for (JsonVariantConst rawStep : steps["steps"].as<JsonArrayConst>()) {
        JsonObjectConst status = rawStep["resolved"]["status"].as<JsonObjectConst>();
        if (status["bleState"].as<int>() != 0) {
            sawEnabledBleStep = true;
            TEST_ASSERT_EQUAL_STRING("background", status["bleBadgeRole"].as<const char*>());
        }
    }
    TEST_ASSERT_TRUE(sawEnabledBleStep);
}

void test_pin_rate_limited_short_circuits() {
    WebServer server(80);
    server.setArg("plain", "{\"index\":1,\"clear\":true}");

    WifiDisplayVisualApiService::handlePin(
        server,
        makeRuntime(),
        [](void* /*ctx*/) { return false; }, nullptr);

    TEST_ASSERT_EQUAL_INT(0, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(0, display.updateCalls);
}

void test_pin_body_renders_and_framebuffer_reports_contract_headers() {
    WebServer layoutServer(80);
    WifiDisplayVisualApiService::handleLayout(layoutServer, makeRuntime());
    JsonDocument layout;
    TEST_ASSERT_FALSE(deserializeJson(layout, layoutServer.lastBody.c_str()));

    WebServer pinServer(80);
    pinServer.setArg("plain", "{\"index\":10,\"clear\":true}");

    WifiDisplayVisualApiService::handlePin(
        pinServer,
        makeRuntime(),
        [](void* /*ctx*/) { return true; }, nullptr);

    TEST_ASSERT_EQUAL_INT(200, pinServer.lastStatusCode);
    TEST_ASSERT_TRUE(bodyContains(pinServer, "\"success\":true"));
    TEST_ASSERT_TRUE(preview.isVisualPinned());
    TEST_ASSERT_EQUAL_INT(10, preview.pinnedStep());
    TEST_ASSERT_EQUAL_INT(1, display.updateCalls);

    WebServer fbServer(80);
    WifiDisplayVisualApiService::handleFramebuffer(fbServer, makeRuntime());

    TEST_ASSERT_EQUAL_INT(200, fbServer.lastStatusCode);
    TEST_ASSERT_EQUAL_STRING("application/octet-stream", fbServer.lastContentType.c_str());
    TEST_ASSERT_EQUAL_UINT(display.rawFramebufferByteLength(), fbServer.lastContentLength);
    TEST_ASSERT_EQUAL_STRING("172", fbServer.sentHeader("X-FB-Raw-Width").c_str());
    TEST_ASSERT_EQUAL_STRING("640", fbServer.sentHeader("X-FB-Raw-Height").c_str());
    TEST_ASSERT_EQUAL_STRING("640", fbServer.sentHeader("X-FB-Logical-Width").c_str());
    TEST_ASSERT_EQUAL_STRING("172", fbServer.sentHeader("X-FB-Logical-Height").c_str());
    TEST_ASSERT_EQUAL_STRING("RGB565LE", fbServer.sentHeader("X-FB-Format").c_str());
    TEST_ASSERT_EQUAL_STRING("canvas-rotation-1", fbServer.sentHeader("X-FB-Transform").c_str());
    TEST_ASSERT_EQUAL_STRING("1", fbServer.sentHeader("X-Display-Manifest-Schema-Version").c_str());
    TEST_ASSERT_EQUAL_STRING("test-fw", fbServer.sentHeader("X-Display-Firmware-Version").c_str());
    TEST_ASSERT_EQUAL_STRING("test-sha", fbServer.sentHeader("X-Display-Firmware-Sha").c_str());
    TEST_ASSERT_EQUAL_STRING(
        layout["settingsFingerprint"].as<const char*>(),
        fbServer.sentHeader("X-Display-Settings-Fingerprint").c_str());
    TEST_ASSERT_EQUAL_STRING("10", fbServer.sentHeader("X-Display-Pinned-Step").c_str());
    TEST_ASSERT_EQUAL_UINT(display.rawFramebufferByteLength(), fbServer.lastBody.length());
}

void test_framebuffer_fails_closed_when_not_pinned() {
    WebServer server(80);

    WifiDisplayVisualApiService::handleFramebuffer(server, makeRuntime());

    TEST_ASSERT_EQUAL_INT(409, server.lastStatusCode);
    TEST_ASSERT_TRUE(bodyContains(server, "framebuffer_not_pinned"));
}

void test_flushshadow_serves_shadow_bytes_with_contract_headers() {
    WebServer pinServer(80);
    pinServer.setArg("plain", "{\"index\":10,\"clear\":true}");
    WifiDisplayVisualApiService::handlePin(
        pinServer,
        makeRuntime(),
        [](void* /*ctx*/) { return true; }, nullptr);
    TEST_ASSERT_EQUAL_INT(200, pinServer.lastStatusCode);
    TEST_ASSERT_TRUE(display.flushShadowAvailable());

    WebServer server(80);
    WifiDisplayVisualApiService::handleFlushShadow(server, makeRuntime());

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_STRING("application/octet-stream", server.lastContentType.c_str());
    TEST_ASSERT_EQUAL_UINT(display.rawFramebufferByteLength(), server.lastContentLength);
    TEST_ASSERT_EQUAL_UINT(display.rawFramebufferByteLength(), server.lastBody.length());
    TEST_ASSERT_EQUAL_STRING("1", server.sentHeader("X-FB-Shadow").c_str());
    TEST_ASSERT_EQUAL_STRING("172", server.sentHeader("X-FB-Raw-Width").c_str());
    TEST_ASSERT_EQUAL_STRING("640", server.sentHeader("X-FB-Raw-Height").c_str());
    TEST_ASSERT_EQUAL_STRING("canvas-rotation-1", server.sentHeader("X-FB-Transform").c_str());
    TEST_ASSERT_EQUAL_STRING("10", server.sentHeader("X-Display-Pinned-Step").c_str());
}

void test_flushshadow_fails_closed_when_not_pinned_or_unavailable() {
    WebServer notPinned(80);
    WifiDisplayVisualApiService::handleFlushShadow(notPinned, makeRuntime());
    TEST_ASSERT_EQUAL_INT(409, notPinned.lastStatusCode);
    TEST_ASSERT_TRUE(bodyContains(notPinned, "framebuffer_not_pinned"));

    // Allocation failure: pin succeeds, shadow route reports unavailable.
    display.flushShadowAllocFails = true;
    WebServer pinServer(80);
    pinServer.setArg("plain", "{\"index\":10,\"clear\":true}");
    WifiDisplayVisualApiService::handlePin(
        pinServer,
        makeRuntime(),
        [](void* /*ctx*/) { return true; }, nullptr);
    TEST_ASSERT_EQUAL_INT(200, pinServer.lastStatusCode);

    WebServer server(80);
    WifiDisplayVisualApiService::handleFlushShadow(server, makeRuntime());
    TEST_ASSERT_EQUAL_INT(503, server.lastStatusCode);
    TEST_ASSERT_TRUE(bodyContains(server, "flush_shadow_unavailable"));
}

void test_clear_releases_pin_and_restores_maintenance_screen() {
    WebServer pinServer(80);
    pinServer.setArg("plain", "{\"index\":10,\"clear\":true}");
    WifiDisplayVisualApiService::handlePin(
        pinServer,
        makeRuntime(),
        [](void* /*ctx*/) { return true; }, nullptr);
    TEST_ASSERT_TRUE(preview.isVisualPinned());

    WebServer clearServer(80);
    WifiDisplayVisualApiService::handleClear(clearServer, makeRuntime());

    TEST_ASSERT_EQUAL_INT(200, clearServer.lastStatusCode);
    TEST_ASSERT_TRUE(bodyContains(clearServer, "\"success\":true"));
    TEST_ASSERT_TRUE(bodyContains(clearServer, "\"active\":false"));
    TEST_ASSERT_TRUE(bodyContains(clearServer, "\"restored\":true"));
    TEST_ASSERT_FALSE(preview.isVisualPinned());
    TEST_ASSERT_EQUAL_INT(1, display.showMaintenanceModeCalls);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_steps_requires_maintenance_boot);
    RUN_TEST(test_steps_manifest_streams_complete_resolved_payload);
    RUN_TEST(test_layout_manifest_includes_framebuffer_transform_and_palette);
    RUN_TEST(test_layout_manifest_geometry_matches_host_contract);
    RUN_TEST(test_steps_and_layout_share_the_same_binding_tuple);
    RUN_TEST(test_every_resolved_role_exists_in_the_layout_palette);
    RUN_TEST(test_hidden_settings_remove_volume_assertions_and_ble_expectations);
    RUN_TEST(test_pin_rate_limited_short_circuits);
    RUN_TEST(test_pin_body_renders_and_framebuffer_reports_contract_headers);
    RUN_TEST(test_framebuffer_fails_closed_when_not_pinned);
    RUN_TEST(test_flushshadow_serves_shadow_bytes_with_contract_headers);
    RUN_TEST(test_flushshadow_fails_closed_when_not_pinned_or_unavailable);
    RUN_TEST(test_clear_releases_pin_and_restores_maintenance_screen);
    return UNITY_END();
}

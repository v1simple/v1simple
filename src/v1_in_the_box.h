#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <array>
#include <cstdint>

// App-owned audio policy. These ranges do not program detector sweeps.
struct V1InTheBoxBandSettings {
    bool muteOutside = false;
    bool unmuteInside = false;
};

struct V1InTheBoxRange {
    bool enabled = true;
    uint16_t lowerMHz = 0;
    uint16_t upperMHz = 0;
};

struct V1InTheBoxSettings {
    // X, Ku, K, Ka.
    std::array<V1InTheBoxBandSettings, 4> bands{};
    // X, Ku, K, Ka low, Ka middle, Ka high. Edges are inclusive.
    std::array<V1InTheBoxRange, 6> boxes{{
        {true, 10500, 10550}, {true, 13400, 13500}, {true, 24050, 24250},
        {true, 33700, 33900}, {true, 34600, 34800}, {true, 35400, 35600}}};
};

inline bool isValidV1InTheBoxSettings(const V1InTheBoxSettings& settings) {
    constexpr uint16_t lower[] = {10500, 13400, 23900, 33400, 33400, 33400};
    constexpr uint16_t upper[] = {10550, 13500, 24250, 36000, 36000, 36000};
    for (size_t i = 0; i < settings.boxes.size(); ++i) {
        const auto& box = settings.boxes[i];
        if (box.lowerMHz < lower[i] || box.upperMHz > upper[i] ||
            box.lowerMHz > box.upperMHz) return false;
    }
    return true;
}

inline bool v1InTheBoxSettingsEqual(const V1InTheBoxSettings& a, const V1InTheBoxSettings& b) {
    for (size_t i = 0; i < a.bands.size(); ++i) {
        if (a.bands[i].muteOutside != b.bands[i].muteOutside ||
            a.bands[i].unmuteInside != b.bands[i].unmuteInside) return false;
    }
    for (size_t i = 0; i < a.boxes.size(); ++i) {
        if (a.boxes[i].enabled != b.boxes[i].enabled ||
            a.boxes[i].lowerMHz != b.boxes[i].lowerMHz ||
            a.boxes[i].upperMHz != b.boxes[i].upperMHz) return false;
    }
    return true;
}

inline void appendV1InTheBoxSettings(JsonObject target, const V1InTheBoxSettings& settings) {
    constexpr const char* bandNames[] = {"x", "ku", "k", "ka"};
    constexpr const char* boxNames[] = {"x", "ku", "k", "kaLow", "kaMid", "kaHigh"};
    JsonObject bands = target["bands"].to<JsonObject>();
    for (size_t i = 0; i < settings.bands.size(); ++i) {
        JsonObject band = bands[bandNames[i]].to<JsonObject>();
        band["muteOutside"] = settings.bands[i].muteOutside;
        band["unmuteInside"] = settings.bands[i].unmuteInside;
    }
    JsonObject boxes = target["boxes"].to<JsonObject>();
    for (size_t i = 0; i < settings.boxes.size(); ++i) {
        JsonObject box = boxes[boxNames[i]].to<JsonObject>();
        box["enabled"] = settings.boxes[i].enabled;
        box["lowerMHz"] = settings.boxes[i].lowerMHz;
        box["upperMHz"] = settings.boxes[i].upperMHz;
    }
}

inline bool parseV1InTheBoxSettings(JsonVariantConst value, V1InTheBoxSettings& output,
                                    String* error = nullptr) {
    auto fail = [&]() {
        if (error) *error = "Invalid In-the-Box settings";
        return false;
    };
    if (!value.is<JsonObjectConst>()) return fail();
    const JsonObjectConst object = value.as<JsonObjectConst>();
    if (object.size() != 2 || !object["bands"].is<JsonObjectConst>() ||
        !object["boxes"].is<JsonObjectConst>()) return fail();
    const JsonObjectConst bands = object["bands"];
    const JsonObjectConst boxes = object["boxes"];
    if (bands.size() != 4 || boxes.size() != 6) return fail();
    constexpr const char* bandNames[] = {"x", "ku", "k", "ka"};
    constexpr const char* boxNames[] = {"x", "ku", "k", "kaLow", "kaMid", "kaHigh"};
    V1InTheBoxSettings parsed;
    for (size_t i = 0; i < parsed.bands.size(); ++i) {
        JsonObjectConst band = bands[bandNames[i]];
        if (band.isNull() || band.size() != 2 || !band["muteOutside"].is<bool>() ||
            !band["unmuteInside"].is<bool>()) return fail();
        parsed.bands[i] = {band["muteOutside"].as<bool>(), band["unmuteInside"].as<bool>()};
    }
    for (size_t i = 0; i < parsed.boxes.size(); ++i) {
        JsonObjectConst box = boxes[boxNames[i]];
        if (box.isNull() || box.size() != 3 || !box["enabled"].is<bool>() ||
            !box["lowerMHz"].is<uint16_t>() || !box["upperMHz"].is<uint16_t>()) return fail();
        parsed.boxes[i] = {box["enabled"].as<bool>(), box["lowerMHz"].as<uint16_t>(),
                           box["upperMHz"].as<uint16_t>()};
    }
    if (!isValidV1InTheBoxSettings(parsed)) return fail();
    output = parsed;
    return true;
}

#include "wifi_display_visual_api_service.h"

#include <ArduinoJson.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "band_utils.h"
#include "color_themes.h"
#include "display_draw.h"
#include "display_layout.h"
#include "display_visual_contract.h"
#include "json_stream_response.h"
#include "modules/alp/alp_runtime_module.h"

namespace WifiDisplayVisualApiService {

namespace {

constexpr int kManifestSchemaVersion = 1;

struct CountingSink {
    size_t count = 0;
    void write(const char* data, size_t len) {
        if (data && len > 0)
            count += len;
    }
};

template <typename ClientT> struct ClientSink {
    ClientT& client;
    void write(const char* data, size_t len) {
        if (!data || len == 0)
            return;
        client.write(reinterpret_cast<const uint8_t*>(data), len);
    }
};

template <typename Sink> void raw(Sink& sink, const char* text) {
    sink.write(text, std::strlen(text));
}

template <typename Sink> void rawLen(Sink& sink, const char* text, size_t len) {
    sink.write(text, len);
}

template <typename Sink> void quoted(Sink& sink, const char* text) {
    raw(sink, "\"");
    const char* s = text ? text : "";
    for (; *s; ++s) {
        switch (*s) {
        case '"':
            raw(sink, "\\\"");
            break;
        case '\\':
            raw(sink, "\\\\");
            break;
        case '\n':
            raw(sink, "\\n");
            break;
        case '\r':
            raw(sink, "\\r");
            break;
        case '\t':
            raw(sink, "\\t");
            break;
        default:
            rawLen(sink, s, 1);
            break;
        }
    }
    raw(sink, "\"");
}

template <typename Sink> void key(Sink& sink, const char* name) {
    quoted(sink, name);
    raw(sink, ":");
}

template <typename Sink> void number(Sink& sink, long value) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%ld", value);
    raw(sink, buf);
}

template <typename Sink> void unumber(Sink& sink, unsigned long value) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%lu", value);
    raw(sink, buf);
}

template <typename Sink> void boolean(Sink& sink, bool value) {
    raw(sink, value ? "true" : "false");
}

template <typename Sink> void color(Sink& sink, uint16_t value) {
    char buf[10];
    std::snprintf(buf, sizeof(buf), "\"0x%04X\"", value);
    raw(sink, buf);
}

template <typename Sink> void quotedChar(Sink& sink, char value) {
    char buf[2] = {value, '\0'};
    quoted(sink, value ? buf : "");
}

template <typename Sink> void rect(Sink& sink, const DisplayLayout::DisplayRect& r) {
    raw(sink, "{");
    key(sink, "x");
    number(sink, r.x);
    raw(sink, ",");
    key(sink, "y");
    number(sink, r.y);
    raw(sink, ",");
    key(sink, "w");
    number(sink, r.w);
    raw(sink, ",");
    key(sink, "h");
    number(sink, r.h);
    raw(sink, "}");
}

const char* directionName(Direction dir) {
    switch (dir) {
    case DIR_FRONT:
        return "front";
    case DIR_SIDE:
        return "side";
    case DIR_REAR:
        return "rear";
    case DIR_NONE:
        return "none";
    }
    return "mixed";
}

void mixByte(uint32_t& h, uint8_t value) {
    h ^= value;
    h *= 16777619UL;
}

void mix16(uint32_t& h, uint16_t value) {
    mixByte(h, static_cast<uint8_t>(value & 0xFF));
    mixByte(h, static_cast<uint8_t>(value >> 8));
}

void mixBool(uint32_t& h, bool value) {
    mixByte(h, value ? 1 : 0);
}

void mixString(uint32_t& h, const String& value) {
    for (size_t i = 0; i < value.length(); ++i) {
        mixByte(h, static_cast<uint8_t>(value[i]));
    }
    mixByte(h, 0);
}

uint32_t settingsFingerprint(const V1Settings& s) {
    uint32_t h = 2166136261UL;
    mix16(h, s.colorBogey);
    mix16(h, s.colorFrequency);
    mix16(h, s.colorArrowFront);
    mix16(h, s.colorArrowSide);
    mix16(h, s.colorArrowRear);
    mix16(h, s.colorBandL);
    mix16(h, s.colorBandKa);
    mix16(h, s.colorBandK);
    mix16(h, s.colorBandX);
    mix16(h, s.colorBandPhoto);
    mix16(h, s.colorWiFiIcon);
    mix16(h, s.colorWiFiConnected);
    mix16(h, s.colorBleConnected);
    mix16(h, s.colorBleDisconnected);
    mix16(h, s.colorBar1);
    mix16(h, s.colorBar2);
    mix16(h, s.colorBar3);
    mix16(h, s.colorBar4);
    mix16(h, s.colorBar5);
    mix16(h, s.colorBar6);
    mix16(h, s.colorMuted);
    mix16(h, s.colorPersisted);
    mix16(h, s.colorVolumeMain);
    mix16(h, s.colorVolumeMute);
    mix16(h, s.colorRssiV1);
    mix16(h, s.colorRssiProxy);
    mix16(h, s.colorObd);
    mix16(h, s.colorAlpConnected);
    mix16(h, s.colorAlpDli);
    mix16(h, s.colorAlpLidActive);
    mix16(h, s.colorAlpAlert);
    mixBool(h, s.freqUseBandColor);
    mixBool(h, s.hideWifiIcon);
    mixBool(h, s.hideProfileIndicator);
    mixBool(h, s.hideBatteryIcon);
    mixBool(h, s.showBatteryPercent);
    mixBool(h, s.hideBleIcon);
    mixBool(h, s.hideVolumeIndicator);
    mixBool(h, s.hideRssiIndicator);
    mixString(h, s.slot0Name);
    mixString(h, s.slot1Name);
    mixString(h, s.slot2Name);
    mix16(h, s.slot0Color);
    mix16(h, s.slot1Color);
    mix16(h, s.slot2Color);
    return h;
}

void settingsFingerprintHex(const V1Settings& settings, char* out, size_t outSize) {
    std::snprintf(out, outSize, "0x%08lX", static_cast<unsigned long>(settingsFingerprint(settings)));
}

template <typename Sink> void bindingFields(Sink& sink, const Runtime& runtime, const V1Settings& settings) {
    key(sink, "schemaVersion");
    number(sink, kManifestSchemaVersion);
    raw(sink, ",");
    key(sink, "firmwareVersion");
    quoted(sink, runtime.firmwareVersion ? runtime.firmwareVersion : "unknown");
    raw(sink, ",");
    key(sink, "firmwareSha");
    quoted(sink, runtime.firmwareSha ? runtime.firmwareSha : "unknown");
    raw(sink, ",");
    key(sink, "settingsFingerprint");
    char buf[12];
    settingsFingerprintHex(settings, buf, sizeof(buf));
    quoted(sink, buf);
}

bool requireMaintenance(WebServer& server, const Runtime& runtime) {
    if (runtime.maintenanceBootActive) {
        return true;
    }
    server.send(403, "application/json", "{\"success\":false,\"error\":\"maintenance_required\"}");
    return false;
}

bool requireRuntime(WebServer& server, const Runtime& runtime, bool needDisplay) {
    if (!runtime.preview || !runtime.getSettings || (needDisplay && !runtime.display)) {
        server.send(503, "application/json", "{\"success\":false,\"error\":\"visual_runtime_unavailable\"}");
        return false;
    }
    return true;
}

template <typename PayloadFn> void sendMeasuredJson(WebServer& server, PayloadFn payload) {
    CountingSink counter;
    payload(counter);
    server.setContentLength(counter.count);
    server.send(200, "application/json", "");
    auto client = server.client();
    ClientSink<decltype(client)> sink{client};
    payload(sink);
}

void stepId(const DisplayPreviewModule::ResolvedStep& step, char* out, size_t outSize) {
    const char* band = "none";
    switch (step.primary.band) {
    case BAND_LASER:
        band = "laser";
        break;
    case BAND_KA:
        band = "ka";
        break;
    case BAND_K:
        band = "k";
        break;
    case BAND_X:
        band = "x";
        break;
    case BAND_KU:
        band = "ku";
        break;
    case BAND_NONE:
        band = "none";
        break;
    }
    std::snprintf(out, outSize, "%03d_%s_%s_%lu", step.index, band, directionName(step.primary.dir),
                  static_cast<unsigned long>(step.primary.freqMHz));
}

template <typename Sink> void writeFlags(Sink& sink, uint8_t flags) {
    raw(sink, "{");
    key(sink, "muted");
    boolean(sink, (flags & DisplayPreviewModule::FLAG_MUTED) != 0);
    raw(sink, ",");
    key(sink, "photo");
    boolean(sink, (flags & DisplayPreviewModule::FLAG_PHOTO) != 0);
    raw(sink, ",");
    key(sink, "flashArrow");
    boolean(sink, (flags & DisplayPreviewModule::FLAG_FLASH_ARROW) != 0);
    raw(sink, ",");
    key(sink, "flashBand");
    boolean(sink, (flags & DisplayPreviewModule::FLAG_FLASH_BAND) != 0);
    raw(sink, "}");
}

template <typename Sink> void writeRawStep(Sink& sink, const DisplayPreviewModule::PreviewStep& rawStep) {
    raw(sink, "{");
    key(sink, "band");
    quoted(sink, bandName(rawStep.band));
    raw(sink, ",");
    key(sink, "direction");
    quoted(sink, directionName(rawStep.dir));
    raw(sink, ",");
    key(sink, "frequencyMHz");
    unumber(sink, rawStep.freqMHz);
    raw(sink, ",");
    key(sink, "frontBars");
    unumber(sink, rawStep.frontBars);
    raw(sink, ",");
    key(sink, "rearBars");
    unumber(sink, rawStep.rearBars);
    raw(sink, ",");
    key(sink, "flags");
    writeFlags(sink, rawStep.flags);
    raw(sink, ",");
    key(sink, "secondaryBand");
    quoted(sink, bandName(rawStep.secBand));
    raw(sink, ",");
    key(sink, "secondaryDirection");
    quoted(sink, directionName(rawStep.secDir));
    raw(sink, ",");
    key(sink, "secondaryFrequencyMHz");
    unumber(sink, rawStep.secFreqMHz);
    raw(sink, ",");
    key(sink, "thirdBand");
    quoted(sink, bandName(rawStep.thirdBand));
    raw(sink, ",");
    key(sink, "thirdDirection");
    quoted(sink, directionName(rawStep.thirdDir));
    raw(sink, ",");
    key(sink, "thirdFrequencyMHz");
    unumber(sink, rawStep.thirdFreqMHz);
    raw(sink, ",");
    key(sink, "bogeyChar");
    number(sink, rawStep.bogeyChar);
    raw(sink, ",");
    key(sink, "modeChar");
    number(sink, rawStep.modeChar);
    raw(sink, ",");
    key(sink, "profileSlot");
    number(sink, rawStep.profileSlot);
    raw(sink, ",");
    key(sink, "alpState");
    number(sink, rawStep.alpState);
    raw(sink, ",");
    key(sink, "alpHbByte1");
    number(sink, rawStep.alpHbByte1);
    raw(sink, ",");
    key(sink, "obdState");
    number(sink, rawStep.obdState);
    raw(sink, ",");
    key(sink, "bleState");
    number(sink, rawStep.bleState);
    raw(sink, ",");
    key(sink, "mainVolume");
    number(sink, rawStep.mainVolume);
    raw(sink, ",");
    key(sink, "muteVolume");
    number(sink, rawStep.muteVolume);
    raw(sink, ",");
    key(sink, "alpGunAbbrev");
    quoted(sink, rawStep.alpGunAbbrev ? rawStep.alpGunAbbrev : "");
    raw(sink, "}");
}

template <typename Sink> void writeResolvedAlert(Sink& sink, const DisplayPreviewModule::ResolvedAlert& alert) {
    raw(sink, "{");
    key(sink, "present");
    boolean(sink, alert.present);
    raw(sink, ",");
    key(sink, "band");
    quoted(sink, bandName(alert.band));
    raw(sink, ",");
    key(sink, "direction");
    quoted(sink, directionName(alert.dir));
    raw(sink, ",");
    key(sink, "frequencyMHz");
    unumber(sink, alert.freqMHz);
    raw(sink, ",");
    key(sink, "frequencyText");
    quoted(sink, alert.frequencyText);
    raw(sink, ",");
    key(sink, "frontBars");
    unumber(sink, alert.frontBars);
    raw(sink, ",");
    key(sink, "rearBars");
    unumber(sink, alert.rearBars);
    raw(sink, ",");
    key(sink, "cardBarCount");
    unumber(sink, alert.cardBarCount);
    raw(sink, "}");
}

template <typename Sink>
void writeResolvedStep(Sink& sink, const DisplayPreviewModule::ResolvedStep& step, const V1Settings& settings) {
    auto bandPaletteRole = [](Band band) -> const char* {
        switch (band) {
        case BAND_LASER:
            return "bands.laser";
        case BAND_KA:
            return "bands.ka";
        case BAND_K:
        case BAND_KU:
            return "bands.k";
        case BAND_X:
            return "bands.x";
        case BAND_NONE:
            return "gray";
        }
        return "gray";
    };

    const char* frequencyRole = "frequency";
    if (step.raw.alpGunAbbrev && step.raw.alpGunAbbrev[0] != '\0') {
        // Preview gun abbreviations call setAlpFrequencyOverride() with its
        // default lidActive=true argument.
        frequencyRole = step.muted ? "muted" : "status.alpLidActive";
    } else if (step.primary.band == BAND_LASER) {
        frequencyRole = step.muted ? "muted" : "bands.laser";
    } else if (step.muted) {
        frequencyRole = "muted";
    } else if (step.primary.freqMHz == 0) {
        frequencyRole = "gray";
    } else if (step.photo && settings.freqUseBandColor) {
        frequencyRole = "bands.photo";
    } else if (settings.freqUseBandColor) {
        frequencyRole = bandPaletteRole(step.primary.band);
    }

    const bool topCounterIsDigit = step.status.bogeyChar >= '0' && step.status.bogeyChar <= '9';
    const char* topCounterRole = (!topCounterIsDigit && step.muted) ? "muted" : "bogey";

    const AlpState alpState = static_cast<AlpState>(step.status.alpState);
    const char* alpBadgeRole = "background";
    switch (alpState) {
    case AlpState::OFF:
        alpBadgeRole = "background";
        break;
    case AlpState::IDLE:
        alpBadgeRole = "muted";
        break;
    case AlpState::LISTENING:
        if (step.status.alpHbByte1 == 0x04) {
            alpBadgeRole = "status.alpLidActive";
        } else if (step.status.alpHbByte1 == 0x03) {
            alpBadgeRole = "status.alpDli";
        } else {
            alpBadgeRole = "status.alpConnected";
        }
        break;
    case AlpState::ALERT_ACTIVE:
    case AlpState::NOISE_WINDOW:
    case AlpState::TEARDOWN:
        alpBadgeRole = "status.alpAlert";
        break;
    }

    const char* obdBadgeRole = "background";
    if (step.status.obdState == 1) {
        obdBadgeRole = "status.obd";
    } else if (step.status.obdState == 2) {
        obdBadgeRole = "status.obdAttention";
    }

    const char* bleBadgeRole = "background";
    if (!settings.hideBleIcon) {
        if (step.status.bleState == 1) {
            bleBadgeRole = "status.bleAdvertising";
        } else if (step.status.bleState == 2) {
            // Visual preview pins make the connected freshness state deterministic.
            bleBadgeRole = "status.bleConnected";
        }
    }

    char id[48];
    stepId(step, id, sizeof(id));
    raw(sink, "{");
    key(sink, "index");
    number(sink, step.index);
    raw(sink, ",");
    key(sink, "id");
    quoted(sink, id);
    raw(sink, ",");
    key(sink, "raw");
    writeRawStep(sink, step.raw);
    raw(sink, ",");
    key(sink, "resolved");
    raw(sink, "{");
    key(sink, "primary");
    writeResolvedAlert(sink, step.primary);
    raw(sink, ",");
    key(sink, "secondary");
    writeResolvedAlert(sink, step.secondary);
    raw(sink, ",");
    key(sink, "third");
    writeResolvedAlert(sink, step.third);
    raw(sink, ",");
    key(sink, "activeBandMask");
    unumber(sink, step.activeBandMask);
    raw(sink, ",");
    key(sink, "activeDirectionMask");
    unumber(sink, step.activeDirectionMask);
    raw(sink, ",");
    key(sink, "flashMask");
    unumber(sink, step.flashMask);
    raw(sink, ",");
    key(sink, "bandFlashMask");
    unumber(sink, step.bandFlashMask);
    raw(sink, ",");
    key(sink, "mainMeterCount");
    unumber(sink, step.mainMeterCount);
    raw(sink, ",");
    key(sink, "alertCount");
    unumber(sink, step.alertCount);
    raw(sink, ",");
    key(sink, "muted");
    boolean(sink, step.muted);
    raw(sink, ",");
    key(sink, "photo");
    boolean(sink, step.photo);
    raw(sink, ",");
    key(sink, "frequencyRole");
    quoted(sink, frequencyRole);
    raw(sink, ",");
    key(sink, "status");
    raw(sink, "{");
    key(sink, "bogeyChar");
    quotedChar(sink, step.status.bogeyChar);
    raw(sink, ",");
    key(sink, "modeChar");
    quotedChar(sink, step.status.hasMode ? step.status.modeChar : 0);
    raw(sink, ",");
    key(sink, "profileSlot");
    number(sink, step.status.profileSlot);
    raw(sink, ",");
    key(sink, "alpState");
    number(sink, step.status.alpState);
    raw(sink, ",");
    key(sink, "alpHbByte1");
    unumber(sink, step.status.alpHbByte1);
    raw(sink, ",");
    key(sink, "obdState");
    number(sink, step.status.obdState);
    raw(sink, ",");
    key(sink, "bleState");
    number(sink, step.status.bleState);
    raw(sink, ",");
    key(sink, "mainVolume");
    unumber(sink, step.status.mainVolume);
    raw(sink, ",");
    key(sink, "muteVolume");
    unumber(sink, step.status.muteVolume);
    raw(sink, ",");
    key(sink, "muted");
    boolean(sink, step.muted);
    raw(sink, ",");
    key(sink, "topCounterRole");
    quoted(sink, topCounterRole);
    raw(sink, ",");
    key(sink, "muteBadgeRole");
    quoted(sink, step.muted ? "muted" : "background");
    raw(sink, ",");
    key(sink, "alpBadgeRole");
    quoted(sink, alpBadgeRole);
    raw(sink, ",");
    key(sink, "obdBadgeRole");
    quoted(sink, obdBadgeRole);
    raw(sink, ",");
    key(sink, "bleBadgeRole");
    quoted(sink, bleBadgeRole);
    raw(sink, "}}}");
}

template <typename Sink> void writeStepsPayload(Sink& sink, const Runtime& runtime, const V1Settings& settings) {
    raw(sink, "{");
    bindingFields(sink, runtime, settings);
    raw(sink, ",");
    key(sink, "manifest");
    quoted(sink, "display-visual-steps");
    raw(sink, ",");
    key(sink, "stepCount");
    number(sink, DisplayPreviewModule::stepCount());
    raw(sink, ",");
    key(sink, "steps");
    raw(sink, "[");
    for (int i = 0; i < DisplayPreviewModule::stepCount(); ++i) {
        DisplayPreviewModule::ResolvedStep step;
        DisplayPreviewModule::resolveStep(i, step);
        if (i > 0)
            raw(sink, ",");
        writeResolvedStep(sink, step, settings);
    }
    raw(sink, "],");
    key(sink, "complete");
    boolean(sink, true);
    raw(sink, "}");
}

template <typename Sink> void writePalettePayload(Sink& sink, const V1Settings& s) {
    const ColorPalette& base = ColorThemes::STANDARD();
    uint16_t configured[6] = {s.colorBar1, s.colorBar2, s.colorBar3, s.colorBar4, s.colorBar5, s.colorBar6};
    uint16_t ramp[8]{};
    DisplayVisualContract::buildMainMeterRamp(configured, ramp);

    key(sink, "palette");
    raw(sink, "{");
    key(sink, "background");
    color(sink, base.bg);
    raw(sink, ",");
    key(sink, "text");
    color(sink, base.text);
    raw(sink, ",");
    key(sink, "gray");
    color(sink, base.colorGray);
    raw(sink, ",");
    key(sink, "frequency");
    color(sink, s.colorFrequency);
    raw(sink, ",");
    key(sink, "bogey");
    color(sink, s.colorBogey);
    raw(sink, ",");
    key(sink, "muted");
    color(sink, s.colorMuted);
    raw(sink, ",");
    key(sink, "persisted");
    color(sink, s.colorPersisted);
    raw(sink, ",");
    key(sink, "bands");
    raw(sink, "{");
    key(sink, "laser");
    color(sink, s.colorBandL);
    raw(sink, ",");
    key(sink, "ka");
    color(sink, s.colorBandKa);
    raw(sink, ",");
    key(sink, "k");
    color(sink, s.colorBandK);
    raw(sink, ",");
    key(sink, "x");
    color(sink, s.colorBandX);
    raw(sink, ",");
    key(sink, "photo");
    color(sink, s.colorBandPhoto);
    raw(sink, "},");
    key(sink, "arrows");
    raw(sink, "{");
    key(sink, "front");
    color(sink, s.colorArrowFront);
    raw(sink, ",");
    key(sink, "side");
    color(sink, s.colorArrowSide);
    raw(sink, ",");
    key(sink, "rear");
    color(sink, s.colorArrowRear);
    raw(sink, "},");
    key(sink, "mainMeterRamp");
    raw(sink, "[");
    for (int i = 0; i < 8; ++i) {
        if (i > 0)
            raw(sink, ",");
        color(sink, ramp[i]);
    }
    raw(sink, "],");
    key(sink, "cardMeterRamp");
    raw(sink, "[");
    for (int i = 0; i < 6; ++i) {
        if (i > 0)
            raw(sink, ",");
        color(sink, configured[i]);
    }
    raw(sink, "],");
    key(sink, "cardMeterDimRamp");
    raw(sink, "[");
    for (int i = 0; i < 6; ++i) {
        if (i > 0)
            raw(sink, ",");
        color(sink, dimColor(configured[i], 30));
    }
    raw(sink, "],");
    key(sink, "status");
    raw(sink, "{");
    key(sink, "profile0");
    color(sink, s.slot0Color);
    raw(sink, ",");
    key(sink, "profile1");
    color(sink, s.slot1Color);
    raw(sink, ",");
    key(sink, "profile2");
    color(sink, s.slot2Color);
    raw(sink, ",");
    key(sink, "wifiIcon");
    color(sink, s.colorWiFiIcon);
    raw(sink, ",");
    key(sink, "wifiConnected");
    color(sink, s.colorWiFiConnected);
    raw(sink, ",");
    key(sink, "bleConnected");
    color(sink, dimColor(s.colorBleConnected, 85));
    raw(sink, ",");
    key(sink, "bleConnectedStale");
    color(sink, dimColor(s.colorBleConnected, 40));
    raw(sink, ",");
    key(sink, "bleDisconnected");
    color(sink, dimColor(s.colorBleDisconnected, 85));
    raw(sink, ",");
    key(sink, "bleAdvertising");
    color(sink, dimColor(s.colorBleDisconnected, 85));
    raw(sink, ",");
    key(sink, "volumeMain");
    color(sink, s.colorVolumeMain);
    raw(sink, ",");
    key(sink, "volumeMute");
    color(sink, s.colorVolumeMute);
    raw(sink, ",");
    key(sink, "rssiV1");
    color(sink, s.colorRssiV1);
    raw(sink, ",");
    key(sink, "rssiProxy");
    color(sink, s.colorRssiProxy);
    raw(sink, ",");
    key(sink, "obd");
    color(sink, s.colorObd);
    raw(sink, ",");
    key(sink, "obdAttention");
    color(sink, 0xF800);
    raw(sink, ",");
    key(sink, "alpConnected");
    color(sink, s.colorAlpConnected);
    raw(sink, ",");
    key(sink, "alpDli");
    color(sink, s.colorAlpDli);
    raw(sink, ",");
    key(sink, "alpLidActive");
    color(sink, s.colorAlpLidActive);
    raw(sink, ",");
    key(sink, "alpAlert");
    color(sink, s.colorAlpAlert);
    raw(sink, "}}");
}

template <typename Sink> void writeZone(Sink& sink, const char* id, const DisplayLayout::DisplayRect& r, bool comma) {
    if (comma)
        raw(sink, ",");
    raw(sink, "{");
    key(sink, "id");
    quoted(sink, id);
    raw(sink, ",");
    key(sink, "rect");
    rect(sink, r);
    raw(sink, ",");
    key(sink, "role");
    quoted(sink, "semantic_zone");
    raw(sink, "}");
}

const char* bandRole(int index) {
    switch (index) {
    case 0:
        return "bands.laser";
    case 1:
        return "bands.ka";
    case 2:
        return "bands.k";
    case 3:
        return "bands.x";
    }
    return "bands.k";
}

const char* bandKey(int index) {
    switch (index) {
    case 0:
        return "laser";
    case 1:
        return "ka";
    case 2:
        return "k";
    case 3:
        return "x";
    }
    return "k";
}

template <typename Sink> void writeStringField(Sink& sink, const char* name, const char* value, bool comma = true) {
    key(sink, name);
    quoted(sink, value);
    if (comma)
        raw(sink, ",");
}

template <typename Sink>
void writeRectField(Sink& sink, const char* name, const DisplayLayout::DisplayRect& r, bool comma = true) {
    key(sink, name);
    rect(sink, r);
    if (comma)
        raw(sink, ",");
}

template <typename Sink>
void writeIgnoredElement(Sink& sink, const char* id, const DisplayLayout::DisplayRect& r, bool comma) {
    if (comma)
        raw(sink, ",");
    raw(sink, "{");
    writeStringField(sink, "id", id);
    writeRectField(sink, "rect", r);
    writeStringField(sink, "role", "ignored", false);
    raw(sink, "}");
}

template <typename Sink>
void writeStatusTextElement(Sink& sink, const char* id, const char* source, const DisplayLayout::DisplayRect& r,
                            const char* textRole, const char* roleSource, const char* minCoverage,
                            const char* maxCoverage, bool comma) {
    if (comma)
        raw(sink, ",");
    raw(sink, "{");
    writeStringField(sink, "id", id);
    writeStringField(sink, "source", source);
    writeRectField(sink, "rect", r);
    writeStringField(sink, "textRole", textRole);
    if (roleSource && roleSource[0] != '\0') {
        writeStringField(sink, "roleSource", roleSource);
    }
    key(sink, "minCoverage");
    raw(sink, minCoverage);
    raw(sink, ",");
    key(sink, "maxCoverage");
    raw(sink, maxCoverage);
    raw(sink, "}");
}

template <typename Sink> void writeLayoutPayload(Sink& sink, const Runtime& runtime, const V1Settings& settings) {
    raw(sink, "{");
    bindingFields(sink, runtime, settings);
    raw(sink, ",");
    key(sink, "manifest");
    quoted(sink, "display-visual-layout");
    raw(sink, ",");
    key(sink, "screen");
    raw(sink, "{");
    key(sink, "logical");
    raw(sink, "{");
    key(sink, "width");
    number(sink, V1Display::logicalFramebufferWidth());
    raw(sink, ",");
    key(sink, "height");
    number(sink, V1Display::logicalFramebufferHeight());
    raw(sink, "},");
    key(sink, "raw");
    raw(sink, "{");
    key(sink, "width");
    number(sink, V1Display::rawFramebufferWidth());
    raw(sink, ",");
    key(sink, "height");
    number(sink, V1Display::rawFramebufferHeight());
    raw(sink, ",");
    key(sink, "format");
    quoted(sink, V1Display::rawFramebufferFormat());
    raw(sink, ",");
    key(sink, "transform");
    quoted(sink, V1Display::rawFramebufferTransform());
    raw(sink, "}},");
    key(sink, "coarseZones");
    raw(sink, "[");
    writeZone(sink, "frequency", DisplayLayout::kFrequencyZoneRect, false);
    writeZone(sink, "bands", DisplayLayout::kBandsColumnRect, true);
    writeZone(sink, "mainSignalBars", DisplayLayout::kSignalBarsRect, true);
    writeZone(sink, "cards", DisplayLayout::kSecondaryCardsRect, true);
    writeZone(sink, "topCounter", DisplayLayout::kTopCounterRect, true);
    raw(sink, "],");
    key(sink, "elements");
    raw(sink, "{");
    key(sink, "bandCells");
    raw(sink, "[");
    for (int i = 0; i < 4; ++i) {
        if (i > 0)
            raw(sink, ",");
        raw(sink, "{");
        key(sink, "index");
        number(sink, i);
        raw(sink, ",");
        key(sink, "label");
        quoted(sink, DisplayVisualContract::bandCellLabel(0, i));
        raw(sink, ",");
        writeStringField(sink, "band", bandKey(i));
        const uint8_t bandMask =
            (i == 2) ? static_cast<uint8_t>(BAND_K | BAND_KU) : DisplayVisualContract::bandCellMask(0, i);
        key(sink, "bandMask");
        number(sink, bandMask);
        raw(sink, ",");
        writeRectField(sink, "rect", DisplayLayout::bandCellAssertRect(i));
        writeStringField(sink, "role", "anti_aliased_text");
        writeStringField(sink, "activeRole", bandRole(i));
        writeStringField(sink, "inactiveRole", "gray");
        key(sink, "minCoverage");
        raw(sink, "0.01");
        raw(sink, ",");
        key(sink, "inactiveMinCoverage");
        raw(sink, "0.01");
        raw(sink, "}");
    }
    raw(sink, "],");
    key(sink, "directionArrows");
    raw(sink, "[");
    const uint8_t arrowMasks[3] = {DIR_FRONT, DIR_SIDE, DIR_REAR};
    const char* arrowIds[3] = {"front", "side", "rear"};
    for (int i = 0; i < 3; ++i) {
        if (i > 0)
            raw(sink, ",");
        raw(sink, "{");
        writeStringField(sink, "direction", arrowIds[i]);
        key(sink, "directionMask");
        number(sink, arrowMasks[i]);
        raw(sink, ",");
        writeRectField(sink, "rect", DisplayLayout::directionArrowAssertRect(arrowMasks[i], true));
        writeStringField(sink, "role", "active_fill");
        char activeRole[24];
        std::snprintf(activeRole, sizeof(activeRole), "arrows.%s", arrowIds[i]);
        writeStringField(sink, "activeRole", activeRole);
        writeStringField(sink, "inactiveRole", "gray", false);
        raw(sink, "}");
    }
    raw(sink, "],");
    key(sink, "mainSignalBars");
    raw(sink, "[");
    for (int i = 0; i < 8; ++i) {
        if (i > 0)
            raw(sink, ",");
        raw(sink, "{");
        key(sink, "index");
        number(sink, i);
        raw(sink, ",");
        writeRectField(sink, "rect", DisplayLayout::mainSignalBarRect(i));
        writeStringField(sink, "role", "active_fill");
        char activeRole[24];
        std::snprintf(activeRole, sizeof(activeRole), "mainMeterRamp.%d", i);
        writeStringField(sink, "activeRole", activeRole);
        writeStringField(sink, "inactiveRole", "gray", false);
        raw(sink, "}");
    }
    raw(sink, "],");
    key(sink, "frequency");
    raw(sink, "{");
    writeRectField(sink, "rect", DisplayLayout::frequencyTextRect());
    writeStringField(sink, "role", "anti_aliased_text");
    writeStringField(sink, "textRole", "frequency");
    writeStringField(sink, "roleSource", "frequencyRole");
    key(sink, "minCoverage");
    raw(sink, "0.02");
    raw(sink, ",");
    key(sink, "maxCoverage");
    raw(sink, "0.70");
    raw(sink, "},");
    key(sink, "cardSlots");
    raw(sink, "[");
    for (int i = 0; i < 2; ++i) {
        if (i > 0)
            raw(sink, ",");
        raw(sink, "{");
        key(sink, "slot");
        number(sink, i);
        raw(sink, ",");
        writeRectField(sink, "rect", DisplayLayout::cardTextRect(i));
        writeRectField(sink, "emptyRect", DisplayLayout::cardRect(i));
        writeStringField(sink, "role", "anti_aliased_text");
        writeStringField(sink, "textRole", "text");
        key(sink, "minCoverage");
        raw(sink, "0.02");
        raw(sink, ",");
        key(sink, "maxCoverage");
        raw(sink, "0.85");
        raw(sink, "}");
    }
    raw(sink, "],");
    key(sink, "cardMeterBars");
    raw(sink, "[");
    for (int slot = 0; slot < 2; ++slot) {
        for (int i = 0; i < 6; ++i) {
            if (slot > 0 || i > 0)
                raw(sink, ",");
            raw(sink, "{");
            key(sink, "slot");
            number(sink, slot);
            raw(sink, ",");
            key(sink, "index");
            number(sink, i);
            raw(sink, ",");
            writeRectField(sink, "rect", DisplayLayout::cardMeterAssertRect(slot, i));
            writeStringField(sink, "role", "active_fill");
            char activeRole[24];
            std::snprintf(activeRole, sizeof(activeRole), "cardMeterRamp.%d", i);
            writeStringField(sink, "activeRole", activeRole);
            char inactiveRole[28];
            std::snprintf(inactiveRole, sizeof(inactiveRole), "cardMeterDimRamp.%d", i);
            writeStringField(sink, "inactiveRole", inactiveRole, false);
            raw(sink, "}");
        }
    }
    raw(sink, "],");
    key(sink, "statusText");
    raw(sink, "[");
    writeStatusTextElement(sink, "topCounter", "bogeyChar", DisplayLayout::kTopCounterRect, "bogey", "topCounterRole",
                           "0.02", "0.80", false);
    if (!settings.hideVolumeIndicator) {
        writeStatusTextElement(sink, "volumeMain", "mainVolume", DisplayLayout::volumeMainRect(), "status.volumeMain",
                               nullptr, "0.02", "0.80", true);
        writeStatusTextElement(sink, "volumeMute", "muteVolume", DisplayLayout::volumeMuteRect(), "status.volumeMute",
                               nullptr, "0.02", "0.80", true);
    }
    raw(sink, "],");
    key(sink, "statusBadges");
    raw(sink, "[");
    raw(sink, "{");
    writeStringField(sink, "id", "mute");
    writeStringField(sink, "source", "muted");
    writeRectField(sink, "rect", DisplayLayout::muteBadgeAssertRect());
    writeRectField(sink, "coverageRect", DisplayLayout::muteBadgeRect());
    writeStringField(sink, "role", "active_fill");
    writeStringField(sink, "roleSource", "muteBadgeRole");
    writeStringField(sink, "activeRole", "muted");
    writeStringField(sink, "inactiveRole", "background");
    writeStringField(sink, "activeWhen", "nonzero");
    key(sink, "minCoverage");
    raw(sink, "0.90");
    raw(sink, ",");
    key(sink, "inactiveMinCoverage");
    raw(sink, "0.98");
    raw(sink, "},");
    raw(sink, "{");
    writeStringField(sink, "id", "alp");
    writeStringField(sink, "source", "alpState");
    writeRectField(sink, "rect", DisplayLayout::alpBadgeRect());
    writeStringField(sink, "role", "anti_aliased_text");
    writeStringField(sink, "roleSource", "alpBadgeRole");
    writeStringField(sink, "activeRole", "status.alpConnected");
    writeStringField(sink, "inactiveRole", "background");
    writeStringField(sink, "activeWhen", "nonzero");
    key(sink, "minCoverage");
    raw(sink, "0.02");
    raw(sink, ",");
    key(sink, "inactiveMinCoverage");
    raw(sink, "0.98");
    raw(sink, "},");
    raw(sink, "{");
    writeStringField(sink, "id", "obd");
    writeStringField(sink, "source", "obdState");
    writeRectField(sink, "rect", DisplayLayout::obdBadgeRect());
    writeStringField(sink, "role", "anti_aliased_text");
    writeStringField(sink, "roleSource", "obdBadgeRole");
    writeStringField(sink, "activeRole", "status.obd");
    writeStringField(sink, "inactiveRole", "background");
    writeStringField(sink, "activeWhen", "nonzero");
    key(sink, "minCoverage");
    raw(sink, "0.02");
    raw(sink, ",");
    key(sink, "inactiveMinCoverage");
    raw(sink, "0.98");
    raw(sink, "},");
    raw(sink, "{");
    writeStringField(sink, "id", "ble");
    writeStringField(sink, "source", "bleState");
    writeRectField(sink, "rect", DisplayLayout::bleBadgeRect());
    writeStringField(sink, "role", "line_glyph");
    writeStringField(sink, "roleSource", "bleBadgeRole");
    writeStringField(sink, "activeRole", "status.bleAdvertising");
    writeStringField(sink, "inactiveRole", "background");
    writeStringField(sink, "activeWhen", "nonzero");
    key(sink, "minCoverage");
    raw(sink, "0.05");
    raw(sink, ",");
    key(sink, "inactiveMinCoverage");
    raw(sink, "0.98");
    raw(sink, "}],");
    key(sink, "ignored");
    raw(sink, "[");
    writeIgnoredElement(sink, "directionArrowClusterRaised", DisplayLayout::directionArrowClusterRect(true), false);
    writeIgnoredElement(sink, "wifiBadge", DisplayLayout::wifiBadgeRect(), true);
    writeIgnoredElement(sink, "rssi", DisplayLayout::rssiRect(), true);
    // Slot names are permitted to reach 20 characters and the renderer centers
    // them at 12 px/character without clipping to its 130 px clear window.
    // Cover the full on-screen worst case until profile text becomes a semantic
    // role assertion of its own.
    writeIgnoredElement(sink, "profile", DisplayLayout::profileMaxTextRect(), true);
    writeIgnoredElement(sink, "batteryPercent", DisplayLayout::batteryPercentRect(), true);
    writeIgnoredElement(sink, "batteryIcon", DisplayLayout::batteryIconRect(), true);
    writeIgnoredElement(sink, "gpsBadge", DisplayLayout::gpsBadgeRect(), true);
    raw(sink, "]},");
    writePalettePayload(sink, settings);
    raw(sink, ",");
    key(sink, "overlaps");
    raw(sink, "[");
    raw(sink, "{\"id\":\"profile_battery\",\"elements\":[\"profile\",\"batteryIcon\"],\"reason\":\"profile clear can "
              "overlap the battery icon corner\"},");
    raw(sink, "{\"id\":\"band_gps\",\"elements\":[\"bandCells[0]\",\"gpsBadge\"],\"reason\":\"top band cell clear can "
              "overlap the GPS badge\"},");
    raw(sink,
        "{\"id\":\"arrow_battery\",\"elements\":[\"directionArrowClusterRaised\",\"batteryPercent\",\"batteryIcon\"],"
        "\"reason\":\"raised arrow cluster can overlap battery display regions and requires a restore\"}");
    raw(sink, "],");
    key(sink, "masks");
    raw(sink, "[],");
    key(sink, "complete");
    boolean(sink, true);
    raw(sink, "}");
}

bool parsePinBody(WebServer& server, int& index, bool& clear) {
    JsonDocument doc;
    if (server.hasArg("plain")) {
        String body = server.arg("plain");
        DeserializationError err = deserializeJson(doc, body.c_str());
        if (err) {
            return false;
        }
        if (doc["index"].is<int>()) {
            index = doc["index"].as<int>();
        } else {
            return false;
        }
        clear = doc["clear"].is<bool>() ? doc["clear"].as<bool>() : true;
        return true;
    }
    if (!server.hasArg("index")) {
        return false;
    }
    index = server.arg("index").toInt();
    clear = !server.hasArg("clear") || server.arg("clear") == "true" || server.arg("clear") == "1";
    return true;
}

} // namespace

void handleSteps(WebServer& server, const Runtime& runtime) {
    if (!requireMaintenance(server, runtime) || !requireRuntime(server, runtime, false)) {
        return;
    }
    const V1Settings& settings = runtime.getSettings(runtime.getSettingsCtx);
    sendMeasuredJson(server, [&](auto& sink) { writeStepsPayload(sink, runtime, settings); });
}

void handleLayout(WebServer& server, const Runtime& runtime) {
    if (!requireMaintenance(server, runtime) || !requireRuntime(server, runtime, false)) {
        return;
    }
    const V1Settings& settings = runtime.getSettings(runtime.getSettingsCtx);
    sendMeasuredJson(server, [&](auto& sink) { writeLayoutPayload(sink, runtime, settings); });
}

void handlePin(WebServer& server, const Runtime& runtime, bool (*checkRateLimit)(void* ctx), void* rateLimitCtx) {
    if (checkRateLimit && !checkRateLimit(rateLimitCtx))
        return;
    if (!requireMaintenance(server, runtime) || !requireRuntime(server, runtime, true)) {
        return;
    }

    int index = -1;
    bool clear = true;
    if (!parsePinBody(server, index, clear)) {
        server.send(400, "application/json", "{\"success\":false,\"error\":\"invalid_pin_body\"}");
        return;
    }

    uint32_t renderSeq = 0;
    if (!runtime.preview->pinStep(index, clear, &renderSeq)) {
        server.send(400, "application/json", "{\"success\":false,\"error\":\"invalid_step\"}");
        return;
    }

    JsonDocument doc;
    doc["success"] = true;
    doc["index"] = index;
    doc["clear"] = clear;
    doc["renderSeq"] = renderSeq;
    sendJsonStream(server, doc, 200);
}

namespace {

// Shared by the framebuffer and flushshadow routes: both responses describe
// the same raw-canvas geometry and must carry identical binding/sequence
// headers so the host can prove the two captures came from the same pinned
// render.
void sendFramebufferContractHeaders(WebServer& server, const Runtime& runtime) {
    const V1Settings& settings = runtime.getSettings(runtime.getSettingsCtx);
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%u", V1Display::rawFramebufferWidth());
    server.sendHeader("X-FB-Raw-Width", buf);
    std::snprintf(buf, sizeof(buf), "%u", V1Display::rawFramebufferHeight());
    server.sendHeader("X-FB-Raw-Height", buf);
    std::snprintf(buf, sizeof(buf), "%u", V1Display::logicalFramebufferWidth());
    server.sendHeader("X-FB-Logical-Width", buf);
    std::snprintf(buf, sizeof(buf), "%u", V1Display::logicalFramebufferHeight());
    server.sendHeader("X-FB-Logical-Height", buf);
    server.sendHeader("X-FB-Format", V1Display::rawFramebufferFormat());
    server.sendHeader("X-FB-Transform", V1Display::rawFramebufferTransform());
    std::snprintf(buf, sizeof(buf), "%d", kManifestSchemaVersion);
    server.sendHeader("X-Display-Manifest-Schema-Version", buf);
    server.sendHeader("X-Display-Firmware-Version", runtime.firmwareVersion ? runtime.firmwareVersion : "unknown");
    server.sendHeader("X-Display-Firmware-Sha", runtime.firmwareSha ? runtime.firmwareSha : "unknown");
    char fingerprint[12];
    settingsFingerprintHex(settings, fingerprint, sizeof(fingerprint));
    server.sendHeader("X-Display-Settings-Fingerprint", fingerprint);
    std::snprintf(buf, sizeof(buf), "%lu", static_cast<unsigned long>(runtime.display->renderSequenceId()));
    server.sendHeader("X-Display-Render-Seq", buf);
    std::snprintf(buf, sizeof(buf), "%d", runtime.preview->pinnedStep());
    server.sendHeader("X-Display-Pinned-Step", buf);
}

} // namespace

void handleFramebuffer(WebServer& server, const Runtime& runtime) {
    if (!requireMaintenance(server, runtime) || !requireRuntime(server, runtime, true)) {
        return;
    }
    if (!runtime.preview->isVisualPinned()) {
        server.send(409, "application/json", "{\"success\":false,\"error\":\"framebuffer_not_pinned\"}");
        return;
    }
    if (!runtime.display->rawFramebufferAvailable()) {
        server.send(503, "application/json", "{\"success\":false,\"error\":\"framebuffer_unavailable\"}");
        return;
    }

    sendFramebufferContractHeaders(server, runtime);

    const size_t len = runtime.display->rawFramebufferByteLength();
    server.setContentLength(len);
    server.send(200, "application/octet-stream", "");
    auto client = server.client();
    client.write(runtime.display->rawFramebufferBytes(), len);
}

void handleFlushShadow(WebServer& server, const Runtime& runtime) {
    if (!requireMaintenance(server, runtime) || !requireRuntime(server, runtime, true)) {
        return;
    }
    if (!runtime.preview->isVisualPinned()) {
        server.send(409, "application/json", "{\"success\":false,\"error\":\"framebuffer_not_pinned\"}");
        return;
    }
    if (!runtime.display->flushShadowAvailable()) {
        // Shadow allocation failed (or pinning did not enable it). The host
        // treats this as a protocol error rather than silently skipping the
        // framebuffer-vs-panel comparison.
        server.send(503, "application/json", "{\"success\":false,\"error\":\"flush_shadow_unavailable\"}");
        return;
    }

    sendFramebufferContractHeaders(server, runtime);
    server.sendHeader("X-FB-Shadow", "1");

    const size_t len = runtime.display->rawFramebufferByteLength();
    server.setContentLength(len);
    server.send(200, "application/octet-stream", "");
    auto client = server.client();
    client.write(runtime.display->flushShadowBytes(), len);
}

void handleClear(WebServer& server, const Runtime& runtime) {
    if (!requireMaintenance(server, runtime) || !requireRuntime(server, runtime, true)) {
        return;
    }
    runtime.preview->clearVisualPin();
    runtime.display->showMaintenanceMode();
    server.send(200, "application/json", "{\"success\":true,\"active\":false,\"restored\":true}");
}

} // namespace WifiDisplayVisualApiService

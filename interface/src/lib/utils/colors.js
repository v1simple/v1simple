const COLOR_BOOLEAN_KEYS = new Set([
    'freqUseBandColor',
    'hideWifiIcon',
    'hideProfileIndicator',
    'hideBatteryIcon',
    'showBatteryPercent',
    'hideBleIcon',
    'hideVolumeIndicator',
    'hideRssiIndicator'
]);

export const COLOR_PARAM_ORDER = [
    'bogey',
    'freq',
    'freqUseBandColor',
    'arrowFront',
    'arrowSide',
    'arrowRear',
    'bandL',
    'bandKa',
    'bandK',
    'bandX',
    'bandPhoto',
    'wifiIcon',
    'wifiConnected',
    'bleConnected',
    'bleDisconnected',
    'bar1',
    'bar2',
    'bar3',
    'bar4',
    'bar5',
    'bar6',
    'muted',
    'persisted',
    'volumeMain',
    'volumeMute',
    'rssiV1',
    'rssiProxy',
    'obd',
    'alpConnected',
    'alpDli',
    'alpLidActive',
    'alpAlert',
    'hideWifiIcon',
    'hideProfileIndicator',
    'hideBatteryIcon',
    'showBatteryPercent',
    'hideBleIcon',
    'hideVolumeIndicator',
    'hideRssiIndicator',
    'brightness'
];

const DEFAULT_COLORS = {
    bogey: 0xf800,
    freq: 0xf800,
    freqUseBandColor: false,
    arrowFront: 0xf800,
    arrowSide: 0xf800,
    arrowRear: 0xf800,
    bandL: 0x001f,
    bandKa: 0xf800,
    bandK: 0x001f,
    bandX: 0x07e0,
    bandPhoto: 0x780f,
    wifiIcon: 0x07ff,
    wifiConnected: 0x07e0,
    bleConnected: 0x07e0,
    bleDisconnected: 0x001f,
    bar1: 0x07e0,
    bar2: 0x07e0,
    bar3: 0xffe0,
    bar4: 0xffe0,
    bar5: 0xf800,
    bar6: 0xf800,
    muted: 0x3186,
    persisted: 0x18c3,
    volumeMain: 0xf800,
    volumeMute: 0x7bef,
    rssiV1: 0x07e0,
    rssiProxy: 0x001f,
    obd: 0x001f,
    alpConnected: 0x07e0,
    alpDli: 0xfd20,
    alpLidActive: 0x001f,
    alpAlert: 0xf800,
    hideWifiIcon: false,
    hideProfileIndicator: false,
    hideBatteryIcon: false,
    showBatteryPercent: false,
    hideBleIcon: false,
    hideVolumeIndicator: false,
    hideRssiIndicator: false,
    brightness: 200
};

export const COLOR_PICKER_PRESETS = [
    { label: 'Red', hex: '#f80000', red: 248, green: 0, blue: 0 },
    { label: 'Green', hex: '#00fc00', red: 0, green: 252, blue: 0 },
    { label: 'Blue', hex: '#0000f8', red: 0, green: 0, blue: 248 },
    { label: 'Yellow', hex: '#f8fc00', red: 248, green: 252, blue: 0 },
    { label: 'Cyan', hex: '#00fcf8', red: 0, green: 252, blue: 248 },
    { label: 'Magenta', hex: '#f800f8', red: 248, green: 0, blue: 248 },
    { label: 'Orange', hex: '#f8a000', red: 248, green: 160, blue: 0 },
    {
        label: 'White',
        hex: '#f8fcf8',
        red: 248,
        green: 252,
        blue: 248,
        buttonClass: 'btn btn-sm bg-white text-black'
    }
];

export function cloneDefaultColors() {
    return { ...DEFAULT_COLORS };
}

export function rgb565ToRgb888(rgb565) {
    const value = typeof rgb565 === 'number' ? rgb565 : 0;
    return {
        red: ((value >> 11) & 0x1f) << 3,
        green: ((value >> 5) & 0x3f) << 2,
        blue: (value & 0x1f) << 3
    };
}

export function rgb888ToRgb565(red, green, blue) {
    const r = Number(red) >> 3;
    const g = Number(green) >> 2;
    const b = Number(blue) >> 3;
    return (r << 11) | (g << 5) | b;
}

export function rgb565ToHex(rgb565) {
    const { red, green, blue } = rgb565ToRgb888(rgb565);
    return `#${[red, green, blue].map((value) => value.toString(16).padStart(2, '0')).join('')}`;
}

export function rgb565ToHexStr(rgb565) {
    const value = typeof rgb565 === 'number' ? rgb565 : 0;
    return value.toString(16).toUpperCase().padStart(4, '0');
}

export function rgbComponentsToHex(red, green, blue) {
    return `#${[red, green, blue]
        .map((value) =>
            Math.max(0, Math.min(255, Number(value) || 0))
                .toString(16)
                .padStart(2, '0')
        )
        .join('')}`;
}

export function hexToRgb565(hex) {
    if (!hex || hex.length < 7) return 0;
    const red = parseInt(hex.slice(1, 3), 16);
    const green = parseInt(hex.slice(3, 5), 16);
    const blue = parseInt(hex.slice(5, 7), 16);
    return rgb888ToRgb565(red, green, blue);
}

export function parseColorInput(input) {
    let clean = String(input ?? '')
        .trim()
        .toUpperCase();
    if (!clean) return null;
    if (clean.startsWith('0X')) clean = clean.slice(2);
    if (clean.startsWith('#')) clean = clean.slice(1);
    if (!/^[0-9A-F]+$/.test(clean)) return null;
    if (clean.length <= 5) {
        const value = parseInt(clean, 16);
        return value <= 0xffff ? value : null;
    }
    if (clean.length === 6) {
        const red = parseInt(clean.slice(0, 2), 16);
        const green = parseInt(clean.slice(2, 4), 16);
        const blue = parseInt(clean.slice(4, 6), 16);
        return rgb888ToRgb565(red, green, blue);
    }
    return null;
}

// Linear blend a→b by num/den in RGB565 space. Exact port of lerpRgb565 in
// src/display_bands.cpp (C++ integer division truncates toward zero).
function lerpRgb565(a, b, num, den) {
    const ar = (a >> 11) & 0x1f,
        ag = (a >> 5) & 0x3f,
        ab = a & 0x1f;
    const br = (b >> 11) & 0x1f,
        bg = (b >> 5) & 0x3f,
        bb = b & 0x1f;
    const half = Math.trunc(den / 2);
    const r = ar + Math.trunc(((br - ar) * num + half) / den);
    const g = ag + Math.trunc(((bg - ag) * num + half) / den);
    const bl = ab + Math.trunc(((bb - ab) * num + half) / den);
    return ((r << 11) | (g << 5) | bl) & 0xffff;
}

/**
 * Sample the six configured bar colors (bar1..bar6) across the display's
 * 8-bar meter, exactly as the firmware paints it (src/display_bands.cpp):
 * bar i maps to continuous index i*5/7 in [0,5]; endpoints land exactly on
 * bar1 and bar6, in-between positions blend linearly in RGB565.
 *
 * @param {Record<string, number>} colors  Object with bar1..bar6 RGB565 values
 * @returns {number[]}  8 RGB565 values, index 0 = weakest (bottom bar)
 */
export function sampleBarRampRgb565(colors) {
    const configured = [1, 2, 3, 4, 5, 6].map((n) => colors[`bar${n}`] ?? 0);
    const out = [];
    for (let i = 0; i < 8; i += 1) {
        const scaled = i * 5;
        const idx = Math.trunc(scaled / 7);
        const rem = scaled % 7;
        out.push(
            rem === 0 || idx >= 5
                ? configured[idx]
                : lerpRgb565(configured[idx], configured[idx + 1], rem, 7)
        );
    }
    return out;
}

/**
 * Derive the six stored bar colors (bar1..bar6) from three gradient stops.
 * Slots sit at t = i/5: the lower half blends bottom→middle, the upper half
 * middle→top, using the same RGB565 lerp the firmware renders with. The
 * stored/wire format stays six colors — this is a UI-side authoring helper.
 *
 * @param {number} bottom RGB565 (bar 1, weakest)
 * @param {number} middle RGB565 (ramp midpoint)
 * @param {number} top    RGB565 (bar 6, strongest)
 * @returns {{bar1:number,bar2:number,bar3:number,bar4:number,bar5:number,bar6:number}}
 */
export function deriveBarsFromStops(bottom, middle, top) {
    return {
        bar1: bottom,
        bar2: lerpRgb565(bottom, middle, 2, 5), // t=0.2
        bar3: lerpRgb565(bottom, middle, 4, 5), // t=0.4
        bar4: lerpRgb565(middle, top, 1, 5), // t=0.6
        bar5: lerpRgb565(middle, top, 3, 5), // t=0.8
        bar6: top
    };
}

/**
 * Recover 3 gradient stops from six stored bar colors, when possible.
 * bottom/top are exact (bar1/bar6). The middle is found by exhaustive
 * per-channel search: the lerp is per-channel separable, so each RGB565
 * channel of the middle can be solved independently against the four
 * blended bars (bar2..bar5), ≤64 candidates per channel. `exact` is true
 * only when the returned stops reproduce all six stored colors —
 * stepped/banded themes (like the factory default) report exact:false,
 * with `middle` still set to the best-fit seed for the editor.
 *
 * @param {Record<string, number>} colors Object with bar1..bar6
 * @returns {{bottom:number, middle:number, top:number, exact:boolean}}
 */
export function stopsFromBars(colors) {
    const bars = [1, 2, 3, 4, 5, 6].map((n) => colors[`bar${n}`] ?? 0);
    const bottom = bars[0];
    const top = bars[5];
    const channelOf = (c, i) => (i === 0 ? (c >> 11) & 0x1f : i === 1 ? (c >> 5) & 0x3f : c & 0x1f);
    const lerpCh = (a, b, num, den) => a + Math.trunc(((b - a) * num + Math.trunc(den / 2)) / den);
    const maxes = [31, 63, 31];

    let exact = true;
    const middleCh = [];
    for (let i = 0; i < 3; i += 1) {
        const b = channelOf(bottom, i);
        const t = channelOf(top, i);
        let best = 0;
        let bestErr = Infinity;
        for (let m = 0; m <= maxes[i]; m += 1) {
            const err =
                Math.abs(lerpCh(b, m, 2, 5) - channelOf(bars[1], i)) +
                Math.abs(lerpCh(b, m, 4, 5) - channelOf(bars[2], i)) +
                Math.abs(lerpCh(m, t, 1, 5) - channelOf(bars[3], i)) +
                Math.abs(lerpCh(m, t, 3, 5) - channelOf(bars[4], i));
            if (err < bestErr) {
                bestErr = err;
                best = m;
                if (err === 0) break;
            }
        }
        if (bestErr !== 0) exact = false;
        middleCh.push(best);
    }

    const middle = ((middleCh[0] << 11) | (middleCh[1] << 5) | middleCh[2]) & 0xffff;
    return { bottom, middle, top, exact };
}

export function normalizeColorPayload(payload, base = cloneDefaultColors()) {
    const next = { ...base };
    for (const [key, rawValue] of Object.entries(payload ?? {})) {
        if (!(key in next)) continue;
        if (COLOR_BOOLEAN_KEYS.has(key)) {
            next[key] =
                rawValue === true ||
                rawValue === 1 ||
                rawValue === '1' ||
                String(rawValue).toLowerCase() === 'true';
            continue;
        }
        if (typeof rawValue === 'number') {
            next[key] = rawValue;
            continue;
        }
        if (typeof rawValue === 'string') {
            const parsed = parseInt(rawValue, 10);
            if (!Number.isNaN(parsed)) {
                next[key] = parsed;
            }
        }
    }
    return next;
}

export function buildColorParams(colors) {
    const params = new URLSearchParams();
    for (const key of COLOR_PARAM_ORDER) {
        params.append(key, colors[key]);
    }
    return params;
}

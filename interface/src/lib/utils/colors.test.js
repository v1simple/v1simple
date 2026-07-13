import { describe, expect, it } from 'vitest';
import {
	COLOR_PARAM_ORDER,
	buildColorParams,
	cloneDefaultColors,
	hexToRgb565,
	normalizeColorPayload,
	parseColorInput,
	rgbComponentsToHex,
	rgb565ToHex,
	deriveBarsFromStops,
	rgb565ToHexStr,
	rgb888ToRgb565,
	sampleBarRampRgb565,
	stopsFromBars
} from './colors.js';

describe('colors utilities', () => {
	it('parses rgb565 and rgb888 color inputs', () => {
		expect(parseColorInput('0x07E0')).toBe(0x07e0);
		expect(parseColorInput('#00FC00')).toBe(0x07e0);
		expect(parseColorInput('07E0')).toBe(0x07e0);
		expect(parseColorInput('1234567')).toBeNull();
		expect(parseColorInput('not-a-color')).toBeNull();
	});

	it('round-trips canonical rgb values through hex helpers', () => {
		const green = rgb888ToRgb565(0, 252, 0);
		expect(green).toBe(0x07e0);
		expect(rgb565ToHex(green)).toBe('#00fc00');
		expect(rgb565ToHexStr(green)).toBe('07E0');
		expect(hexToRgb565('#00FC00')).toBe(green);
		expect(hexToRgb565('bad')).toBe(0);
		expect(rgbComponentsToHex(248, 160, 0)).toBe('#f8a000');
		expect(rgbComponentsToHex(999, undefined, -10)).toBe('#ff0000');
	});

	it('normalizes booleans and numeric strings from API payloads', () => {
		const normalized = normalizeColorPayload(
			{
				bogey: '63488',
				freqUseBandColor: 'true',
				hideWifiIcon: 1,
				showBatteryPercent: '1',
				hideBleIcon: '0',
				brightness: '123',
				ignoredKey: '999'
			},
			cloneDefaultColors()
		);

		expect(normalized.bogey).toBe(63488);
		expect(normalized.freqUseBandColor).toBe(true);
		expect(normalized.hideWifiIcon).toBe(true);
		expect(normalized.showBatteryPercent).toBe(true);
		expect(normalized.hideBleIcon).toBe(false);
		expect(normalized.brightness).toBe(123);
		expect(normalized.ignoredKey).toBeUndefined();
	});

	it('keeps frontend defaults aligned with firmware display defaults', () => {
		const defaults = cloneDefaultColors();

		expect(defaults.volumeMain).toBe(0xF800);
		expect(defaults.volumeMute).toBe(0x7BEF);
		expect(defaults.alpAlert).toBe(0xF800);
		expect(defaults.brightness).toBe(200);
	});

	it('samples the six bar colors across the 8-bar meter exactly like the firmware', () => {
		// Mirrors expectedBarColor() in test/test_display_rendering_bands:
		// bar i → continuous index i*5/7, endpoints exact, RGB565 lerp between.
		const defaults = cloneDefaultColors();
		expect(sampleBarRampRgb565(defaults)).toEqual([
			0x07e0, 0x07e0, 0x6fe0, 0xffe0, 0xffe0, 0xfb80, 0xf800, 0xf800
		]);

		// Endpoints land exactly on bar1 and bar6 for any palette.
		const distinct = {
			bar1: 0x001f, bar2: 0x07e0, bar3: 0xffe0,
			bar4: 0xf800, bar5: 0xf81f, bar6: 0xffff
		};
		const ramp = sampleBarRampRgb565(distinct);
		expect(ramp).toHaveLength(8);
		expect(ramp[0]).toBe(distinct.bar1);
		expect(ramp[7]).toBe(distinct.bar6);
		expect(ramp).toEqual([
			0x001f, 0x05aa, 0x6fe0, 0xfee0, 0xf940, 0xf812, 0xfa5f, 0xffff
		]);

		// A flat palette must stay flat (no interpolation drift).
		const flat = Object.fromEntries([1, 2, 3, 4, 5, 6].map((n) => [`bar${n}`, 0x07e0]));
		expect(new Set(sampleBarRampRgb565(flat))).toEqual(new Set([0x07e0]));
	});

	it('derives six bar colors from three gradient stops', () => {
		const derived = deriveBarsFromStops(0x07e0, 0xffe0, 0xf800); // green→yellow→red
		expect(derived).toEqual({
			bar1: 0x07e0, bar2: 0x67e0, bar3: 0xcfe0,
			bar4: 0xfe60, bar5: 0xfb40, bar6: 0xf800
		});
		// Flat stops stay flat.
		expect(new Set(Object.values(deriveBarsFromStops(0x001f, 0x001f, 0x001f)))).toEqual(
			new Set([0x001f])
		);
	});

	it('recovers stops from derived themes and rejects stepped themes', () => {
		// Any derived theme must round-trip: exact stops that reproduce all six.
		const derived = deriveBarsFromStops(0x001f, 0xf81f, 0xffff);
		const stops = stopsFromBars(derived);
		expect(stops.exact).toBe(true);
		expect(deriveBarsFromStops(stops.bottom, stops.middle, stops.top)).toEqual(derived);

		// The stepped factory default (G,G,Y,Y,R,R) is not a 3-stop ramp.
		expect(stopsFromBars(cloneDefaultColors()).exact).toBe(false);
	});

	it('builds URL params in the firmware API order', () => {
		const colors = {
			...cloneDefaultColors(),
			showBatteryPercent: true,
			brightness: 123
		};
		const params = buildColorParams(colors);

		expect([...params.keys()]).toEqual(COLOR_PARAM_ORDER);
		expect(params.get('showBatteryPercent')).toBe('true');
		expect(params.get('brightness')).toBe('123');
	});
});

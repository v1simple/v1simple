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
	rgb565ToHexStr,
	rgb888ToRgb565
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

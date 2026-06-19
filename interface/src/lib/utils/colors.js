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
	bogey: 0xF800,
	freq: 0xF800,
	freqUseBandColor: false,
	arrowFront: 0xF800,
	arrowSide: 0xF800,
	arrowRear: 0xF800,
	bandL: 0x001F,
	bandKa: 0xF800,
	bandK: 0x001F,
	bandX: 0x07E0,
	bandPhoto: 0x780F,
	wifiIcon: 0x07FF,
	wifiConnected: 0x07E0,
	bleConnected: 0x07E0,
	bleDisconnected: 0x001F,
	bar1: 0x07E0,
	bar2: 0x07E0,
	bar3: 0xFFE0,
	bar4: 0xFFE0,
	bar5: 0xF800,
	bar6: 0xF800,
	muted: 0x3186,
	persisted: 0x18C3,
	volumeMain: 0xF800,
	volumeMute: 0x7BEF,
	rssiV1: 0x07E0,
	rssiProxy: 0x001F,
	obd: 0x001F,
	alpConnected: 0x07E0,
	alpDli: 0xFD20,
	alpLidActive: 0x001F,
	alpAlert: 0xF800,
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
	{ label: 'White', hex: '#f8fcf8', red: 248, green: 252, blue: 248, buttonClass: 'btn btn-sm bg-white text-black' }
];

export function cloneDefaultColors() {
	return { ...DEFAULT_COLORS };
}

export function rgb565ToRgb888(rgb565) {
	const value = typeof rgb565 === 'number' ? rgb565 : 0;
	return {
		red: ((value >> 11) & 0x1F) << 3,
		green: ((value >> 5) & 0x3F) << 2,
		blue: (value & 0x1F) << 3
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
		.map((value) => Math.max(0, Math.min(255, Number(value) || 0)).toString(16).padStart(2, '0'))
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
	let clean = String(input ?? '').trim().toUpperCase();
	if (!clean) return null;
	if (clean.startsWith('0X')) clean = clean.slice(2);
	if (clean.startsWith('#')) clean = clean.slice(1);
	if (!/^[0-9A-F]+$/.test(clean)) return null;
	if (clean.length <= 5) {
		const value = parseInt(clean, 16);
		return value <= 0xFFFF ? value : null;
	}
	if (clean.length === 6) {
		const red = parseInt(clean.slice(0, 2), 16);
		const green = parseInt(clean.slice(2, 4), 16);
		const blue = parseInt(clean.slice(4, 6), 16);
		return rgb888ToRgb565(red, green, blue);
	}
	return null;
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

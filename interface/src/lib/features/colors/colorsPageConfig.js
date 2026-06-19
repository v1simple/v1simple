export const SIGNAL_BARS = [1, 2, 3, 4, 5, 6];

export const DISPLAY_SETTINGS_ENDPOINT = '/api/display/settings';
export const DISPLAY_SETTINGS_RESET_ENDPOINT = '/api/display/settings/reset';
export const DISPLAY_PREVIEW_ENDPOINT = '/api/display/preview';
export const DISPLAY_PREVIEW_CLEAR_ENDPOINT = '/api/display/preview/clear';
export const QUIET_SETTINGS_ENDPOINT = '/api/quiet/settings';

export const BAND_FIELDS = [
	{ key: 'bandL', id: 'bandL-color', label: 'Laser (L)', pickerLabel: 'Laser Band', preview: 'L' },
	{ key: 'bandKa', id: 'bandKa-color', label: 'Ka Band', pickerLabel: 'Ka Band', preview: 'Ka' },
	{ key: 'bandK', id: 'bandK-color', label: 'K Band', pickerLabel: 'K Band', preview: 'K' },
	{ key: 'bandX', id: 'bandX-color', label: 'X Band', pickerLabel: 'X Band', preview: 'X' },
	{
		key: 'bandPhoto',
		id: 'band-photo-color',
		label: 'Photo',
		pickerLabel: 'Photo Radar Band',
		preview: 'P',
		swatchSize: 'md'
	}
];

export const ARROW_FIELDS = [
	{ key: 'arrowFront', id: 'arrow-front-color', label: 'Front', pickerLabel: 'Front Arrow', preview: '▲' },
	{ key: 'arrowSide', id: 'arrow-side-color', label: 'Side', pickerLabel: 'Side Arrows', preview: '◀▶' },
	{ key: 'arrowRear', id: 'arrow-rear-color', label: 'Rear', pickerLabel: 'Rear Arrow', preview: '▼' }
];

export const BADGE_FIELDS = [
	{
		key: 'obd',
		id: 'obd-color',
		label: 'OBD Badge',
		pickerLabel: 'OBD Badge',
		preview: 'OBD',
		previewClass: 'text-xl font-bold font-mono'
	}
];

export const ALP_BADGE_FIELDS = [
	{
		key: 'alpConnected',
		id: 'alp-connected-color',
		label: 'Connected',
		pickerLabel: 'ALP Connected',
		preview: 'ALP',
		previewClass: 'text-xl font-bold font-mono'
	},
	{
		key: 'alpDli',
		id: 'alp-dli-color',
		label: 'DLI',
		pickerLabel: 'ALP DLI',
		preview: 'ALP',
		previewClass: 'text-xl font-bold font-mono'
	},
	{
		key: 'alpLidActive',
		id: 'alp-lid-active-color',
		label: 'LID',
		pickerLabel: 'ALP LID',
		preview: 'ALP',
		previewClass: 'text-xl font-bold font-mono'
	},
	{
		key: 'alpAlert',
		id: 'alp-alert-color',
		label: 'Alert',
		pickerLabel: 'ALP Alert',
		preview: 'ALP',
		previewClass: 'text-xl font-bold font-mono'
	}
];

export const STATUS_FIELD_ROWS = [
	[
		{
			key: 'wifiConnected',
			id: 'wifiConnected-color',
			label: 'WiFi Connected',
			pickerLabel: 'WiFi Connected',
			preview: '📶'
		},
		{
			key: 'wifiIcon',
			id: 'wifiIcon-color',
			label: 'WiFi (No Client)',
			pickerLabel: 'WiFi (No Client)',
			preview: '📶'
		}
	],
	[
		{
			key: 'bleConnected',
			id: 'bleConnected-color',
			label: 'Proxy Connected',
			pickerLabel: 'Proxy Connected',
			preview: '🔗'
		},
		{
			key: 'bleDisconnected',
			id: 'bleDisconnected-color',
			label: 'Proxy Ready',
			pickerLabel: 'Proxy Ready',
			preview: '⛓️'
		}
	]
];

export const VISIBILITY_TOGGLES = [
	{
		key: 'hideWifiIcon',
		title: 'Hide WiFi Icon',
		description: 'Show briefly on connect, then hide'
	},
	{
		key: 'hideProfileIndicator',
		title: 'Hide Profile Indicator',
		description: 'Show on profile change, then hide'
	},
	{
		key: 'hideBatteryIcon',
		title: 'Hide Battery Icon',
		description: 'Hide the battery indicator'
	},
	{
		key: 'showBatteryPercent',
		title: 'Show Battery Percentage',
		description: 'Show battery level as percentage instead of icon',
		disabled: (colors) => colors.hideBatteryIcon
	},
	{
		key: 'hideBleIcon',
		title: 'Hide BLE Proxy Icon',
		description: 'Hide the BLE proxy status indicator'
	},
	{
		key: 'hideVolumeIndicator',
		title: 'Hide Volume Indicator',
		description: 'Hide the V1 volume display (requires V1 firmware 4.1028+)'
	},
	{
		key: 'hideRssiIndicator',
		title: 'Hide RSSI Indicator',
		description: 'Hide the BLE signal strength display'
	}
];

// One editable control per physical meter segment.
export const SIGNAL_BARS = [1, 2, 3, 4, 5, 6];

export const DISPLAY_SETTINGS_ENDPOINT = '/api/display/settings';
export const DISPLAY_SETTINGS_RESET_ENDPOINT = '/api/display/settings/reset';
export const DISPLAY_PREVIEW_ENDPOINT = '/api/display/preview';
export const DISPLAY_PREVIEW_CLEAR_ENDPOINT = '/api/display/preview/clear';
export const QUIET_SETTINGS_ENDPOINT = '/api/quiet/settings';

export const BAND_FIELDS = [
    {
        key: 'bandL',
        id: 'bandL-color',
        label: 'Laser (L)',
        pickerLabel: 'Laser Band',
        preview: 'L'
    },
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
    {
        key: 'arrowFront',
        id: 'arrow-front-color',
        label: 'Front',
        pickerLabel: 'Front Arrow',
        preview: '▲'
    },
    {
        key: 'arrowSide',
        id: 'arrow-side-color',
        label: 'Side',
        pickerLabel: 'Side Arrows',
        preview: '◀▶'
    },
    {
        key: 'arrowRear',
        id: 'arrow-rear-color',
        label: 'Rear',
        pickerLabel: 'Rear Arrow',
        preview: '▼'
    }
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
        title: 'Show WiFi status',
        description: 'Show when WiFi is active.',
        inverted: true
    },
    {
        key: 'hideProfileIndicator',
        title: 'Show active slot',
        description: 'When off, the slot still appears briefly after a change.',
        inverted: true
    },
    {
        key: 'hideBatteryIcon',
        title: 'Show battery level',
        description: 'Show the V1Simple battery indicator.',
        inverted: true
    },
    {
        key: 'showBatteryPercent',
        title: 'Show battery percentage',
        description: 'Use a percentage instead of the battery icon.',
        disabled: (colors) => colors.hideBatteryIcon
    },
    {
        key: 'hideBleIcon',
        title: 'Show Bluetooth proxy status',
        description: 'Show the phone connection indicator on this screen.',
        inverted: true
    },
    {
        key: 'hideVolumeIndicator',
        title: 'Show V1 volume',
        description: 'Show volume levels from V1 firmware 4.1028 or later.',
        inverted: true
    },
    {
        key: 'hideRssiIndicator',
        title: 'Show connection strength',
        description: 'Show V1 and phone signal strength while V1 volume is shown.',
        inverted: true
    }
];

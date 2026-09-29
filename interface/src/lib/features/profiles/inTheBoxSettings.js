export const IN_THE_BOX_BANDS = [
    { key: 'x', label: 'X', boxes: ['x'] },
    { key: 'ku', label: 'Ku', boxes: ['ku'] },
    { key: 'k', label: 'K', boxes: ['k'] },
    { key: 'ka', label: 'Ka', boxes: ['kaLow', 'kaMid', 'kaHigh'] }
];

export const IN_THE_BOX_BOXES = {
    x: { label: 'X', min: 10500, max: 10550, lowerMHz: 10500, upperMHz: 10550 },
    ku: { label: 'Ku', min: 13400, max: 13500, lowerMHz: 13400, upperMHz: 13500 },
    k: { label: 'K', min: 23900, max: 24250, lowerMHz: 24050, upperMHz: 24250 },
    kaLow: { label: 'Ka low', min: 33400, max: 36000, lowerMHz: 33700, upperMHz: 33900 },
    kaMid: { label: 'Ka middle', min: 33400, max: 36000, lowerMHz: 34600, upperMHz: 34800 },
    kaHigh: { label: 'Ka high', min: 33400, max: 36000, lowerMHz: 35400, upperMHz: 35600 }
};

export function createDefaultInTheBoxSettings() {
    return {
        bands: Object.fromEntries(IN_THE_BOX_BANDS.map(({ key }) => [key, {
            muteOutside: false, unmuteInside: false
        }])),
        boxes: Object.fromEntries(Object.entries(IN_THE_BOX_BOXES).map(([key, box]) => [key, {
            enabled: true, lowerMHz: box.lowerMHz, upperMHz: box.upperMHz
        }]))
    };
}

export function fromApiInTheBoxSettings(api) {
    const settings = createDefaultInTheBoxSettings();
    for (const { key } of IN_THE_BOX_BANDS) {
        settings.bands[key] = {
            muteOutside: api?.bands?.[key]?.muteOutside === true,
            unmuteInside: api?.bands?.[key]?.unmuteInside === true
        };
    }
    for (const key of Object.keys(IN_THE_BOX_BOXES)) {
        const box = api?.boxes?.[key];
        if (!box) continue;
        settings.boxes[key] = {
            enabled: typeof box.enabled === 'boolean' ? box.enabled : true,
            lowerMHz: Number(box.lowerMHz ?? settings.boxes[key].lowerMHz),
            upperMHz: Number(box.upperMHz ?? settings.boxes[key].upperMHz)
        };
    }
    return settings;
}

export function cloneInTheBoxSettings(settings) {
    if (!settings) return createDefaultInTheBoxSettings();
    return {
        bands: Object.fromEntries(IN_THE_BOX_BANDS.map(({ key }) => [key, { ...settings.bands[key] }])),
        boxes: Object.fromEntries(Object.keys(IN_THE_BOX_BOXES).map((key) => [key, { ...settings.boxes[key] }]))
    };
}

export function toApiInTheBoxSettings(settings) {
    return cloneInTheBoxSettings(settings);
}

export function inTheBoxSettingsError(settings) {
    if (!settings) return null;
    for (const [key, limits] of Object.entries(IN_THE_BOX_BOXES)) {
        const box = settings.boxes[key];
        if (!Number.isInteger(box?.lowerMHz) || !Number.isInteger(box?.upperMHz) ||
            box.lowerMHz < limits.min || box.upperMHz > limits.max || box.lowerMHz > box.upperMHz) {
            return `${limits.label} box needs whole MHz values from ${limits.min} to ${limits.max}, with the lower edge no higher than the upper edge.`;
        }
    }
    return null;
}

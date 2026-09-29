import { describe, expect, it } from 'vitest';
import {
    IN_THE_BOX_BOXES, cloneInTheBoxSettings, createDefaultInTheBoxSettings,
    fromApiInTheBoxSettings, inTheBoxSettingsError, toApiInTheBoxSettings
} from './inTheBoxSettings';

describe('In-the-Box profile settings', () => {
    it('loads absent legacy app settings with every muting action off', () => {
        const settings = fromApiInTheBoxSettings();
        expect(settings).toEqual(createDefaultInTheBoxSettings());
        expect(Object.values(settings.bands)).toEqual(Array(4).fill({ muteOutside: false, unmuteInside: false }));
        expect(settings.boxes).toEqual({
            x: { enabled: true, lowerMHz: 10500, upperMHz: 10550 },
            ku: { enabled: true, lowerMHz: 13400, upperMHz: 13500 },
            k: { enabled: true, lowerMHz: 24050, upperMHz: 24250 },
            kaLow: { enabled: true, lowerMHz: 33700, upperMHz: 33900 },
            kaMid: { enabled: true, lowerMHz: 34600, upperMHz: 34800 },
            kaHigh: { enabled: true, lowerMHz: 35400, upperMHz: 35600 }
        });
    });

    it('deeply isolates editing and submitted app settings from their source', () => {
        const original = createDefaultInTheBoxSettings();
        original.bands.ka.unmuteInside = true;
        original.boxes.kaMid.enabled = false;
        const loaded = fromApiInTheBoxSettings(original);
        const edited = cloneInTheBoxSettings(loaded);
        const submitted = toApiInTheBoxSettings(edited);
        expect(submitted).toEqual(original);
        edited.bands.ka.unmuteInside = false;
        edited.boxes.kaMid.lowerMHz = 34500;
        expect(submitted).toEqual(original);
        expect(loaded).toEqual(original);
    });

    it.each(Object.entries(IN_THE_BOX_BOXES))('accepts inclusive Gen2 edges for %s', (key, limits) => {
        const settings = createDefaultInTheBoxSettings();
        settings.boxes[key].lowerMHz = limits.min;
        settings.boxes[key].upperMHz = limits.min;
        expect(inTheBoxSettingsError(settings)).toBeNull();
        settings.boxes[key].lowerMHz = limits.max;
        settings.boxes[key].upperMHz = limits.max;
        expect(inTheBoxSettingsError(settings)).toBeNull();
        settings.boxes[key].lowerMHz = limits.min - 1;
        expect(inTheBoxSettingsError(settings)).not.toBeNull();
        settings.boxes[key].lowerMHz = limits.min;
        settings.boxes[key].upperMHz = limits.max + 1;
        expect(inTheBoxSettingsError(settings)).not.toBeNull();
    });

    it.each([undefined, NaN, 24050.5, 24251])('rejects invalid K edges even when the box is disabled: %s', (edge) => {
        const settings = createDefaultInTheBoxSettings();
        settings.boxes.k.enabled = false;
        settings.boxes.k.lowerMHz = edge;
        expect(inTheBoxSettingsError(settings)).not.toBeNull();
    });
});

import { describe, expect, it } from 'vitest';

import {
    detectorConfigurationFromSnapshot,
    fromApiSettings,
    toApiDetectorConfiguration,
    toApiSettings
} from './profileSettingsAdapter.js';

function snapshotWith(observations = {}) {
    return {
        observations: {
            mode: { available: false, value: null },
            displayOn: { available: false, value: null },
            currentVolume: { available: false, main: null, muted: null },
            savedVolume: { available: false, main: null, muted: null },
            ...observations
        }
    };
}

describe('profile settings adapter', () => {
    it('leaves every unavailable detector observation explicitly unchanged', () => {
        expect(toApiDetectorConfiguration(detectorConfigurationFromSnapshot(snapshotWith())))
            .toEqual({
                userSettings: 'value',
                mode: { policy: 'unchanged' },
                display: 'unchanged',
                volume: { policy: 'unchanged' },
                bluetoothLed: 'unchanged',
                customFrequencies: 'unchanged'
            });
    });

    it.each([
        ['A', 1], ['C', 1], ['U', 1],
        ['l', 2], ['c', 2], ['u', 2],
        ['L', 3]
    ])('maps observed mode %s to explicit mode %i', (observed, expected) => {
        const detector = detectorConfigurationFromSnapshot(snapshotWith({
            mode: { available: true, value: observed }
        }));
        expect(toApiDetectorConfiguration(detector).mode).toEqual({
            policy: 'value', value: expected
        });
    });

    it('does not invent a mode policy for an unrecognized observed value', () => {
        const detector = detectorConfigurationFromSnapshot(snapshotWith({
            mode: { available: true, value: '?' }
        }));
        expect(toApiDetectorConfiguration(detector).mode).toEqual({ policy: 'unchanged' });
    });

    it('uses saved-volume availability ahead of a different current volume', () => {
        const detector = detectorConfigurationFromSnapshot(snapshotWith({
            currentVolume: { available: true, main: 1, muted: 2 },
            savedVolume: { available: true, main: 8, muted: 3 }
        }));
        expect(toApiDetectorConfiguration(detector).volume).toEqual({
            policy: 'saved', main: 8, muted: 3
        });
    });

    it('maps current-only zero volume to an explicit temporary 0/0 pair', () => {
        const detector = detectorConfigurationFromSnapshot(snapshotWith({
            currentVolume: { available: true, main: 0, muted: 0 }
        }));
        expect(toApiDetectorConfiguration(detector).volume).toEqual({
            policy: 'temporary', main: 0, muted: 0
        });
    });

    it('preserves captured base bytes through an edited save payload', () => {
        const settings = fromApiSettings({
            bytes: [165, 90, 195, 60, 129, 126],
            xBand: true,
            kBand: false
        });
        settings.x = false;
        expect(toApiSettings(settings)).toMatchObject({
            baseBytes: [165, 90, 195, 60, 129, 126],
            xBand: false,
            kBand: false
        });
    });
});

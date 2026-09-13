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
            bluetoothIndicator: { available: false, value: null },
            customFrequencies: { definitionsAvailable: false, effectiveDefinitions: [] },
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
                customFrequencies: { policy: 'unchanged' }
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

    it('prefills a differing observed current pair without inventing a volume policy', () => {
        const detector = detectorConfigurationFromSnapshot(snapshotWith({
            currentVolume: { available: true, main: 1, muted: 2 },
            savedVolume: { available: true, main: 8, muted: 3 }
        }));
        expect(detector).toMatchObject({ volumePolicy: 'unchanged', mainVolume: 1, mutedVolume: 2 });
        expect(toApiDetectorConfiguration(detector).volume).toEqual({ policy: 'unchanged' });
    });

    it('does not infer saved policy when current and saved volume are equal', () => {
        const detector = detectorConfigurationFromSnapshot(snapshotWith({
            currentVolume: { available: true, main: 8, muted: 3 },
            savedVolume: { available: true, main: 8, muted: 3 }
        }));
        expect(detector).toMatchObject({
            volumePolicy: 'unchanged', mainVolume: 8, mutedVolume: 3,
            volumeFeedback: 'none', volumeDisconnect: 'restore_saved'
        });
        expect(toApiDetectorConfiguration(detector).volume).toEqual({ policy: 'unchanged' });
    });

    it('prefills current-only zero volume without inventing command aux bits', () => {
        const detector = detectorConfigurationFromSnapshot(snapshotWith({
            currentVolume: { available: true, main: 0, muted: 0 }
        }));
        expect(detector).toMatchObject({ volumePolicy: 'unchanged', mainVolume: 0, mutedVolume: 0 });
        expect(toApiDetectorConfiguration(detector).volume).toEqual({ policy: 'unchanged' });
    });

    it('round-trips volume command policy, display-off Bluetooth intent, and complete custom definitions', () => {
        const detector = detectorConfigurationFromSnapshot(snapshotWith({
            displayOn: { available: true, value: false },
            bluetoothIndicator: { available: true, value: 'blinking' },
            currentVolume: { available: true, main: 5, muted: 2 },
            customFrequencies: {
                definitionsAvailable: true,
                effectiveDefinitions: [
                    { index: 0, lowerMHz: 24050, upperMHz: 24150 },
                    { index: 1, lowerMHz: 0, upperMHz: 0 }
                ]
            }
        }));
        detector.volumePolicy = 'temporary';
        detector.volumeFeedback = 'always';
        detector.volumeDisconnect = 'keep_current';
        expect(toApiDetectorConfiguration(detector)).toMatchObject({
            display: 'off',
            bluetoothLed: 'on',
            volume: {
                policy: 'temporary', main: 5, muted: 2,
                feedback: 'always', disconnect: 'keep_current'
            },
            customFrequencies: {
                policy: 'value',
                definitions: [
                    { index: 0, lowerMHz: 24050, upperMHz: 24150 },
                    { index: 1, lowerMHz: 0, upperMHz: 0 }
                ]
            }
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

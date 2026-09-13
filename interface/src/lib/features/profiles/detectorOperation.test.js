import { describe, expect, it } from 'vitest';

import {
    readDetectorOperationId,
    validDetectorOperationStart,
    validDetectorOperationStatus
} from './detectorOperation.js';

function components(overrides = {}) {
    const empty = {
        requested: false, sent: false, verified: false,
        outcome: 'not_requested', reason: 'none'
    };
    return Object.fromEntries(
        ['userSettings', 'display', 'mode', 'volume', 'customFrequencies', 'factoryReset']
            .map((name) => [name, { ...empty, ...(overrides[name] || {}) }])
    );
}

function succeeded(componentOverrides = {}) {
    return {
        operationId: 42,
        kind: 'apply_profile',
        source: 'maintenance_ui',
        profileName: 'Road',
        targetAddress: 'AA:BB:CC:DD:EE:FF',
        returnToMaintenance: false,
        state: 'succeeded',
        reason: 'none',
        terminal: true,
        result: 'succeeded',
        components: components(componentOverrides)
    };
}

describe('detector operation contract', () => {
    it('accepts a profile whose policies form a verified no-op', () => {
        expect(validDetectorOperationStatus(succeeded(), 42)).toBe(true);
        expect(validDetectorOperationStatus(succeeded({ display: {
            requested: true, sent: false, verified: true,
            outcome: 'unchanged', reason: 'none'
        } }), 42)).toBe(true);
    });

    it('rejects contradictory success evidence', () => {
        expect(validDetectorOperationStatus(succeeded({ display: {
            requested: true, sent: false, verified: false,
            outcome: 'unchanged', reason: 'none'
        } }), 42)).toBe(false);
        expect(validDetectorOperationStatus(succeeded({ display: {
            requested: true, sent: true, verified: true,
            outcome: 'mismatch', reason: 'display_mismatch'
        } }), 42)).toBe(false);
        expect(validDetectorOperationStatus(succeeded({ display: {
            requested: true, sent: true, verified: false,
            outcome: 'pending', reason: 'none'
        } }), 42)).toBe(false);
        expect(validDetectorOperationStatus(succeeded({ display: {
            requested: true, sent: false, verified: false,
            outcome: 'sent', reason: 'none'
        } }), 42)).toBe(false);
    });

    it('accepts a truthful factory failure that occurred before any reset send', () => {
        const status = succeeded();
        status.kind = 'factory_reset';
        delete status.profileName;
        status.state = 'failed';
        status.result = 'failed';
        status.reason = 'detector_timeout';
        expect(validDetectorOperationStatus(status, 42)).toBe(true);
        status.reason = 'factory_reset_send_failed';
        expect(validDetectorOperationStatus(status, 42)).toBe(false);
    });

    it('bounds stored and returned identities to the firmware uint32 domain', () => {
        expect(validDetectorOperationStart({
            success: true, queued: true, rebooting: true, target: 'normal',
            state: 'pending_normal_boot', operationId: 0xffffffff
        })).toBe(true);
        expect(validDetectorOperationStart({
            success: true, queued: true, rebooting: true, target: 'normal',
            state: 'pending_normal_boot', operationId: 0x100000000
        })).toBe(false);
        expect(validDetectorOperationStart({
            success: false, queued: true, rebooting: true, target: 'normal',
            state: 'pending_normal_boot', operationId: 42
        })).toBe(false);

        const removed = [];
        expect(readDetectorOperationId({
            getItem: () => '4294967296',
            removeItem: (key) => removed.push(key)
        })).toBeNull();
        expect(removed).toHaveLength(1);
    });
});

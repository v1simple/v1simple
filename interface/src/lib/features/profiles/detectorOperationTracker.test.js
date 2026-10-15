import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

import { installFetchMock, jsonResponse } from '../../../test/fetch-mock.js';
import { DETECTOR_OPERATION_STORAGE_KEY } from './detectorOperation.js';
import { createDetectorOperationTracker } from './detectorOperationTracker.js';

function operationStatus(state = 'running') {
    const terminal = state === 'succeeded';
    return {
        operationId: 42,
        kind: 'apply_profile',
        source: 'maintenance_ui',
        profileName: 'Road',
        targetAddress: 'AA:BB:CC:DD:EE:FF',
        returnToMaintenance: true,
        state,
        reason: 'none',
        terminal,
        result: terminal ? state : 'in_progress',
        components: Object.fromEntries(
            ['userSettings', 'display', 'mode', 'volume', 'customFrequencies', 'factoryReset']
                .map((name) => [name, {
                    requested: false, sent: false, verified: false,
                    outcome: 'not_requested', reason: 'none'
                }])
        )
    };
}

describe('detector operation tracker lifecycle', () => {
    let storage;
    let onStatus;
    let onMessage;
    let tracker;

    beforeEach(() => {
        vi.useFakeTimers();
        const entries = new Map([[DETECTOR_OPERATION_STORAGE_KEY, '42']]);
        storage = {
            getItem: (key) => entries.get(key) ?? null,
            setItem: (key, value) => entries.set(key, String(value)),
            removeItem: (key) => entries.delete(key)
        };
        onStatus = vi.fn();
        onMessage = vi.fn();
        tracker = createDetectorOperationTracker({ storage, onStatus, onMessage });
    });

    afterEach(() => {
        tracker.dispose();
        vi.useRealTimers();
        vi.restoreAllMocks();
    });

    it('pauses after five minutes while retaining the exact identity for a reload', async () => {
        const fetchMock = installFetchMock([
            { match: '/api/autopush/status?operationId=42', respond: jsonResponse(operationStatus()) }
        ]);
        await tracker.resume();
        await vi.advanceTimersByTimeAsync(5 * 60 * 1000);

        expect(onMessage).toHaveBeenCalledExactlyOnceWith({
            type: 'warning',
            text: 'Detector operation status polling paused after five minutes. Reload to resume the exact operation.'
        });
        const pausedRequestCount = fetchMock.mock.calls.length;
        await vi.advanceTimersByTimeAsync(60_000);
        expect(fetchMock).toHaveBeenCalledTimes(pausedRequestCount);
        expect(storage.getItem(DETECTOR_OPERATION_STORAGE_KEY)).toBe('42');

        tracker.dispose();
        tracker = createDetectorOperationTracker({ storage, onStatus, onMessage });
        await tracker.resume();
        expect(fetchMock).toHaveBeenCalledTimes(pausedRequestCount + 1);
        expect(fetchMock).toHaveBeenLastCalledWith('/api/autopush/status?operationId=42', expect.anything());
    });

    it('gives a newly accepted operation a fresh polling window after a pause', async () => {
        let terminal = false;
        const fetchMock = installFetchMock([
            { match: '/api/autopush/status?operationId=42', respond: jsonResponse(operationStatus()) },
            { match: '/api/autopush/status?operationId=43', respond: () => jsonResponse({
                ...operationStatus(terminal ? 'succeeded' : 'running'), operationId: 43
            }) }
        ]);
        await tracker.resume();
        await vi.advanceTimersByTimeAsync(5 * 60 * 1000);

        tracker.accepted({ operationId: 43, state: 'pending_normal_boot' });
        expect(storage.getItem(DETECTOR_OPERATION_STORAGE_KEY)).toBe('43');
        expect(onStatus).toHaveBeenLastCalledWith(expect.objectContaining({
            operationId: 43, state: 'pending_normal_boot', terminal: false
        }));
        await vi.advanceTimersByTimeAsync(750);
        expect(fetchMock).toHaveBeenLastCalledWith('/api/autopush/status?operationId=43', expect.anything());
        expect(onStatus).toHaveBeenLastCalledWith(expect.objectContaining({ operationId: 43, state: 'running' }));
        terminal = true;
        await vi.advanceTimersByTimeAsync(750);
        expect(onStatus).toHaveBeenLastCalledWith(expect.objectContaining({ operationId: 43, state: 'succeeded' }));
        expect(storage.getItem(DETECTOR_OPERATION_STORAGE_KEY)).toBeNull();
        expect(vi.getTimerCount()).toBe(0);
    });

    it('follows the accepted operation when saving its identity fails over an older hint', async () => {
        vi.spyOn(storage, 'setItem').mockImplementation(() => { throw new Error('Quota exceeded'); });
        const result = {
            ...operationStatus('succeeded'), operationId: 43,
            state: 'partial', result: 'partial', reason: 'apply_partial'
        };
        const fetchMock = installFetchMock([
            { match: '/api/autopush/status?operationId=43', respond: jsonResponse(result) }
        ]);

        tracker.accepted({ operationId: 43, state: 'pending_normal_boot' });
        await vi.advanceTimersByTimeAsync(750);

        expect(fetchMock).toHaveBeenCalledExactlyOnceWith('/api/autopush/status?operationId=43', expect.anything());
        expect(onStatus).toHaveBeenLastCalledWith(result);
        expect(onMessage).not.toHaveBeenCalled();
        expect(storage.getItem(DETECTOR_OPERATION_STORAGE_KEY)).toBe('42');
        expect(vi.getTimerCount()).toBe(0);
    });

    it('publishes the result when storage becomes unreadable during a request', async () => {
        let resolveResponse;
        const fetchMock = installFetchMock([{
            match: '/api/autopush/status?operationId=42',
            respond: () => new Promise((resolve) => { resolveResponse = resolve; })
        }]);
        const pending = tracker.resume();
        vi.spyOn(storage, 'getItem').mockImplementation(() => { throw new Error('Storage unavailable'); });
        resolveResponse(jsonResponse(operationStatus('succeeded')));
        await pending;

        expect(onStatus).toHaveBeenCalledExactlyOnceWith(operationStatus('succeeded'));
        expect(onMessage).not.toHaveBeenCalled();
        expect(fetchMock).toHaveBeenCalledTimes(1);
        expect(vi.getTimerCount()).toBe(0);
    });

    it('keeps polling without storage and does not promise recovery through reload', async () => {
        vi.spyOn(storage, 'getItem').mockImplementation(() => { throw new Error('Storage unavailable'); });
        vi.spyOn(storage, 'setItem').mockImplementation(() => { throw new Error('Storage unavailable'); });
        const fetchMock = installFetchMock([
            { match: '/api/autopush/status?operationId=42', respond: jsonResponse(operationStatus()) }
        ]);
        tracker.accepted({ operationId: 42, state: 'pending_normal_boot' });
        await vi.advanceTimersByTimeAsync(750);
        expect(onStatus).toHaveBeenLastCalledWith(operationStatus());
        await vi.advanceTimersByTimeAsync(5 * 60 * 1000);

        expect(onMessage).toHaveBeenCalledExactlyOnceWith({
            type: 'warning', text: 'Detector operation status polling paused after five minutes.'
        });
        const pausedRequestCount = fetchMock.mock.calls.length;
        await vi.advanceTimersByTimeAsync(60_000);
        expect(fetchMock).toHaveBeenCalledTimes(pausedRequestCount);
        expect(vi.getTimerCount()).toBe(0);
    });

    it.each(['terminal status', 'identity mismatch'])(
        'preserves another operation\'s resume hint when receiving %s', async (outcome) => {
            let resolveResponse;
            installFetchMock([{
                match: '/api/autopush/status?operationId=42',
                respond: () => new Promise((resolve) => { resolveResponse = resolve; })
            }]);
            const pending = tracker.resume();
            storage.setItem(DETECTOR_OPERATION_STORAGE_KEY, '43');
            resolveResponse(outcome === 'terminal status'
                ? jsonResponse(operationStatus('succeeded'))
                : jsonResponse({ error: 'operation_mismatch' }, 409));
            await pending;

            if (outcome === 'terminal status') {
                expect(onStatus).toHaveBeenCalledExactlyOnceWith(operationStatus('succeeded'));
                expect(onMessage).not.toHaveBeenCalled();
            } else {
                expect(onStatus).toHaveBeenCalledExactlyOnceWith(null);
                expect(onMessage).toHaveBeenCalledExactlyOnceWith({
                    type: 'error',
                    text: 'Saved operation 42 does not match the detector\'s current operation. No status was reused.'
                });
            }
            expect(storage.getItem(DETECTOR_OPERATION_STORAGE_KEY)).toBe('43');
            expect(vi.getTimerCount()).toBe(0);
        }
    );

    it('stops after completion even when removing the resume hint fails', async () => {
        vi.spyOn(storage, 'removeItem').mockImplementation(() => { throw new Error('Storage unavailable'); });
        const fetchMock = installFetchMock([
            { match: '/api/autopush/status?operationId=42', respond: jsonResponse(operationStatus('succeeded')) }
        ]);
        await tracker.resume();
        await vi.advanceTimersByTimeAsync(60_000);

        expect(onStatus).toHaveBeenCalledExactlyOnceWith(operationStatus('succeeded'));
        expect(onMessage).not.toHaveBeenCalled();
        expect(fetchMock).toHaveBeenCalledTimes(1);
        expect(vi.getTimerCount()).toBe(0);
    });

    it('keeps the accepted progress when a later response regresses and stops polling', async () => {
        let state = 'recapturing';
        const fetchMock = installFetchMock([
            { match: '/api/autopush/status?operationId=42', respond: () => jsonResponse(operationStatus(state)) }
        ]);
        await tracker.resume();
        state = 'running';
        await vi.advanceTimersByTimeAsync(750);

        expect(onStatus).toHaveBeenCalledExactlyOnceWith(operationStatus('recapturing'));
        expect(onMessage).toHaveBeenCalledExactlyOnceWith({
            type: 'error', text: 'Detector operation status regressed; stale status was rejected.'
        });
        await vi.advanceTimersByTimeAsync(60_000);
        expect(fetchMock).toHaveBeenCalledTimes(2);
        expect(storage.getItem(DETECTOR_OPERATION_STORAGE_KEY)).toBe('42');
    });

    it.each(['terminal status', 'identity mismatch', 'network failure'])(
        'ignores a pending %s after the page is disposed', async (outcome) => {
            let resolveResponse;
            let rejectResponse;
            const fetchMock = installFetchMock([{
                match: '/api/autopush/status?operationId=42',
                respond: () => new Promise((resolve, reject) => {
                    resolveResponse = resolve;
                    rejectResponse = reject;
                })
            }]);
            const pending = tracker.resume();
            tracker.dispose();
            if (outcome === 'network failure') rejectResponse(new Error('Connection lost'));
            else if (outcome === 'identity mismatch') resolveResponse(jsonResponse({ error: 'operation_mismatch' }, 409));
            else resolveResponse(jsonResponse(operationStatus('succeeded')));
            await pending;
            await vi.advanceTimersByTimeAsync(60_000);

            expect(onStatus).not.toHaveBeenCalled();
            expect(onMessage).not.toHaveBeenCalled();
            expect(storage.getItem(DETECTOR_OPERATION_STORAGE_KEY)).toBe('42');
            expect(fetchMock).toHaveBeenCalledTimes(1);
            expect(vi.getTimerCount()).toBe(0);
        }
    );

    it('cancels a scheduled poll when the page is disposed', async () => {
        const fetchMock = installFetchMock([
            { match: '/api/autopush/status?operationId=42', respond: jsonResponse(operationStatus()) }
        ]);
        await tracker.resume();
        tracker.dispose();
        await vi.advanceTimersByTimeAsync(60_000);
        expect(fetchMock).toHaveBeenCalledTimes(1);
        expect(vi.getTimerCount()).toBe(0);
        expect(storage.getItem(DETECTOR_OPERATION_STORAGE_KEY)).toBe('42');
    });

    it('remembers a start accepted after navigation without updating the disposed page', () => {
        tracker.dispose();
        tracker.accepted({ operationId: 43, state: 'pending_normal_boot' });
        expect(storage.getItem(DETECTOR_OPERATION_STORAGE_KEY)).toBe('43');
        expect(onStatus).not.toHaveBeenCalled();
        expect(onMessage).not.toHaveBeenCalled();
        expect(vi.getTimerCount()).toBe(0);
    });
});

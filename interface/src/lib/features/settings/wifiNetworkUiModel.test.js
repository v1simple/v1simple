import { describe, expect, it } from 'vitest';

import {
    applyWifiScanResponse,
    beginWifiScan,
    buildWifiEditorRequest,
    closeWifiEditorState,
    closeWifiScan,
    createWifiEditorState,
    createWifiScanState,
    disposeWifiScan,
    isCurrentWifiScan,
    openWifiEditorState,
    selectWifiScanNetwork,
    setWifiScanSaving
} from './wifiNetworkUiModel.js';

describe('WiFi network UI model', () => {
    it('invalidates an overlapping scan and ignores its stale response', () => {
        const first = beginWifiScan(createWifiScanState(), 1);
        const second = beginWifiScan(first, 2);

        expect(second.runId).toBe(first.runId + 1);
        expect(isCurrentWifiScan(second, first.runId)).toBe(false);
        expect(applyWifiScanResponse(second, first.runId, { networks: [{ ssid: 'Old' }] })).toEqual({
            state: second,
            accepted: false,
            shouldPoll: false
        });
    });

    it('invalidates responses when closed or disposed', () => {
        const active = beginWifiScan(createWifiScanState());
        const closed = closeWifiScan(active);
        const disposed = disposeWifiScan(active);

        expect(closed.closed).toBe(true);
        expect(isCurrentWifiScan(closed.state, active.runId)).toBe(false);
        expect(isCurrentWifiScan(disposed, active.runId)).toBe(false);
    });

    it('retains scanning state until a successful completed response refreshes results', () => {
        const active = beginWifiScan(createWifiScanState());
        const pending = applyWifiScanResponse(active, active.runId, {
            scanning: true,
            networks: [{ ssid: 'TooEarly' }]
        });
        const completed = applyWifiScanResponse(pending.state, active.runId, {
            scanning: false,
            networks: [{ ssid: 'Fresh', secure: true }]
        });

        expect(pending).toMatchObject({ accepted: true, shouldPoll: true });
        expect(pending.state.networks).toEqual([]);
        expect(completed).toMatchObject({ accepted: true, shouldPoll: false });
        expect(completed.state.networks).toEqual([{ ssid: 'Fresh', secure: true }]);
    });

    it('resets scan selection and refuses a non-forced close while saving', () => {
        const active = beginWifiScan(createWifiScanState());
        const selected = selectWifiScanNetwork(active, { ssid: 'Garage', secure: true });
        const saving = setWifiScanSaving({ ...selected, password: 'secret' }, true);

        expect(closeWifiScan(saving)).toEqual({ state: saving, closed: false });
        expect(closeWifiScan(saving, { force: true }).state).toEqual({
            ...createWifiScanState(),
            runId: saving.runId + 1
        });
    });

    it('normalizes labels and priority while preserving the exact SSID on a label-only edit', () => {
        const editor = openWifiEditorState(
            3,
            {
                mode: 'edit',
                label: ' Phone ',
                ssid: ' Hotspot ',
                priority: 999,
                hasExistingPassword: true
            },
            4
        );

        expect(buildWifiEditorRequest(editor)).toEqual({
            index: 3,
            label: 'Phone',
            ssid: ' Hotspot ',
            priority: 255
        });
        expect(
            closeWifiEditorState(editor, { actionInFlight: true })
        ).toEqual({ state: editor, closed: false });
        expect(closeWifiEditorState(editor, { force: true })).toEqual({
            state: createWifiEditorState(),
            closed: true
        });
    });

    it('preserves whitespace-only SSIDs and rejects only an empty SSID', () => {
        const editor = openWifiEditorState(null, { ssid: '   ' });
        expect(buildWifiEditorRequest(editor).ssid).toBe('   ');
        expect(buildWifiEditorRequest({ ...editor, ssid: '' })).toBeNull();
    });
});

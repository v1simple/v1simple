import { fireEvent, render, screen, within } from '@testing-library/svelte';
import { describe, expect, it } from 'vitest';
import ProfileOperationStatus from './ProfileOperationStatus.svelte';
import { DETECTOR_OPERATION_COMPONENTS, validDetectorOperationStatus } from './detectorOperation';

function operationStatus(overrides = {}, requested = {}) {
    return {
        operationId: 42,
        kind: 'apply_profile',
        source: 'maintenance_ui',
        profileName: 'Daily Drive',
        targetAddress: 'AA:BB:CC:DD:EE:FF',
        returnToMaintenance: true,
        state: 'succeeded',
        result: 'succeeded',
        reason: 'none',
        terminal: true,
        components: Object.fromEntries(DETECTOR_OPERATION_COMPONENTS.map((name) => [name, {
            requested: false, sent: false, verified: false,
            outcome: 'not_requested', reason: 'none',
            ...requested[name]
        }])),
        ...overrides
    };
}

describe('ProfileOperationStatus', () => {
    it.each([0, 1, 2])('shows recorded slot %i as a one-based number without a profile name', (slot) => {
        const status = operationStatus({ kind: 'apply_slot', slot, profileName: null });
        expect(validDetectorOperationStatus(status, 42)).toBe(true);
        const { unmount } = render(ProfileOperationStatus, { operationStatus: status });

        expect(screen.getByText(`Slot ${slot + 1}`)).toBeInTheDocument();
        expect(screen.queryByText(/^Profile:/)).not.toBeInTheDocument();
        unmount();
    });

    it('shows verified success without implying already-matched settings were sent', async () => {
        const status = operationStatus({}, {
            display: { requested: true, verified: true, outcome: 'unchanged' },
            volume: { requested: true, sent: true, verified: true, outcome: 'verified' }
        });
        expect(validDetectorOperationStatus(status, 42)).toBe(true);
        const { unmount } = render(ProfileOperationStatus, { operationStatus: status });

        expect(screen.getByRole('heading', { name: 'Applied and verified' })).toBeInTheDocument();
        const display = screen.getByText('Detector display').closest('.surface-panel');
        expect(within(display).getByText('Already matched')).toBeInTheDocument();
        expect(within(display).getByText('Sent: No · Verified: Yes')).toBeInTheDocument();
        const volume = screen.getByText('Detector volume').closest('.surface-panel');
        expect(within(volume).getByText('Sent: Yes · Verified: Yes')).toBeInTheDocument();
        expect(screen.queryByText('Custom frequency ranges')).not.toBeInTheDocument();

        const details = screen.getByText('Operation details').closest('details');
        expect(details).not.toHaveAttribute('open');
        await fireEvent.click(screen.getByText('Operation details'));
        expect(details).toHaveAttribute('open');
        expect(within(details).getByText('Detector operation 42')).toBeInTheDocument();
        expect(within(details).getByText('apply_profile · target AA:BB:CC:DD:EE:FF · source maintenance_ui'))
            .toBeInTheDocument();
        unmount();
    });

    it('keeps partial application and unverified commands visible beside confirmed settings', () => {
        const status = operationStatus({ state: 'partial', result: 'partial', reason: 'recapture_timed_out' }, {
            display: { requested: true, sent: true, verified: true, outcome: 'verified' },
            volume: { requested: true, sent: true, outcome: 'timeout', reason: 'volume_timeout' }
        });
        expect(validDetectorOperationStatus(status, 42)).toBe(true);
        const { unmount } = render(ProfileOperationStatus, { operationStatus: status });

        expect(screen.getByRole('heading', { name: 'Apply not fully verified' })).toBeInTheDocument();
        expect(screen.queryByText('Applied and verified')).not.toBeInTheDocument();
        expect(screen.getByText('Reading the detector settings after the operation timed out.')).toBeInTheDocument();
        const volume = screen.getByText('Detector volume').closest('.surface-panel');
        expect(within(volume).getByText('Timed out')).toBeInTheDocument();
        expect(within(volume).getByText('Confirmation of the volume setting timed out.')).toBeInTheDocument();
        expect(within(volume).getByText('Sent: Yes · Verified: No')).toBeInTheDocument();
        const display = screen.getByText('Detector display').closest('.surface-panel');
        expect(within(display).getByText('Sent: Yes · Verified: Yes')).toBeInTheDocument();
        unmount();
    });

    it('does not describe a sent factory reset as a fully verified reset', () => {
        const status = operationStatus({
            kind: 'factory_reset', profileName: undefined,
            state: 'partial', result: 'partial', reason: 'factory_reset_scope_unverified'
        }, {
            factoryReset: { requested: true, sent: true, outcome: 'sent' }
        });
        expect(validDetectorOperationStatus(status, 42)).toBe(true);
        const { unmount } = render(ProfileOperationStatus, { operationStatus: status });

        expect(screen.getByRole('heading', { name: 'Reset not fully verified' })).toBeInTheDocument();
        expect(screen.getByText('The reset command was sent, but the detector cannot confirm every factory default.'))
            .toBeInTheDocument();
        const reset = screen.getByText('Factory reset command').closest('.surface-panel');
        expect(within(reset).getByText('Sent, not verified')).toBeInTheDocument();
        expect(within(reset).getByText('Sent: Yes · Verified: No')).toBeInTheDocument();
        expect(screen.queryByText('Applied and verified')).not.toBeInTheDocument();
        unmount();
    });

    it('does not imply settings changed when an interrupted operation has no confirmed commands', () => {
        const status = operationStatus({ state: 'partial', result: 'partial', reason: 'interrupted' });
        expect(validDetectorOperationStatus(status, 42)).toBe(true);
        const { unmount } = render(ProfileOperationStatus, { operationStatus: status });

        expect(screen.getByRole('heading', { name: 'Apply not fully verified' })).toBeInTheDocument();
        expect(screen.getByText('The operation was interrupted before it finished.')).toBeInTheDocument();
        expect(screen.queryByText('Applied and verified')).not.toBeInTheDocument();
        expect(screen.queryByText('Partly applied')).not.toBeInTheDocument();
        expect(screen.queryByText(/Sent: Yes/)).not.toBeInTheDocument();
        unmount();
    });

    it('shows a failed operation even when a command was sent to the detector', () => {
        const status = operationStatus({ state: 'failed', result: 'failed', reason: 'detector_timeout' }, {
            userSettings: { requested: true, sent: true, outcome: 'timeout', reason: 'user_bytes_timeout' }
        });
        expect(validDetectorOperationStatus(status, 42)).toBe(true);
        const { unmount } = render(ProfileOperationStatus, { operationStatus: status });

        expect(screen.getByRole('heading', { name: 'Apply failed' })).toBeInTheDocument();
        expect(screen.getByText('The detector operation timed out.')).toBeInTheDocument();
        const settings = screen.getByText('Detection settings').closest('.surface-panel');
        expect(within(settings).getByText('Sent: Yes · Verified: No')).toBeInTheDocument();
        expect(screen.queryByText('Applied and verified')).not.toBeInTheDocument();
        unmount();
    });

    it('labels a running factory reset distinctly from applying a profile', async () => {
        const status = operationStatus({ state: 'running', result: 'in_progress', terminal: false });
        const { rerender, unmount } = render(ProfileOperationStatus, { operationStatus: status });
        expect(screen.getByRole('heading', { name: 'Applying profile' })).toBeInTheDocument();
        await rerender({ operationStatus: {
            ...status, kind: 'factory_reset', profileName: undefined,
            components: { ...status.components, factoryReset: {
                requested: true, sent: false, verified: false, outcome: 'pending', reason: 'none'
            } }
        } });
        expect(screen.getByRole('heading', { name: 'Resetting detector' })).toBeInTheDocument();
        expect(screen.queryByText('Applying profile')).not.toBeInTheDocument();
        unmount();
    });
});

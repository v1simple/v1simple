import { fireEvent, render, screen } from '@testing-library/svelte';
import { afterEach, describe, expect, it, vi } from 'vitest';

import ColorPickerModal from './ColorPickerModal.svelte';
import ProfileSaveDialog from '#lib/features/profiles/ProfileSaveDialog.svelte';
import SettingsWifiModal from '#lib/features/settings/SettingsWifiModal.svelte';

function cancel(dialog) {
    return fireEvent(dialog, new Event('cancel', { cancelable: true }));
}

describe('native dialog ownership', () => {
    afterEach(() => vi.restoreAllMocks());

    it('opens the named color dialog natively and routes cancel without applying drafts', async () => {
        const show = vi.spyOn(HTMLDialogElement.prototype, 'showModal');
        const close = vi.spyOn(HTMLDialogElement.prototype, 'close');
        const oncancel = vi.fn();
        const onapply = vi.fn();
        const view = render(ColorPickerModal, {
            open: true, label: 'Band Ka Color', red: 8, green: 16, blue: 24, oncancel, onapply
        });
        const dialog = screen.getByRole('dialog', { name: 'Band Ka Color' });
        expect(show).toHaveBeenCalledTimes(1);
        expect(dialog).toHaveAttribute('open');
        await fireEvent.input(screen.getByLabelText(/red/i), { target: { value: '248' } });
        expect(await cancel(dialog)).toBe(false);
        expect(oncancel).toHaveBeenCalledTimes(1);
        expect(onapply).not.toHaveBeenCalled();
        view.unmount();
        expect(close).toHaveBeenCalledTimes(1);
    });

    it('keeps profile cancellation available while saving', async () => {
        const oncancel = vi.fn();
        const onsave = vi.fn();
        render(ProfileSaveDialog, {
            open: true, saveName: 'Highway', saveDescription: 'Draft',
            savingProfile: 'Highway', mutationBusy: true, oncancel, onsave
        });
        const dialog = screen.getByRole('dialog', { name: 'Save Profile' });
        expect(screen.getByRole('button', { name: 'Cancel' })).toBeEnabled();
        expect(screen.getByRole('button', { name: 'Saving Highway...' })).toBeDisabled();
        expect(await cancel(dialog)).toBe(false);
        expect(oncancel).toHaveBeenCalledTimes(1);
        expect(onsave).not.toHaveBeenCalled();
        expect(screen.getByLabelText('Profile Name')).toHaveValue('Highway');
    });

    it('moves WiFi focus when scanning and network selection replace the view', async () => {
        const network = { ssid: 'Test network', secure: true, rssi: -40 };
        const props = {
            open: true, wifiScanning: true, wifiNetworks: [network],
            oncloseWifiModal: vi.fn(), onselectNetwork: vi.fn()
        };
        const view = render(SettingsWifiModal, props);
        await view.rerender({ ...props, wifiScanning: false });
        expect(screen.getByRole('button', { name: /Test network/ })).toHaveFocus();
        await view.rerender({ ...props, wifiScanning: false, selectedNetwork: network });
        expect(screen.getByLabelText('Password')).toHaveFocus();
        await view.rerender({ ...props, wifiScanning: false, selectedNetwork: { ...network, secure: false } });
        expect(screen.getByRole('button', { name: 'Back' })).toHaveFocus();
        await fireEvent.click(screen.getByRole('button', { name: 'Back' }));
        expect(screen.getByRole('button', { name: /Test network/ })).toHaveFocus();
    });

    it.each([false, true])('preserves WiFi close policy with connecting=%s', async (wifiConnecting) => {
        const oncloseWifiModal = vi.fn();
        const onconnectToNetwork = vi.fn();
        render(SettingsWifiModal, {
            open: true, wifiConnecting, selectedNetwork: { ssid: 'Test network', secure: true },
            wifiPassword: 'draft-password', oncloseWifiModal, onconnectToNetwork
        });
        const dialog = screen.getByRole('dialog', { name: 'Select WiFi Network' });
        expect(await cancel(dialog)).toBe(false);
        expect(oncloseWifiModal).toHaveBeenCalledTimes(wifiConnecting ? 0 : 1);
        await fireEvent.click(screen.getByRole('button', { name: 'Close WiFi picker' }));
        expect(oncloseWifiModal).toHaveBeenCalledTimes(wifiConnecting ? 0 : 2);
        expect(screen.getByLabelText('Password')).toHaveValue('draft-password');
        expect(onconnectToNetwork).not.toHaveBeenCalled();
    });
});

import { fireEvent, render, screen, waitFor } from '@testing-library/svelte';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

import { cloneDefaultColors, COLOR_PARAM_ORDER } from '$lib/utils/colors';
import { installFetchMock, installFixtureFetchMock, jsonResponse } from '../../test/fetch-mock.js';
import Page from './+page.svelte';

const DISPLAY_SETTINGS_ENDPOINT = '/api/display/settings';
const DISPLAY_SETTINGS_RESET_ENDPOINT = '/api/display/settings/reset';
const DISPLAY_PREVIEW_ENDPOINT = '/api/display/preview';
const DISPLAY_PREVIEW_CLEAR_ENDPOINT = '/api/display/preview/clear';
const QUIET_SETTINGS_ENDPOINT = '/api/quiet/settings';

function installDefaultFetch(overrides = []) {
    return installFixtureFetchMock(['display_colors_success', 'quiet_settings_success'], overrides);
}

describe('colors route page', () => {
    beforeEach(() => {
        global.confirm = vi.fn(() => true);
    });

    afterEach(() => {
        vi.useRealTimers();
        vi.restoreAllMocks();
    });

    it('surfaces a synthetic rate-limited display preview failure', async () => {
        installDefaultFetch([
            {
                method: 'POST',
                match: DISPLAY_PREVIEW_ENDPOINT,
                respond: jsonResponse({ success: false, message: 'Too many requests' }, 429)
            }
        ]);
        const { unmount } = render(Page);

        await screen.findByRole('button', { name: 'Preview' });
        await fireEvent.click(screen.getByRole('button', { name: 'Preview' }));

        await screen.findByText('Failed to preview colors');
        unmount();
    });

    it('opens the bar editor in advanced mode for the stepped default theme', async () => {
        installDefaultFetch();
        const { unmount } = render(Page);

        // Factory default (G,G,Y,Y,R,R) is not a 3-stop ramp → advanced pickers.
        await fireEvent.click(await screen.findByRole('button', { name: 'Alert colors' }));
        expect(await screen.findByText('Segment 1')).toBeVisible();
        expect(screen.queryByText('Bottom (weakest)')).not.toBeInTheDocument();

        unmount();
    });

    it('opens the bar editor in simple mode when the saved theme is a 3-stop ramp', async () => {
        const { deriveBarsFromStops } = await import('$lib/utils/colors');
        installDefaultFetch([
            {
                method: 'GET',
                match: DISPLAY_SETTINGS_ENDPOINT,
                respond: jsonResponse({
                    ...cloneDefaultColors(),
                    ...deriveBarsFromStops(0x07e0, 0xffe0, 0xf800)
                })
            }
        ]);
        const { unmount } = render(Page);

        await fireEvent.click(await screen.findByRole('button', { name: 'Alert colors' }));
        expect(await screen.findByText('Bottom (weakest)')).toBeVisible();
        expect(screen.queryByText('Segment 1')).not.toBeInTheDocument();

        unmount();
    });

    it('switching to simple mode does not convert colors until a stop is edited', async () => {
        const fetchMock = installDefaultFetch();
        const { unmount } = render(Page);

        await fireEvent.click(await screen.findByRole('button', { name: 'Alert colors' }));
        await screen.findByText('Segment 1');
        await fireEvent.click(screen.getByRole('button', { name: /use simple 3-stop gradient/i }));
        await fireEvent.click(await screen.findByRole('button', { name: 'Alert colors' }));
        expect(await screen.findByText('Bottom (weakest)')).toBeVisible();

        // Save without editing a stop: the stepped default must round-trip intact.
        await fireEvent.click(await screen.findByRole('button', { name: /save display settings/i }));
        await waitFor(() => {
            const saveCall = fetchMock.mock.calls.find(
                ([url, init]) => url === DISPLAY_SETTINGS_ENDPOINT && init?.method === 'POST'
            );
            expect(saveCall?.[1]?.body?.get('bar2')).toBe(String(cloneDefaultColors().bar2));
        });

        // Mode choice survives the post-save refetch (findBy waits out the
        // loading state the refetch puts the page into).
        await fireEvent.click(await screen.findByRole('button', { name: 'Alert colors' }));
        expect(await screen.findByText('Bottom (weakest)')).toBeVisible();

        unmount();
    });

    it('loads colors on mount', async () => {
        const fetchMock = installDefaultFetch();
        const { unmount } = render(Page);

        await screen.findByText('OBD Badge');
        await waitFor(() => {
            expect(fetchMock.mock.calls.some(([url]) => url === DISPLAY_SETTINGS_ENDPOINT)).toBe(
                true
            );
        });
        expect(screen.getByText('48%')).toBeInTheDocument();

        unmount();
    });

    it('keeps edits across categories and saves all 39 display fields from the single action bar', async () => {
        const savedColors = cloneDefaultColors();
        const fetchMock = installDefaultFetch([
            { method: 'GET', match: DISPLAY_SETTINGS_ENDPOINT, respond: jsonResponse(savedColors) }
        ]);
        const { container, unmount } = render(Page);
        const brightness = await screen.findByRole('slider', { name: 'Screen brightness' });
        expect(screen.getByRole('button', { name: 'Screen' })).toHaveAttribute('aria-pressed', 'true');
        expect(container.querySelector('section[aria-label="Status settings"]')).not.toBeVisible();
        await fireEvent.input(brightness, { target: { value: '100' } });

        await fireEvent.click(screen.getByRole('button', { name: 'Status' }));
        expect(brightness).not.toBeVisible();
        await fireEvent.click(screen.getByRole('checkbox', { name: /^Show WiFi status/ }));
        await fireEvent.click(screen.getByRole('button', { name: 'Alert colors' }));
        const bogey = screen.getByRole('button', { name: 'Bogey counter color' })
            .closest('.field-control').querySelector('input[type="text"]');
        await fireEvent.change(bogey, { target: { value: '07E0' } });

        await fireEvent.click(screen.getByRole('button', { name: 'Integrations' }));
        const obd = screen.getByRole('button', { name: 'OBD Badge color' })
            .closest('.field-control').querySelector('input[type="text"]');
        await fireEvent.change(obd, { target: { value: 'F800' } });
        await fireEvent.click(screen.getByRole('button', { name: 'Show all settings' }));
        for (const section of container.querySelectorAll('section[aria-label]')) {
            expect(section).toBeVisible();
        }
        expect(screen.getByRole('slider', { name: 'Screen brightness' })).toBe(brightness);
        expect(brightness).toHaveValue('100');
        expect(bogey).toHaveValue('07E0');
        expect(obd).toHaveValue('F800');

        await fireEvent.click(screen.getByRole('button', { name: 'Screen' }));
        expect(screen.getAllByRole('button', { name: 'Save display settings' })).toHaveLength(1);
        await fireEvent.click(screen.getByRole('button', { name: 'Save display settings' }));
        await waitFor(() => {
            const saved = fetchMock.mock.calls.find(
                ([url, init]) => url === DISPLAY_SETTINGS_ENDPOINT && init?.method === 'POST'
            )?.[1].body;
            expect(saved).toBeDefined();
            expect([...saved.keys()].sort()).toEqual([...COLOR_PARAM_ORDER].sort());
            expect([...saved.keys()]).toHaveLength(39);
            const expected = { ...savedColors, brightness: 100, hideWifiIcon: true, bogey: 2016, obd: 63488 };
            for (const key of COLOR_PARAM_ORDER) expect(saved.get(key)).toBe(String(expected[key]));
        });
        unmount();
    });

    it.each([false, true])('keeps visibility save flags intact with positive controls (hidden=%s)', async (hidden) => {
        const visibilityFields = [
            ['hideWifiIcon', /^Show WiFi status/],
            ['hideProfileIndicator', /^Show active slot/],
            ['hideBatteryIcon', /^Show battery level/],
            ['hideBleIcon', /^Show Bluetooth proxy status/],
            ['hideVolumeIndicator', /^Show V1 volume/],
            ['hideRssiIndicator', /^Show connection strength/]
        ];
        const savedColors = {
            ...cloneDefaultColors(),
            ...Object.fromEntries(visibilityFields.map(([key]) => [key, hidden])),
            showBatteryPercent: true
        };
        const fetchMock = installDefaultFetch([
            { method: 'GET', match: DISPLAY_SETTINGS_ENDPOINT, respond: jsonResponse(savedColors) }
        ]);
        const { unmount } = render(Page);

        await fireEvent.click(await screen.findByRole('button', { name: 'Status' }));
        for (const [, name] of visibilityFields) {
            const control = await screen.findByRole('checkbox', { name });
            expect(control.checked).toBe(!hidden);
            await fireEvent.click(control);
            expect(control.checked).toBe(hidden);
        }
        const percentage = screen.getByRole('checkbox', { name: /^Show battery percentage/ });
        expect(percentage).toBeChecked();
        expect(percentage.disabled).toBe(!hidden);

        await fireEvent.click(screen.getByRole('button', { name: /save display settings/i }));
        await waitFor(() => {
            const saveCall = fetchMock.mock.calls.find(
                ([url, init]) => url === DISPLAY_SETTINGS_ENDPOINT && init?.method === 'POST'
            );
            expect(saveCall).toBeDefined();
            for (const [key] of visibilityFields) {
                expect(saveCall[1].body.get(key)).toBe(String(!hidden));
            }
            expect(saveCall[1].body.get('showBatteryPercent')).toBe('true');
            expect(saveCall[1].body.get('brightness')).toBe(String(savedColors.brightness));
            expect(saveCall[1].body.get('bogey')).toBe(String(savedColors.bogey));
        });

        unmount();
    });

    it('shows load error when the colors request fails', async () => {
        installFetchMock(
            [
                {
                    method: 'GET',
                    match: DISPLAY_SETTINGS_ENDPOINT,
                    respond: () => Promise.reject(new Error('offline'))
                }
            ],
            jsonResponse({})
        );
        const { unmount } = render(Page);

        await screen.findByText('Failed to load colors');
        unmount();
    });

    it.each(['http', 'network'])(
        'prevents default-color saves after an initial %s failure and preserves colors on retry',
        async (failure) => {
            let displayReads = 0;
            const savedColors = { ...cloneDefaultColors(), bogey: 2016, brightness: 70 };
            const fetchMock = installDefaultFetch([
                {
                    method: 'GET',
                    match: DISPLAY_SETTINGS_ENDPOINT,
                    respond: () => {
                        if (displayReads++ === 0) {
                            if (failure === 'network') throw new Error('offline');
                            return jsonResponse({}, 503);
                        }
                        return jsonResponse(savedColors);
                    }
                }
            ]);
            const { unmount } = render(Page);

            await screen.findByText('Failed to load colors');
            expect(screen.queryByRole('button', { name: /save display settings/i })).not.toBeInTheDocument();
            expect(screen.queryByRole('slider')).not.toBeInTheDocument();
            expect(
                fetchMock.mock.calls.some(
                    ([url, init]) => url === DISPLAY_SETTINGS_ENDPOINT && init?.method === 'POST'
                )
            ).toBe(false);

            await fireEvent.click(screen.getByRole('button', { name: 'Retry' }));
            const saveButton = await screen.findByRole('button', { name: /save display settings/i });
            await fireEvent.input(screen.getByRole('slider'), { target: { value: '100' } });
            await fireEvent.click(saveButton);
            await screen.findByText('Colors saved! Previewing on display...');
            const saveCall = fetchMock.mock.calls.find(
                ([url, init]) => url === DISPLAY_SETTINGS_ENDPOINT && init?.method === 'POST'
            );
            expect(saveCall[1].body.get('brightness')).toBe('100');
            expect(saveCall[1].body.get('bogey')).toBe('2016');
            unmount();
        }
    );

    it('disables battery percentage when the battery icon is hidden', async () => {
        installDefaultFetch([
            {
                method: 'GET',
                match: DISPLAY_SETTINGS_ENDPOINT,
                respond: jsonResponse({
                    ...cloneDefaultColors(),
                    hideBatteryIcon: true,
                    showBatteryPercent: true
                })
            }
        ]);
        const { unmount } = render(Page);

        await fireEvent.click(await screen.findByRole('button', { name: 'Status' }));
        expect(
            await screen.findByRole('checkbox', { name: /show battery percentage/i })
        ).toBeDisabled();

        unmount();
    });

    it('lets firmware expire the save preview without cancelling a newer manual preview', async () => {
        vi.useFakeTimers();
        const fetchMock = installDefaultFetch();
        const { unmount } = render(Page);

        expect(await screen.findByText('OBD Badge')).toBeInTheDocument();

        const saveButton = await screen.findByRole('button', { name: /save display settings/i });
        await fireEvent.click(saveButton);

        await screen.findByText('Colors saved! Previewing on display...');
        const saveCall = fetchMock.mock.calls.find(
            ([url, init]) => url === DISPLAY_SETTINGS_ENDPOINT && init?.method === 'POST'
        );
        expect(saveCall?.[1]?.body?.get('obd')).toBe(String(cloneDefaultColors().obd));
        // The save hold has ended on the device. A new Preview owns its own
        // lifetime and must survive the old browser's six-second deadline.
        await vi.advanceTimersByTimeAsync(5900);
        await fireEvent.click(screen.getByRole('button', { name: 'Preview' }));
        expect(
            fetchMock.mock.calls.some(
                ([url, init]) => url === DISPLAY_PREVIEW_ENDPOINT && init?.method === 'POST'
            )
        ).toBe(true);
        await vi.advanceTimersByTimeAsync(100);
        expect(
            fetchMock.mock.calls.some(
                ([url, init]) => url === DISPLAY_PREVIEW_CLEAR_ENDPOINT && init?.method === 'POST'
            )
        ).toBe(false);

        unmount();
        await vi.advanceTimersByTimeAsync(6000);
        expect(
            fetchMock.mock.calls.some(
                ([url, init]) => url === DISPLAY_PREVIEW_CLEAR_ENDPOINT && init?.method === 'POST'
            )
        ).toBe(false);
    });

    it('runs preview and reset actions', async () => {
        const fetchMock = installDefaultFetch();
        const { unmount } = render(Page);

        await fireEvent.click(await screen.findByRole('button', { name: /^preview$/i }));
        await fireEvent.click(screen.getByRole('button', { name: /reset defaults/i }));

        await screen.findByText('Colors reset to defaults!');
        expect(
            fetchMock.mock.calls.some(
                ([url, init]) => url === DISPLAY_PREVIEW_ENDPOINT && init?.method === 'POST'
            )
        ).toBe(true);
        expect(
            fetchMock.mock.calls.some(
                ([url, init]) => url === DISPLAY_SETTINGS_RESET_ENDPOINT && init?.method === 'POST'
            )
        ).toBe(true);

        unmount();
    });

    it('reverts stealth mode when the quiet save fails', async () => {
        installDefaultFetch([
            {
                method: 'POST',
                match: QUIET_SETTINGS_ENDPOINT,
                respond: jsonResponse({ success: false }, 500)
            }
        ]);
        const { unmount } = render(Page);

        const stealthToggle = await screen.findByRole('checkbox', { name: /quiet screen between alerts/i });
        expect(stealthToggle).not.toBeChecked();

        await fireEvent.click(stealthToggle);

        await screen.findByText('Failed to save stealth setting');
        await waitFor(() => expect(stealthToggle).not.toBeChecked());

        unmount();
    });
});

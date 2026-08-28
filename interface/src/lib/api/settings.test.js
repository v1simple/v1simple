import { afterEach, describe, expect, it, vi } from 'vitest';

import { URLENCODED_FORM_CONTENT_TYPE } from '$lib/utils/poll';
import { postSettingsForm } from './settings.js';

describe('postSettingsForm', () => {
    afterEach(() => {
        vi.restoreAllMocks();
    });

    it('posts ordered duplicate fields as URL-encoded data, never multipart', async () => {
        const fetchSpy = vi.fn(async () => new Response('{}', { status: 200 }));
        global.fetch = fetchSpy;
        const formData = new FormData();
        formData.append('profile', '0');
        formData.append('profile', '2');
        formData.append('label', 'Garage V1');

        await postSettingsForm(formData, '/api/device/settings');

        const [url, init] = fetchSpy.mock.calls[0];
        expect(url).toBe('/api/device/settings');
        expect(init.body).toBeInstanceOf(URLSearchParams);
        expect(Array.from(init.body.entries())).toEqual([
            ['profile', '0'],
            ['profile', '2'],
            ['label', 'Garage V1']
        ]);
        expect(init.headers['Content-Type']).toBe(URLENCODED_FORM_CONTENT_TYPE);
        expect(JSON.stringify(init.headers)).not.toContain('multipart/form-data');
    });
});

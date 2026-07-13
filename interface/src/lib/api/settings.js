import { fetchWithTimeout } from '$lib/utils/poll';

export async function postSettingsForm(formData, endpoint) {
	if (!endpoint) {
		throw new Error('postSettingsForm requires an explicit endpoint');
	}

	return fetchWithTimeout(endpoint, {
		method: 'POST',
		body: formData
	});
}

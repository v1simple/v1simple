import { dev } from '$app/env';
import { error } from '@sveltejs/kit';

export const prerender = false;

export function load() {
    if (!dev) {
        throw error(404, 'Not found');
    }
}

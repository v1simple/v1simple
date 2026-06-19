import adapter from '@sveltejs/adapter-static';
import { vitePreprocess } from '@sveltejs/vite-plugin-svelte';

/** @type {import('@sveltejs/kit').Config} */
const config = {
	preprocess: vitePreprocess(),
	kit: {
		adapter: adapter({
			pages: 'build',
			assets: 'build',
			fallback: 'index.html',  // SPA fallback
			precompress: false,      // We'll gzip separately
			strict: true
		}),
		paths: {
			base: '',  // No base path needed for ESP32
			relative: false
		}
	}
};

export default config;

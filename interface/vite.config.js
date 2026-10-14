import tailwindcss from '@tailwindcss/vite';
import adapter from '@sveltejs/adapter-static';
import { sveltekit } from '@sveltejs/kit/vite';
import { vitePreprocess } from '@sveltejs/vite-plugin-svelte';
import { svelteTesting } from '@testing-library/svelte/vite';
import { defineConfig } from 'vitest/config';

export default defineConfig({
    plugins: [
        tailwindcss(),
        sveltekit({
            preprocess: vitePreprocess(),
            adapter: adapter({
                pages: 'build',
                assets: 'build',
                fallback: 'index.html', // SPA fallback
                precompress: false, // We'll gzip separately
                strict: true
            }),
            paths: {
                base: '', // No base path needed for ESP32
                relative: false
            },
            version: {
                pollInterval: 0 // Preserve the absence of interval polling.
            }
        }),
        process.env.VITEST ? svelteTesting() : null
    ].filter(Boolean),
    build: {
        // Keep release assets compact for the ESP32 LittleFS budget.
        minify: 'oxc',
        cssMinify: 'lightningcss',
        rolldownOptions: {
            output: {
                codeSplitting: {
                    groups: [
                        {
                            // Avoid a separate LittleFS file for each small shared control.
                            name: 'shared-controls',
                            test: /src[\\/]lib[\\/]components[\\/](PageHeader|StatusAlert|ToggleSetting|CardSectionHead)\.svelte$/,
                            minShareCount: 2
                        },
                        {
                            // The root layout already loads both stores and utils/poll.js.
                            // Requests and timers still start only when retain* is called.
                            name: 'shared-device-state',
                            test: /src[\\/]lib[\\/]stores[\\/](deviceSettings|runtimeStatus)\.svelte\.js$/,
                            minShareCount: 2
                        }
                    ]
                }
            }
        }
    },
    test: {
        environment: 'jsdom',
        setupFiles: ['./src/test/setup.js'],
        include: ['src/**/*.{test,spec}.{js,ts}']
    }
});

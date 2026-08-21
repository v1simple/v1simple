import tailwindcss from '@tailwindcss/vite';
import { sveltekit } from '@sveltejs/kit/vite';
import { svelteTesting } from '@testing-library/svelte/vite';
import { defineConfig } from 'vitest/config';

export default defineConfig({
    plugins: [tailwindcss(), sveltekit(), process.env.VITEST ? svelteTesting() : null].filter(
        Boolean
    ),
    build: {
        // Keep release assets compact for the ESP32 LittleFS budget.
        minify: 'oxc',
        cssMinify: 'lightningcss'
    },
    test: {
        environment: 'jsdom',
        setupFiles: ['./src/test/setup.js'],
        include: ['src/**/*.{test,spec}.{js,ts}']
    }
});

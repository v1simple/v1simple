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
        include: ['src/**/*.{test,spec}.{js,ts}'],
        coverage: {
            provider: 'v8',
            reporter: ['text-summary', 'lcov', 'html'],
            reportsDirectory: './coverage',
            all: true,
            include: ['src/**/*.{js,ts,svelte}'],
            exclude: ['src/**/*.test.{js,ts}', 'src/**/*.spec.{js,ts}', 'src/test/**'],
            thresholds: {
                lines: 70,
                branches: 50,
                functions: 70,
                statements: 70
            }
        }
    }
});

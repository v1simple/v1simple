import js from '@eslint/js';
import svelte from 'eslint-plugin-svelte';
import prettier from 'eslint-config-prettier';
import globals from 'globals';
import svelteConfig from './svelte.config.js';

/** Globals injected by Vitest when tests opt into the global API. */
const vitestGlobals = {
    suite: 'readonly',
    test: 'readonly',
    describe: 'readonly',
    it: 'readonly',
    expect: 'readonly',
    assert: 'readonly',
    vi: 'readonly',
    beforeAll: 'readonly',
    afterAll: 'readonly',
    beforeEach: 'readonly',
    afterEach: 'readonly'
};

export default [
    {
        ignores: ['.svelte-kit/', 'build/', 'coverage/', 'node_modules/']
    },

    js.configs.recommended,
    ...svelte.configs.recommended,

    {
        rules: {
            // The codebase consistently uses `catch (e) { showUserFacingError() }`
            // without inspecting the error object. Flagging every one of those
            // (51 sites) would force a source rewrite for zero behavior change,
            // so caught-error bindings are exempt. Unused locals/args are still
            // errors; prefix an intentionally unused arg with `_` to opt out.
            'no-unused-vars': [
                'error',
                {
                    args: 'after-used',
                    argsIgnorePattern: '^_',
                    varsIgnorePattern: '^_',
                    caughtErrors: 'none'
                }
            ],

            // Off: adding keys to the 18 existing `{#each}` blocks changes DOM
            // identity/reuse semantics, which is a behavior change and out of
            // scope for a formatting pass. Worth revisiting on its own.
            'svelte/require-each-key': 'off',

            // Off: SvelteKit's resolve() guards against a non-empty `base` path.
            // This app ships as a static SPA served from the ESP32 root with
            // `paths.base: ''` (see svelte.config.js), so plain hrefs are correct.
            'svelte/no-navigation-without-resolve': 'off',

            // Off: the two flagged `new URLSearchParams()` uses are throwaway
            // request-body builders inside async save handlers, not reactive
            // state. SvelteURLSearchParams would add reactivity nothing reads.
            'svelte/prefer-svelte-reactivity': 'off'
        }
    },

    // Application + library code runs in the browser.
    {
        files: ['src/**/*.js', 'src/**/*.svelte', 'src/**/*.svelte.js'],
        languageOptions: {
            ecmaVersion: 2023,
            sourceType: 'module',
            globals: {
                ...globals.browser
            }
        }
    },

    // Svelte 5 / runes: hand the plugin the project's svelte.config.js.
    {
        files: ['**/*.svelte', '**/*.svelte.js'],
        languageOptions: {
            parserOptions: {
                svelteConfig
            }
        }
    },

    // Build tooling, config files and deploy scripts run in Node.
    {
        files: ['*.js', 'scripts/**/*.js'],
        languageOptions: {
            ecmaVersion: 2023,
            sourceType: 'module',
            globals: {
                ...globals.node
            }
        }
    },

    // Test files: browser env plus the Vitest globals and Node helpers.
    {
        files: ['**/*.test.js', '**/*.spec.js', 'src/test/**/*.js'],
        languageOptions: {
            globals: {
                ...globals.browser,
                ...globals.node,
                ...vitestGlobals
            }
        }
    },

    // eslint-config-prettier must stay last so it can switch off every
    // formatting rule that Prettier already owns.
    prettier
];
